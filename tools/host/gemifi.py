#!/usr/bin/env python3
"""gemifi host tool: talk to a gemifi device over USB MIDI SysEx (protocol/sysex.md).

  gemifi.py ports                     list MIDI ports (gemifi ones are marked)
  gemifi.py hello                     print HELLO from the device (app or bootloader)
  gemifi.py log [--names]             print LOG / EVENT / PARAM_CHANGED messages live (Ctrl-C to stop)
  gemifi.py reboot app|boot           ask the device to reboot into the app or the bootloader
  gemifi.py flash FILE.gfw            upload firmware through the bootloader (NUC1xx)
  gemifi.py flash FILE.bin --board 0x3F00 --version 1.2.3
                                      upload to an A/B device (ESP32/ESP8266): the app takes it itself
  gemifi.py flash FILE --dry-run      check the image and print the upload plan; no MIDI
  gemifi.py flash FILE.gfw            Code 61 (board 0x0002): through its gemifi bootloader, or through M-Audio's
                                      own (maudio_update.py) on a unit that still has it (--via picks)
  gemifi.py restore-stock FirmwareFile.hex.txt
                                      Code 61: send a vendor update file as it is (back to stock firmware)

Config (app firmware, sysex.md 4.3):
  gemifi.py schema [--json] [--raw OUT.bin]   print the device's param schema
  gemifi.py get [NAME|ID[:INDEX]]...          read values (no args: every value of every param)
  gemifi.py set NAME[:INDEX]=VALUE... [--commit]
                                      set values live; VALUE is a number, an enum option label
                                      (any case), on/off for bool, or a note name (MIDI 60 = C4)
  gemifi.py commit                    store the current values in flash
  gemifi.py factory-reset [--yes]     back to defaults and erase the stored profile
  gemifi.py backup OUT.json           save every value by name (with presets: every slot, then back)
  gemifi.py restore IN.json [--commit]

Presets (sysex.md 4.5; HELLO caps bit 5):
  gemifi.py presets                   list the slots: number, name, active (*), unsaved/stored edits, program, free
  gemifi.py preset select N|NAME [--discard]
  gemifi.py preset save [N|NAME] [--name NAME]   store the live values (save-as with N) and make it active
  gemifi.py preset reset N|NAME|all [--yes]      back to the firmware's preset (stored edits and name dropped)
  gemifi.py preset rename N|NAME NEWNAME         ("" = back to the firmware's name; a free slot becomes a preset)
  gemifi.py preset delete N|NAME [--yes]         remove a preset: the later ones move up (firmware with PRESET_DELETE)
  gemifi.py preset move N|NAME TO|up|down [--yes]  reorder: the preset goes to slot TO, the ones between shift by one
  flash takes --backup FILE / --no-backup and --keep-presets / --use-firmware-presets (see tools/README.md)

Layout and raw readings (sysex.md 4.7, 4.8; HELLO caps bits 8 and 9):
  gemifi.py layout [--json]           where the controls are (the YAML's layout:)
  gemifi.py monitor [KIND...] [--seconds S]
                                      raw readings as they change: buttons, matrix, analog, encoders, pads
                                      (pins before debounce, ADC before adc_min/adc_max, detent counts)

Host feedback (sysex.md 4.6; HELLO caps bit 6). LED, ENCODER, INDEX and DISPLAY are an index or a component id:
  gemifi.py components [--json]       list the device's components: kind, index, id, name, role, extras
  gemifi.py fb led LED LEVEL          LEVEL 0..127 (0 off, 1 blink, 2..127 on) or off/blink/on
  gemifi.py fb rgb LED R G B [--blink]
  gemifi.py fb leds FIRST LEVEL...    several LEDs from FIRST on
  gemifi.py fb value analog|encoder INDEX VALUE      VALUE 0..16383
  gemifi.py fb ring ENCODER dot|boost|wrap|spread VALUE [--center]
  gemifi.py fb text DISPLAY ROW COL TEXT
  gemifi.py fb release [all|leds|values|text]        hand the states back to the device
  fb commands are notifications: sent once, never answered, so they return at once.

Backends: python-rtmidi, then mido, then raw ALSA /dev/snd/midiC*D* (Linux, no dependencies).
Choose one with --backend. --port picks a port by substring instead of the "gemifi" name match.
--serial /dev/ttyUSB0 [--baud 115200] talks raw MIDI bytes over a serial port instead (classic ESP32 and
ESP8266 builds with GM_MIDI_SERIAL; needs pyserial). Every command works over it.
"""
import argparse
import dataclasses
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
import maudio_update as mu  # noqa: E402


class GemifiError(RuntimeError):
    pass


class NoReply(GemifiError):
    pass


class StatusError(GemifiError):
    """The device replied with a final, non-OK status (e.g. BAD_VALUE)."""

    def __init__(self, msg, status):
        super().__init__(msg)
        self.status = status


# ---------------- request / reply session ----------------

class Session:
    """Stop-and-wait requests (sysex.md 1): send, wait for cmd|0x40 with the same seq (200 ms, or
    2000 ms for FW_BEGIN/FW_END/COMMIT/FACTORY_RESET); on a timeout, BAD_CHECKSUM or BUSY resend the identical frame (same
    seq), up to `retries` resends (default 5, so 6 sends). LOG/EVENT seen meanwhile go to on_unsolicited.
    `timeout`/`long_timeout` override the spec values (tests use tiny ones)."""

    def __init__(self, port, timeout=gp.REPLY_TIMEOUT_S, retries=gp.MAX_RESENDS, on_unsolicited=None,
                 verbose=False, long_timeout=None):
        self.port, self.timeout, self.retries = port, timeout, retries
        if long_timeout is None:
            long_timeout = gp.LONG_REPLY_TIMEOUT_S if timeout == gp.REPLY_TIMEOUT_S else timeout
        self.long_timeout = long_timeout
        self.on_unsolicited = on_unsolicited
        self.verbose = verbose
        self.seq = int(time.monotonic() * 1000) & 0x7F
        self.resends = 0
        self.bulk = True        # GET_PARAMS / SET_PARAMS until the device replies BAD_CMD

    def next_seq(self):
        self.seq = (self.seq + 1) & 0x7F
        return self.seq

    def request(self, cmd, payload=b"", retries=None):
        if gp.is_notification(cmd):
            raise ValueError(f"0x{cmd:02x} is a notification (never answered): use notify()")
        seq = self.next_seq()
        frame = gp.build_frame(cmd, seq, payload)
        tries = (self.retries if retries is None else retries) + 1
        for attempt in range(tries):
            if attempt:
                self.resends += 1
                if self.verbose:
                    print(f"  resend {gp.CMD_NAMES.get(cmd, hex(cmd))} seq {seq} (try {attempt + 1})", file=sys.stderr)
            self.port.send(frame)
            wait = self.long_timeout if cmd in gp.LONG_TIMEOUT_CMDS else self.timeout
            deadline = time.monotonic() + wait
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                resend = False
                found = None
                for msg in self.port.recv(left):
                    if not gp.is_gemifi(msg):
                        continue
                    try:
                        f = gp.parse_frame(msg)
                    except gp.ProtocolError as e:
                        if self.verbose:
                            print(f"  ignoring bad frame: {e}", file=sys.stderr)
                        continue
                    if f.is_unsolicited:
                        if self.on_unsolicited:
                            self.on_unsolicited(f)
                        continue
                    if found is None and gp.reply_matches(f, cmd, seq):
                        if f.status in (gp.BAD_CHECKSUM, gp.BUSY):
                            resend = True      # not completed on the device: same seq again
                            if f.status == gp.BUSY:
                                time.sleep(min(self.timeout, 0.05))
                            break
                        found = f              # still pass on unsolicited messages that came with it
                        continue
                    if self.verbose:
                        print(f"  ignoring frame cmd 0x{f.cmd:02x} seq {f.seq}", file=sys.stderr)
                if found is not None:
                    return found
                if resend:
                    break
        raise NoReply(f"no reply to {gp.CMD_NAMES.get(cmd, hex(cmd))} after {tries} tries")

    def notify(self, cmd, payload=b""):
        """Send a host -> device notification (0x20-0x3F, sysex.md 4.6.2) once, without waiting: the device
        never replies, `seq` is ignored (0 is sent), and there are no retries. Only for a device with HELLO
        caps bit 6 (older firmware would answer BAD_CMD in the unsolicited range)."""
        if not gp.is_notification(cmd):
            raise ValueError(f"0x{cmd:02x} is not a notification (0x20-0x3F): use request()")
        self.port.send(gp.build_frame(cmd, 0, payload))

    def reboot(self, mode):
        """REBOOT(mode). The reply may be lost as the device resets, so a timeout counts as success
        (sysex.md 1). Returns True if a reply was seen."""
        try:
            expect_ok(self.request(gp.REBOOT, gp.req_reboot(mode)), "REBOOT")
            return True
        except NoReply:
            return False

    def hello(self):
        f = self.request(gp.HELLO)
        if f.status != gp.OK:
            raise GemifiError(f"HELLO: {gp.STATUS.get(f.status, f.status)}")
        return gp.parse_hello(f.payload)

    # -- config (sysex.md 4.3) --

    def get_schema_bytes(self, window=gp.SCHEMA_WINDOW):
        """Read the whole schema with GET_SCHEMA windows from offset 0 until offset >= total."""
        return self.get_window_bytes(gp.GET_SCHEMA, "GET_SCHEMA", window)

    def get_window_bytes(self, cmd, what, window=gp.SCHEMA_WINDOW):
        """A blob read in windows (GET_SCHEMA, GET_LAYOUT: sysex.md 4.3, 4.7) from 0 until offset >= total."""
        data = bytearray()
        total = None
        while total is None or len(data) < total:
            f = expect_ok(self.request(cmd, gp.req_get_schema(len(data), window)), what)
            t, off, chunk = gp.parse_schema_reply(f.payload)
            if off != len(data):
                raise GemifiError(f"{what} at {len(data)}: device echoed offset {off}")
            if total is not None and t != total:
                raise GemifiError(f"{what}: total changed from {total} to {t} mid-read")
            total = t
            if len(chunk) > window or (not chunk and len(data) < total) or len(data) + len(chunk) > total:
                raise GemifiError(f"{what} at {len(data)}: {len(chunk)} bytes returned (total {total})")
            data += chunk
        return bytes(data)

    def get_schema(self):
        return gp.parse_schema(self.get_schema_bytes())

    def _param(self, cmd, payload, pid, index, what):
        f = expect_ok(self.request(cmd, payload), what)
        r = gp.parse_param_reply(f.payload)
        if (r.id, r.index) != (pid, index):
            raise GemifiError(f"{what}: reply is for id {r.id}[{r.index}], asked for {pid}[{index}]")
        return r.value

    def get_param(self, pid, index=0):
        return self._param(gp.GET_PARAM, gp.req_get_param(pid, index), pid, index, f"GET_PARAM {pid}[{index}]")

    def set_param(self, pid, index, value):
        """Returns the value now in effect."""
        return self._param(gp.SET_PARAM, gp.req_set_param(pid, index, value), pid, index,
                           f"SET_PARAM {pid}[{index}]")

    def get_values(self, p):
        """Every value of param p (a list of `count` ints): GET_PARAMS runs of up to 16, or single
        GET_PARAMs on a device that replies BAD_CMD to GET_PARAMS."""
        if p.count == 1 or not self.bulk:
            return [self.get_param(p.id, i) for i in range(p.count)]
        out = []
        for i in range(0, p.count, gp.MAX_RUN):
            n = min(gp.MAX_RUN, p.count - i)
            f = self.request(gp.GET_PARAMS, gp.req_get_params(p.id, i, n))
            if f.status == gp.BAD_CMD and not out:
                self.bulk = False
                return [self.get_param(p.id, k) for k in range(p.count)]
            expect_ok(f, f"GET_PARAMS {p.id}[{i}..{i + n - 1}]")
            pid, idx, vals = gp.parse_params_reply(f.payload)
            if (pid, idx, len(vals)) != (p.id, i, n):
                raise GemifiError(f"GET_PARAMS: reply is for id {pid}[{idx}] n={len(vals)}, asked for {p.id}[{i}] n={n}")
            out += vals
        return out

    def set_values(self, pid, index, values):
        """SET_PARAMS for a run of up to 16 values (all or none applied), or single SET_PARAMs on a
        device that replies BAD_CMD to SET_PARAMS (then values before a failing one stay applied)."""
        if len(values) > 1 and self.bulk:
            f = self.request(gp.SET_PARAMS, gp.req_set_params(pid, index, values))
            if f.status != gp.BAD_CMD:
                what = f"SET_PARAMS {pid}[{index}..{index + len(values) - 1}]"
                expect_ok(f, what)
                if gp.parse_set_params_reply(f.payload) != (pid, index, len(values)):
                    raise GemifiError(f"{what}: reply doesn't echo id, index and n")
                return
            self.bulk = False
        for k, v in enumerate(values):
            self.set_param(pid, index + k, v)

    def commit(self):
        expect_ok(self.request(gp.COMMIT), "COMMIT")

    def factory_reset(self):
        expect_ok(self.request(gp.FACTORY_RESET), "FACTORY_RESET")

    # -- presets (sysex.md 4.5) --

    def preset_info(self, slot=0):
        """PRESET_INFO for `slot` (0 = the active one)."""
        f = expect_ok(self.request(gp.PRESET_INFO, gp.req_preset_info(slot)), f"PRESET_INFO {slot}")
        info = gp.parse_preset_info(f.payload)
        if slot and info.slot != slot:
            raise GemifiError(f"PRESET_INFO: reply is for slot {info.slot}, asked for {slot}")
        return info

    def preset_infos(self):
        """PRESET_INFO for every slot, 1..count."""
        first = self.preset_info(0)
        return [first if n == first.slot else self.preset_info(n) for n in range(1, first.count + 1)]

    def preset_select(self, slot):
        """Select = SET_PARAM preset.active. Unsaved edits of the old preset are dropped."""
        now = self.set_param(gp.P_PRESET_ACTIVE, 0, slot)
        if now != slot:
            raise GemifiError(f"selected preset {slot}, but the device reports {now}")

    def preset_save(self, slot):
        expect_ok(self.request(gp.PRESET_SAVE, gp.req_preset_save(slot)), f"PRESET_SAVE {slot}")

    def preset_reset(self, slot=gp.PRESET_ALL):
        expect_ok(self.request(gp.PRESET_RESET, gp.req_preset_reset(slot)), f"PRESET_RESET {slot or 'all'}")

    def preset_name(self, slot, name):
        expect_ok(self.request(gp.PRESET_NAME, gp.req_preset_name(slot, name)), f"PRESET_NAME {slot}")

    def preset_delete(self, slot):
        """PRESET_DELETE: the later presets move up one slot; unsaved edits of the active one are dropped."""
        expect_ok(self.request(gp.PRESET_DELETE, gp.req_preset_delete(slot)), f"PRESET_DELETE {slot}")

    def preset_move(self, from_slot, to_slot):
        """PRESET_MOVE: the preset in from_slot goes to to_slot, the ones between shift one slot towards
        from_slot. The active preset follows; its unsaved edits are dropped."""
        expect_ok(self.request(gp.PRESET_MOVE, gp.req_preset_move(from_slot, to_slot)),
                  f"PRESET_MOVE {from_slot} -> {to_slot}")


    # -- host feedback (sysex.md 4.6) --

    def get_component(self, kind, index):
        """GET_COMPONENT -> Component, or None when the device replies BAD_PARAM (past the end, or none of
        this kind)."""
        what = f"GET_COMPONENT {gp.KIND_NAMES.get(kind, kind)} {index}"
        f = self.request(gp.GET_COMPONENT, gp.req_get_component(kind, index))
        if f.status == gp.BAD_PARAM:
            return None
        c = gp.parse_component(expect_ok(f, what).payload)
        if (c.kind, c.index) != (kind, index):
            raise GemifiError(f"{what}: reply is for {c.kind_name} {c.index}")
        return c

    def components(self, kinds=gp.KINDS):
        """Every component of each kind: index 0 gives `count`, then 1..count-1."""
        out = []
        for kind in kinds:
            first = self.get_component(kind, 0)
            if first is None:
                continue
            out.append(first)
            for i in range(1, first.count):
                c = self.get_component(kind, i)
                if c is None:
                    raise GemifiError(f"GET_COMPONENT {first.kind_name} {i}: BAD_PARAM, but count is {first.count}")
                out.append(c)
        return out


def expect_ok(f, what):
    if f.status != gp.OK:
        raise StatusError(f"{what}: device replied {gp.STATUS.get(f.status, f'status {f.status}')}", f.status)
    return f


# ---------------- port discovery ----------------

def is_boot_name(n):
    return "gemifi boot" in n.lower()


def is_app_name(n):
    return "gemifi" in n.lower() and not is_boot_name(n)


def find_port(backend, kind, match=None):
    """kind: 'boot', 'app' or 'any' (app first). match: substring that overrides the name rules.
    A fixed backend (serial) has one port and no names to match: that port, if it's there."""
    names = backend.ports()
    if getattr(backend, "fixed", False):
        return names[0] if names else None
    if match:
        hits = [n for n in names if match.lower() in n.lower()]
        return hits[0] if hits else None
    if kind in ("boot", "any"):
        boot = [n for n in names if is_boot_name(n)]
    if kind in ("app", "any"):
        app = [n for n in names if is_app_name(n)]
    if kind == "boot":
        return boot[0] if boot else None
    if kind == "app":
        return app[0] if app else None
    return (app or boot or [None])[0]


def wait_port(backend, kind, timeout, match=None, gone=None):
    """Wait for a port to appear. If `gone` is given, first wait (briefly) for that name to vanish."""
    deadline = time.monotonic() + timeout
    if getattr(backend, "fixed", False):
        time.sleep(0.5)       # same device file before and after; give the chip time to reset
        while time.monotonic() < deadline:
            n = find_port(backend, kind, match)
            if n:
                return n
            time.sleep(0.1)
        raise GemifiError(f"{getattr(backend, 'path', 'serial port')} didn't come back within {timeout:.0f} s")
    if gone:
        while time.monotonic() < deadline and gone in backend.ports():
            time.sleep(0.05)
    while time.monotonic() < deadline:
        n = find_port(backend, kind, match)
        if n and n != gone:
            time.sleep(0.2)   # let the device finish enumerating before talking to it
            return n
        time.sleep(0.1)
    raise GemifiError(f"no {kind} port appeared within {timeout:.0f} s")


# ---------------- firmware upload (sysex.md 4.2) ----------------

def _upload(sess, img, size, crc32, board_id, fw_version, make_plan, progress=None):
    """FW_BEGIN, FW_DATA for each (offset, length) of make_plan(max_chunk), FW_END; FW_ABORT on any failure."""
    f = expect_ok(sess.request(gp.FW_BEGIN, gp.req_fw_begin(size, crc32, board_id, fw_version)), "FW_BEGIN")
    max_chunk = gp.dec_u14(f.payload[1:3])
    try:
        plan = make_plan(max_chunk)
        for i, (off, n) in enumerate(plan):
            r = expect_ok(sess.request(gp.FW_DATA, gp.req_fw_data(off, img[off:off + n])), f"FW_DATA at 0x{off:04x}")
            echo = gp.dec_u28(r.payload[1:5]) if len(r.payload) >= 5 else None
            if echo != off:
                raise GemifiError(f"FW_DATA at 0x{off:04x}: device echoed offset {echo}")
            if progress:
                progress(i + 1, len(plan), off)
        expect_ok(sess.request(gp.FW_END), "FW_END")
    except BaseException:
        try:
            sess.request(gp.FW_ABORT, retries=1)
        except Exception:
            pass
        raise
    return max_chunk, len(plan)


def board_max_size(board_id):
    """The largest image a gemifi bootloader takes on this board (NUC1xx: its 32 KB APROM)."""
    b = gi.BOARDS.get(board_id)
    return b.max_size if b else gp.APROM_SIZE


def upload(sess, img, hdr, progress=None):
    """Run FW_BEGIN / FW_DATA... / FW_END on a session connected to the bootloader (sysex.md 4.2):
    0x200 upward, page 0 last, no chunk across a 512-byte page."""
    def plan(max_chunk):
        if not gp.valid_max_chunk(max_chunk):
            raise GemifiError(f"device max_chunk {max_chunk} is not a multiple of 4 that divides 512")
        return gp.chunk_plan(hdr.image_size, max_chunk, board_max_size(hdr.board_id))
    return _upload(sess, img, hdr.image_size, hdr.crc32, hdr.board_id, hdr.fw_version, plan, progress)


def upload_ab(sess, data, board_id, fw_version, progress=None):
    """Upload a chip-format image (e.g. an ESP app .bin) to an A/B device: the app itself, no bootloader
    (sysex.md 4.1 "Who takes uploads", 4.2 "A/B devices"). The image is padded with 0xFF to a multiple of 4;
    size and CRC-32 are over the padded file; chunks go in plain order from offset 0. The caller sends
    REBOOT(0) afterwards. Returns (max_chunk, chunks, padded size, crc32)."""
    img = gp.pad4(data)
    crc = gp.ab_crc32(img)

    def plan(max_chunk):
        if max_chunk < 4:
            raise GemifiError(f"device max_chunk {max_chunk} is below 4")
        return gp.linear_chunk_plan(len(img), max_chunk)
    max_chunk, n = _upload(sess, img, len(img), crc, board_id, fw_version, plan, progress)
    return max_chunk, n, len(img), crc


def is_ab(h):
    """HELLO from an app that takes uploads itself (sysex.md 4.1): role 0, caps bit 0, non-zero max_chunk."""
    return h.role == gp.ROLE_APP and bool(h.caps & gp.CAP_FW) and h.max_chunk > 0


def wait_hello(backend, timeout, match=None, gone=None, retries=1):
    """After a reboot into the app: wait for the port to come back and answer HELLO. On USB the port vanishes
    and comes back (often with the same name); on serial the device file stays and HELLO goes unanswered while
    the chip boots. Returns (name, hello)."""
    deadline = time.monotonic() + timeout
    if gone and not getattr(backend, "fixed", False):
        # The reset comes ~50 ms after the REBOOT reply; a fast re-enumeration may never be seen as a gap.
        vanish = min(deadline, time.monotonic() + 1.0)
        while time.monotonic() < vanish and gone in backend.ports():
            time.sleep(0.05)
    else:
        time.sleep(0.3)
    last = None
    while time.monotonic() < deadline:
        name = find_port(backend, "app", match)
        if name:
            try:
                with backend.open(name) as port:
                    return name, Session(port, retries=retries).hello()
            except Exception as e:      # still booting, still enumerating, port busy
                last = e
        time.sleep(0.2)
    raise GemifiError(f"the device didn't answer HELLO within {timeout:.0f} s after the reboot"
                      + (f" ({last})" if last else ""))


def bar(i, n, off):
    w = 30
    k = w * i // n
    sys.stderr.write(f"\r  [{'#' * k}{'.' * (w - k)}] {i}/{n} chunks (0x{off:04x})")
    if i == n:
        sys.stderr.write("\n")
    sys.stderr.flush()


def print_hello(name, h):
    print(f"{name}")
    print(f"  role        {h.role_name}")
    if h.node:
        print(f"  node        {h.node}")
    print(f"  board_id    0x{h.board_id:04x} ({gi.BOARDS.get(h.board_id, ('unknown',))[0]})")
    print(f"  fw_version  {gp.fmt_version(h.fw_version)}")
    print(f"  protocol    {h.proto_version}")
    caps = [n for b, n in ((gp.CAP_FW, "fw-upload"), (gp.CAP_CONFIG, "config"), (gp.CAP_LOG, "log"),
                           (gp.CAP_EVENT, "event"), (gp.CAP_PRESETS, "presets"),
                           (gp.CAP_FEEDBACK, "feedback"), (gp.CAP_LAYOUT, "layout"),
                           (gp.CAP_MONITOR, "monitor")) if h.caps & b]
    print(f"  caps        0x{h.caps:04x} {' '.join(caps)}")
    print(f"  max_chunk   {h.max_chunk}")
    print(f"  uid         {h.uid_hex}")


def describe_param_changed(m, schema=None):
    """PARAM_CHANGED as text; with a schema, by name and display value."""
    p = schema.by_id(m.id) if schema else None
    if p is None:
        return str(m)
    ref = p.name + (f":{m.index}" if p.count > 1 else "")
    return f"PARAM_CHANGED {ref} = {gp.format_value(p, m.value)}"


def print_unsolicited(f, schema=None):
    try:
        if f.cmd == gp.LOG:
            print(gp.parse_log(f.payload), flush=True)
        elif f.cmd == gp.EVENT:
            print(gp.parse_event(f.payload), flush=True)
        elif f.cmd == gp.PARAM_CHANGED:
            print(describe_param_changed(gp.parse_param_changed(f.payload), schema), flush=True)
        elif f.cmd == gp.PRESET_CHANGED:
            print(gp.parse_preset_changed(f.payload), flush=True)
        elif f.cmd == gp.RAW:
            m = gp.parse_raw(f.payload)
            if m is not None:
                print(raw_text(m), flush=True)
        elif f.cmd == gp.BOOTED:
            h = gp.parse_booted(f.payload)
            print(f"BOOTED {h.node or 'device'}, fw {gp.fmt_version(h.fw_version)}, caps 0x{h.caps:04x} "
                  "(it restarted: its feedback state is gone)", flush=True)
        else:
            print(f"[unsolicited 0x{f.cmd:02x}] {f.payload.hex()}", flush=True)
    except gp.ProtocolError as e:
        print(f"[bad 0x{f.cmd:02x}: {e}]", flush=True)


# ---------------- commands ----------------

def cmd_ports(a, backend):
    names = backend.ports()
    print(f"backend: {backend.label}")
    if not names:
        print("  (no MIDI ports with both input and output)")
    for n in names:
        tag = "[boot]" if is_boot_name(n) else "[app] " if is_app_name(n) else "      "
        print(f"  {tag} {n}")
    return 0


def open_session(a, backend, kind="any"):
    name = find_port(backend, kind, a.port)
    if not name:
        raise GemifiError("no gemifi MIDI port found (try `gemifi.py ports`, or --port NAME)")
    port = backend.open(name)
    return name, port, Session(port, retries=a.retries, on_unsolicited=print_unsolicited, verbose=a.verbose)


def cmd_hello(a, backend):
    name, port, s = open_session(a, backend)
    with port:
        print_hello(name, s.hello())
    return 0


def cmd_log(a, backend):
    name, port, s = open_session(a, backend)
    print(f"listening on {name} (Ctrl-C to stop)", file=sys.stderr)
    with port:
        schema = None
        if a.names:
            schema = s.get_schema()   # so PARAM_CHANGED prints names and labels
        try:
            while True:
                for msg in port.recv(0.5):
                    if gp.is_gemifi(msg):
                        try:
                            f = gp.parse_frame(msg)
                        except gp.ProtocolError as e:
                            print(f"[bad frame: {e}]")
                            continue
                        if f.is_unsolicited:
                            print_unsolicited(f, schema)
                    elif a.verbose:
                        print(f"[sysex] {bytes(msg).hex(' ')}")
        except KeyboardInterrupt:
            return 0


def cmd_reboot(a, backend):
    mode = gp.REBOOT_BOOT if a.mode == "boot" else gp.REBOOT_APP
    name, port, s = open_session(a, backend)
    with port:
        replied = s.reboot(mode)
    print(f"{name}: rebooting into {'bootloader' if mode else 'app'}" + ("" if replied else " (no reply; assuming it reset)"))
    if a.wait:
        if mode == gp.REBOOT_APP:
            # The app's port may come back under the same name (an A/B device), so wait for HELLO instead.
            new, _ = wait_hello(backend, a.wait, a.port, gone=name, retries=a.retries)
        else:
            new = wait_port(backend, "boot", a.wait, gone=name)
        print(f"up: {new}")
    return 0


# ---------------- config (sysex.md 4.3, 4.4) ----------------

def config_session(a, backend):
    """Open the app port and check it does config. Returns (name, port, session, hello); the caller closes
    the port (`with port:`)."""
    name, port, s = open_session(a, backend, "app")
    try:
        h = s.hello()
        if h.role != gp.ROLE_APP:
            raise GemifiError(f"{name} is the bootloader; config needs the app (gemifi.py reboot app)")
        if not h.caps & gp.CAP_CONFIG:
            raise GemifiError(f"{name}: this firmware doesn't advertise config (caps 0x{h.caps:04x})")
    except BaseException:
        port.close()
        raise
    return name, port, s, h


def ref_of(p, index):
    return p.name + (f":{index}" if p.count > 1 else "")


def flags_text(p):
    names = [n for b, n in gp.FLAG_NAMES if p.flags & b and b not in (gp.F_PER_INDEX_DEFAULT, gp.F_INDEX_LABELS,
                                                                       gp.F_HAS_UNIT)]
    unknown = p.flags & ~sum(b for b, _ in gp.FLAG_NAMES)
    return " ".join(names + ([f"0x{unknown:02x}?"] if unknown else []))


def print_schema(schema, size=None):
    print(f"schema v{schema.version}: {len(schema.params)} params in {len(schema.groups)} groups"
          + (f" ({size} bytes)" if size is not None else ""))
    rows = []
    for p in schema.params:
        rng = f"{p.min}..{p.max}"
        if p.type == gp.T_NOTE:
            rng += f" ({gp.note_name(p.min) if 0 <= p.min <= 127 else '?'}..{gp.note_name(p.max) if 0 <= p.max <= 127 else '?'})"
        dflt = ", ".join(gp.format_value(p, d, unit=False) for d in p.defaults)
        tname = gp.TYPE_NAMES.get(p.type, f"type{p.type}")
        rows.append((p, [str(p.id), p.name, tname, f"x{p.count}" if p.count > 1 else "", rng, p.unit, dflt,
                         flags_text(p), p.label]))
    head = ["id", "name", "type", "count", "range", "unit", "default", "flags", "label"]
    widths = [max([len(h)] + [len(r[i]) for _, r in rows]) for i, h in enumerate(head)]
    widths[6] = min(widths[6], 24)

    def line(cells):
        return "  " + "  ".join(c.rjust(widths[0]) if i == 0 else c.ljust(widths[i]) for i, c in enumerate(cells)).rstrip()

    for g, gname in enumerate(schema.groups):
        grows = [(p, r) for p, r in rows if p.group == g]
        if not grows:
            continue
        print(f"\n{gname}")
        print(line(head))
        for p, r in grows:
            if len(r[6]) > 24:
                r[6] = r[6][:21] + "..."
            print(line(r))
            pad = " " * (widths[0] + 4)
            if p.options:
                print(pad + "options: " + ", ".join(f"{p.min + i}={o}" for i, o in enumerate(p.options) if o))
            if p.index_labels:
                print(pad + "index:   " + ", ".join(f"{i}={s}" for i, s in enumerate(p.index_labels)))


def cmd_schema(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        raw = s.get_schema_bytes()
    schema = gp.parse_schema(raw)
    if a.raw:
        with open(a.raw, "wb") as f:
            f.write(raw)
        print(f"wrote {len(raw)} bytes to {a.raw}", file=sys.stderr)
    if a.json:
        print(json.dumps(schema.to_json(), indent=1))
    else:
        print(name)
        print_schema(schema, len(raw))
    return 0


def value_lines(schema, items, headers=False):
    """items: [(param, [(index, value)])] -> aligned text lines, enum values by label; with `headers`, a group
    label line before each change of group."""
    rows = []
    for p, vals in items:
        for i, v in vals:
            what = p.label + (f" / {p.index_label(i)}" if p.count > 1 else "")
            rows.append((p, ref_of(p, i), gp.format_value(p, v), what))
    w0 = max((len(r[1]) for r in rows), default=0)
    w1 = max((len(r[2]) for r in rows), default=0)
    out, group = [], None
    for p, ref, val, what in rows:
        if headers and p.group != group:
            group = p.group
            out.append(schema.groups[group] if group < len(schema.groups) else f"group {group}")
        out.append(f"  {ref.ljust(w0)}  {val.ljust(w1)}  {what}".rstrip())
    return out


def cmd_get(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        schema = s.get_schema()
        try:
            refs = [gp.find_param(schema, r) for r in a.refs]
        except ValueError as e:
            raise GemifiError(str(e)) from None
        if not refs:
            ps = sorted(schema.params, key=lambda p: p.group)     # stable: schema order within a group
            for line in value_lines(schema, [(p, list(enumerate(s.get_values(p)))) for p in ps], headers=True):
                print(line)
            return 0
        items = []
        for p, index in refs:
            vals = [(index, s.get_param(p.id, index))] if index is not None else list(enumerate(s.get_values(p)))
            items.append((p, vals))
        for line in value_lines(schema, items):
            print(line)
    return 0


def parse_assignment(schema, text):
    """'name[:index]=value' -> (param, index, value). Raises ValueError."""
    ref, sep, val = text.partition("=")
    if not sep:
        raise ValueError(f"{text!r}: expected NAME[:INDEX]=VALUE")
    p, index = gp.find_param(schema, ref)
    if index is None:
        if p.count > 1:
            raise ValueError(f"{p.name} has {p.count} values; use {p.name}:INDEX=VALUE (INDEX 0..{p.count - 1}"
                             + (" or an index label" if p.index_labels else "") + ")")
        index = 0
    if p.read_only:
        raise ValueError(f"{p.name} is read-only")
    return p, index, gp.parse_value(p, val)


def maybe_commit(s, a, changed):
    if a.commit:
        if not changed:
            print("nothing changed; not committing")
            return
        s.commit()
        print("committed to flash")
    elif changed:
        print("(live only; run `gemifi.py commit` or pass --commit to keep it after power-off)")


def cmd_set(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        schema = s.get_schema()
        try:
            todo = [parse_assignment(schema, t) for t in a.assignments]
        except ValueError as e:
            raise GemifiError(str(e)) from None
        errors = changed = 0
        for p, index, v in todo:
            try:
                now = s.set_param(p.id, index, v)
            except StatusError as e:
                if e.status not in (gp.BAD_PARAM, gp.BAD_VALUE):
                    raise
                print(f"  {ref_of(p, index)}: {gp.STATUS[e.status]} ({gp.format_value(p, v)})")
                errors += 1
                continue
            changed += 1
            note = "" if now == v else f" (device kept {gp.format_value(p, now)}, asked for {gp.format_value(p, v)})"
            print(f"  {ref_of(p, index)} = {gp.format_value(p, now)}{note}")
        maybe_commit(s, a, changed)
    return 1 if errors else 0


def cmd_commit(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        s.commit()
    print(f"{name}: committed to flash")
    return 0


def confirm(prompt):
    try:
        return input(f"{prompt} Type 'yes' to continue: ").strip().lower() == "yes"
    except EOFError:
        return False


def cmd_factory_reset(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        if not a.yes and not confirm(f"{name}: reset every setting to its default and erase the stored profile?"):
            print("cancelled")
            return 1
        s.factory_reset()
    print(f"{name}: factory reset done")
    return 0


BACKUP_FORMAT = 1            # {"values"}: a device without presets
BACKUP_FORMAT_PRESETS = 2    # {"values": globals, "active", "presets": [every slot]}


def has_presets(h):
    return bool(h.caps & gp.CAP_PRESETS) and bool(h.caps & gp.CAP_CONFIG)


def quiet_presets(s):
    """While the tool switches presets itself: drop the PRESET_CHANGED / PARAM_CHANGED that it causes."""
    def handler(f):
        if f.cmd not in (gp.PRESET_CHANGED, gp.PARAM_CHANGED):
            print_unsolicited(f)
    s.on_unsolicited = handler


def preset_session(a, backend):
    """config_session for a device with presets (HELLO caps bit 5). Returns (name, port, session, hello)."""
    name, port, s, h = config_session(a, backend)
    if not h.caps & gp.CAP_PRESETS:
        port.close()
        raise GemifiError(f"{name}: this firmware has no presets (HELLO caps 0x{h.caps:04x}, bit 5 not set)")
    quiet_presets(s)
    return name, port, s, h


def read_values(s, params):
    out = {}
    for p in params:
        vals = s.get_values(p)
        out[p.name] = vals[0] if p.count == 1 else vals
    return out


def preset_params(schema):
    """The params each preset has its own value of (schema flag PRESET)."""
    return [p for p in schema.params if p.per_preset]


def global_params(schema):
    """Everything else, without preset.active (a backup keeps that as "active") and preset.dirty (state)."""
    return [p for p in schema.params if not p.per_preset and p.id not in (gp.P_PRESET_ACTIVE, gp.P_PRESET_DIRTY)]


def sub_schema(schema, params):
    return gp.Schema(schema.version, schema.groups, list(params))


def put_back(s, params, want):
    """Write the values in `want` ({name: value | [values]}) that differ from what the device has now."""
    for p in params:
        w = want[p.name] if isinstance(want[p.name], list) else [want[p.name]]
        now = s.get_values(p)
        for i in range(0, p.count, gp.MAX_RUN):
            if now[i:i + gp.MAX_RUN] != w[i:i + gp.MAX_RUN]:
                s.set_values(p.id, i, w[i:i + gp.MAX_RUN])


def backup_doc(s, h, schema, progress=None):
    """Read the device into a backup document. With presets, it switches to every preset in use in turn and
    reads its values (sysex.md 4.5 "Backups"; free slots can't be selected and aren't backed up), then
    switches back to the slot that was active and puts its unsaved edits back, so the device ends as it
    started (dirty flag included)."""
    doc = {"gemifi_backup": BACKUP_FORMAT, "board_id": h.board_id, "fw_version": gp.fmt_version(h.fw_version)}
    if not has_presets(h) or schema.by_id(gp.P_PRESET_ACTIVE) is None:
        doc["values"] = read_values(s, schema.params)
        return doc
    quiet_presets(s)
    infos = s.preset_infos()
    first = infos[0]
    active = first.active
    pp = preset_params(schema)
    doc.update(gemifi_backup=BACKUP_FORMAT_PRESETS, presets_revision=first.revision, active=active,
               values=read_values(s, global_params(schema)))
    live = read_values(s, pp)              # the active slot as it is now, unsaved edits included
    dirty = next(i.dirty for i in infos if i.slot == active)
    slots, switched = [], False
    used = gp.presets_in_use(infos)
    try:
        for i in used:
            if i.slot == active:
                vals = live
            else:
                s.preset_select(i.slot)
                switched = True
                vals = read_values(s, pp)
            e = {"slot": i.slot, "name": i.name, "stored": i.stored, "stored_revision": i.stored_revision,
                 "values": vals}
            if i.slot == active and dirty:
                e["unsaved"] = True
            slots.append(e)
            if progress:
                progress(len(slots), len(used))
    finally:
        if switched:
            s.preset_select(active)
            if dirty:
                put_back(s, pp, live)
    doc["presets"] = slots
    return doc


def backup_summary(doc):
    n = len(doc["values"]) + sum(len(e["values"]) for e in doc.get("presets", []))
    return f"{n} params" + (f", {len(doc['presets'])} presets" if "presets" in doc else "")


def write_backup(doc, path):
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


def cmd_backup(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        schema = s.get_schema()
        doc = backup_doc(s, h, schema)
    write_backup(doc, a.file)
    print(f"{name}: saved {backup_summary(doc)} to {a.file}")
    return 0


def backup_value(p, v):
    """One value from a backup file: an int, a bool, or text parse_value understands (range left to the device)."""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        return gp.parse_value(p, v, check_range=False)
    raise ValueError(f"{p.name}: can't use {v!r}")


def restore_values(s, schema, values, warn=print):
    """Write a backup's {name: value | [values]} to the device. Unknown names, read-only params and bad entries
    are reported and skipped. Returns (n_set, n_errors)."""
    n_set = n_err = 0
    for name, raw in values.items():
        p = schema.by_name(name)
        if p is None:
            warn(f"  warning: {name}: not in this firmware's schema; skipped")
            continue
        if p.read_only:
            warn(f"  {name}: read-only; skipped")
            continue
        raws = raw if isinstance(raw, list) else [raw]
        if p.count > 1 and not isinstance(raw, list):
            warn(f"  {name}: has {p.count} values but the backup has one; skipped")
            n_err += 1
            continue
        if len(raws) != p.count:
            warn(f"  warning: {name}: backup has {len(raws)} values, device has {p.count}; restoring the first "
                 f"{min(len(raws), p.count)}")
            raws = raws[:p.count]
        vals = []
        for i, v in enumerate(raws):
            try:
                vals.append((i, backup_value(p, v)))
            except ValueError as e:
                warn(f"  {ref_of(p, i)}: {e}")
                n_err += 1
        # contiguous runs of up to 16 for SET_PARAMS; on a failed run, retry one by one to find the bad entries
        runs, cur = [], []
        for i, v in vals:
            if cur and (i != cur[-1][0] + 1 or len(cur) == gp.MAX_RUN):
                runs.append(cur)
                cur = []
            cur.append((i, v))
        if cur:
            runs.append(cur)
        for run in runs:
            try:
                s.set_values(p.id, run[0][0], [v for _, v in run])
                n_set += len(run)
                continue
            except StatusError as e:
                if e.status not in (gp.BAD_PARAM, gp.BAD_VALUE):
                    raise
                if len(run) == 1:
                    warn(f"  {ref_of(p, run[0][0])}: {gp.STATUS[e.status]} (value {run[0][1]})")
                    n_err += 1
                    continue
            for i, v in run:
                try:
                    s.set_param(p.id, i, v)
                    n_set += 1
                except StatusError as e:
                    if e.status not in (gp.BAD_PARAM, gp.BAD_VALUE):
                        raise
                    warn(f"  {ref_of(p, i)}: {gp.STATUS[e.status]} (value {v})")
                    n_err += 1
    missing = [p.name for p in schema.params if p.name not in values and not p.read_only]
    if missing:
        warn(f"  not in the backup, left as they are: {', '.join(missing)}")
    return n_set, n_err


def restore_presets(s, schema, doc, warn=print):
    """A format-2 backup onto a device with presets (sysex.md 4.5 "Backups"): the globals, then every slot
    in the backup (a free one is named first, so it is a preset; then PRESET_RESET it, and if the backup has
    stored edits for it: select, set its values, PRESET_SAVE, PRESET_NAME). On a firmware that tracks the
    presets in use, the ones past the backup's last slot are then deleted, from the end. Last, select the
    slot that was active and COMMIT, so preset.active is stored too. Returns (n_set, n_errors)."""
    quiet_presets(s)
    infos = s.preset_infos()
    count = infos[0].count
    tracked = gp.tracks_in_use(infos)
    used = len(gp.presets_in_use(infos))
    gvals = {k: v for k, v in doc["values"].items() if k not in ("preset.active", "preset.count")}
    n_set, n_err = restore_values(s, sub_schema(schema, global_params(schema)), gvals, warn)
    pview = sub_schema(schema, preset_params(schema))
    done = set()
    for e in sorted(doc["presets"], key=lambda e: e.get("slot") if isinstance(e.get("slot"), int) else 0):
        n = e.get("slot")
        if not isinstance(n, int) or not 1 <= n <= count:
            warn(f"  warning: preset {n}: this device has presets 1..{count}; skipped")
            continue
        if tracked and n > used:
            s.preset_name(n, "")             # a free slot: now a preset (and the slots before it)
            used = n
        s.preset_reset(n)
        done.add(n)
        if not e.get("stored"):
            continue                       # no stored edits in the backup: the firmware's preset as it is
        s.preset_select(n)
        k, err = restore_values(s, pview, e.get("values", {}),
                                lambda m, n=n: warn(m.replace("  ", f"  preset {n}: ", 1)))
        n_set, n_err = n_set + k, n_err + err
        s.preset_save(n)
        name = e.get("name")
        if isinstance(name, str) and name != s.preset_info(n).name:
            try:
                s.preset_name(n, name)
            except (ValueError, StatusError) as ex:
                warn(f"  preset {n}: name {name!r}: {ex}")
                n_err += 1
    if tracked and done:
        for n in range(used, max(done), -1):   # presets the backup doesn't have, past its last one
            s.preset_delete(n)
        used = min(used, max(done))
    left = [n for n in range(1, used + 1) if n not in done]
    if left:
        warn(f"  presets not in the backup, left as they are: {', '.join(map(str, left))}")
    active = doc.get("active")
    s.preset_select(active if isinstance(active, int) and 1 <= active <= used else 1)
    s.commit()
    return n_set, n_err


def cmd_restore(a, backend):
    with open(a.file) as f:
        doc = json.load(f)
    values = doc.get("values") if isinstance(doc, dict) else None
    if not isinstance(values, dict):
        raise GemifiError(f"{a.file}: not a gemifi backup (no \"values\" object)")
    presets = doc.get("presets")
    if presets is not None and not isinstance(presets, list):
        raise GemifiError(f"{a.file}: \"presets\" is not a list")
    name, port, s, h = config_session(a, backend)
    with port:
        if "board_id" in doc and doc["board_id"] != h.board_id:
            print(f"  warning: backup is from board 0x{doc['board_id']:04x}, this is 0x{h.board_id:04x}; "
                  "matching by name anyway")
        schema = s.get_schema()
        if presets is not None and has_presets(h):
            n_set, n_err = restore_presets(s, schema, doc)
            print(f"{name}: restored {n_set} values in {len(presets)} presets" + (f", {n_err} errors" if n_err else ""))
            print("stored in flash (every preset is saved as it is restored)")
            return 1 if n_err else 0
        if presets is not None:
            act = next((e for e in presets if e.get("slot") == doc.get("active")), presets[0] if presets else None)
            print("  warning: the backup has presets but this firmware doesn't; restoring the globals and "
                  f"preset {act['slot'] if act else '?'} only")
            values = dict(values, **(act or {}).get("values", {}))
        elif has_presets(h):
            print(f"  note: a backup without presets; per-preset values go into the active preset "
                  f"({s.preset_info(0).active})")
        n_set, n_err = restore_values(s, schema, values)
        print(f"{name}: restored {n_set} values" + (f", {n_err} errors" if n_err else ""))
        maybe_commit(s, a, n_set)
    return 1 if n_err else 0


# ---------------- presets (sysex.md 4.5) ----------------

def program_text(v):
    return "off" if v < 0 else str(v)


def print_presets(infos):
    first = infos[0]
    tracked = gp.tracks_in_use(infos)
    in_use = f" ({len(gp.presets_in_use(infos))} in use)" if tracked else ""
    print(f"{first.count} presets{in_use}, active {first.active}, firmware presets revision {first.revision}")
    w = max(len(i.name) for i in infos)
    print(f"     #  {'name'.ljust(w)}  {'program':7}  state")
    for i in infos:
        state = []
        if i.dirty:
            state.append("unsaved edits")
        if i.stored:
            state.append("stored edits" + (f" (revision {i.stored_revision})" if i.stale else ""))
        if tracked and not i.in_use:
            state.append("free")
        elif not i.yaml:
            state.append("empty slot")
        mark = "*" if i.is_active else " "
        print(f"  {mark} {i.slot:2d}  {i.name.ljust(w)}  {program_text(i.program):7}  {', '.join(state)}".rstrip())


def cmd_presets(a, backend):
    name, port, s, h = preset_session(a, backend)
    with port:
        infos = s.preset_infos()
    print(name)
    print_presets(infos)
    return 0


def slot_arg(infos, text, in_use=False):
    """A preset by number (1..count) or by name (any case; a preset in use before a free slot). With
    `in_use`, a free slot is refused (select and delete need a preset; sysex.md 4.5 "Presets in use")."""
    t = text.strip()
    used = gp.presets_in_use(infos)
    if t.isdigit():
        n = int(t)
        if not 1 <= n <= infos[0].count:
            raise GemifiError(f"preset {n}: this device has presets 1..{infos[0].count}")
    else:
        hits = [i.slot for i in used + [i for i in infos if i not in used] if i.name.lower() == t.lower()]
        if not hits:
            raise GemifiError(f"no preset named {text!r} (gemifi.py presets lists them)")
        n = hits[0]
    if in_use and all(i.slot != n for i in used):
        raise GemifiError(f"slot {n} is free, not a preset: presets 1..{len(used)} are in use "
                          "(`gemifi.py preset rename` or `preset save` makes a new one)")
    return n


def cmd_preset(a, backend):
    name, port, s, h = preset_session(a, backend)
    with port:
        infos = s.preset_infos()
        cur = next(i for i in infos if i.is_active)
        what = a.preset_cmd
        if what == "select":
            n = slot_arg(infos, a.slot, in_use=True)
            if cur.dirty and not a.discard and not confirm(
                    f"preset {cur.slot} ({cur.name}) has unsaved edits; switching drops them."):
                print("cancelled (save them with `gemifi.py preset save`, or pass --discard)")
                return 1
            s.preset_select(n)
            i = s.preset_info(n)
            print(f"{name}: preset {n} ({i.name}) active")
            return 0
        if what == "save":
            n = slot_arg(infos, a.slot) if a.slot is not None else cur.slot
            if a.name is not None:
                gp_check_name(a.name)
            s.preset_save(n)
            if a.name is not None:
                s.preset_name(n, a.name)
            i = s.preset_info(n)
            print(f"{name}: saved into preset {n} ({i.name})" + ("" if n == cur.slot else f", copied from {cur.slot}")
                  + "; it is now the active one")
            return 0
        if what == "reset":
            if a.slot.lower() == "all":
                n, label = gp.PRESET_ALL, "every preset"
            else:
                n = slot_arg(infos, a.slot)
                label = f"preset {n} ({next(i.name for i in infos if i.slot == n)})"
            if not a.yes and not confirm(f"{name}: drop the stored edits and name of {label}, back to the "
                                         "firmware's values?"):
                print("cancelled")
                return 1
            s.preset_reset(n)
            print(f"{name}: {label} back to the firmware's values")
            return 0
        if what == "delete":
            if not gp.tracks_in_use(infos):
                raise GemifiError(f"{name}: this firmware has no PRESET_DELETE; `gemifi.py preset reset` empties a "
                                  "slot instead")
            used = gp.presets_in_use(infos)
            n = slot_arg(infos, a.slot, in_use=True)
            if len(used) == 1:
                raise GemifiError(f"{name}: preset {n} is the only one in use; the last preset can't be deleted")
            last = len(used)
            gone = next(i.name for i in infos if i.slot == n)
            nxt = (n if n < last else n - 1) if cur.slot == n else cur.slot - 1 if cur.slot > n else cur.slot
            # the preset that will be active, by its slot before the move
            nxt_name = next(i.name for i in infos if i.slot == (nxt + 1 if nxt >= n else nxt))
            moves = (f"preset {last} moves up one slot" if n + 1 == last else
                     f"presets {n + 1}..{last} move up one slot" if n < last else "it is the last one in use")
            notes = [moves,
                     f"{nxt_name} ({nxt}) is active afterwards"]
            if cur.dirty:
                notes.append(f"the unsaved edits to {cur.name} are dropped")
            if not a.yes and not confirm(f"{name}: delete preset {n} ({gone})? " + "; ".join(notes) + "."):
                print("cancelled")
                return 1
            s.preset_delete(n)
            now = s.preset_info(0)
            print(f"{name}: preset {n} ({gone}) deleted; {last - 1} in use, preset {now.slot} ({now.name}) active")
            return 0
        if what == "move":
            if not gp.tracks_in_use(infos):
                raise GemifiError(f"{name}: this firmware can't move presets (no PRESET_MOVE); update it first")
            used = gp.presets_in_use(infos)
            n = slot_arg(infos, a.slot, in_use=True)
            t = a.to.strip().lower()
            to = n - 1 if t == "up" else n + 1 if t == "down" else int(t) if t.isdigit() else None
            if to is None or not 1 <= to <= len(used):
                raise GemifiError(f"{name}: move to {a.to!r}: give a slot 1..{len(used)}, up or down")
            moving = next(i.name for i in infos if i.slot == n)
            if to == n:
                print(f"{name}: preset {n} ({moving}) is already there")
                return 0
            if cur.dirty and not a.yes and not confirm(
                    f"{name}: moving presets drops the unsaved edits to {cur.name}."):
                print("cancelled")
                return 1
            s.preset_move(n, to)
            order = s.preset_infos()
            now = next(i for i in order if i.is_active)
            print(f"{name}: preset {moving} moved {n} -> {to}; preset {now.slot} ({now.name}) active")
            for i in gp.presets_in_use(order):
                print(f"  {'*' if i.is_active else ' '} {i.slot:2d}  {i.name}")
            return 0
        if what == "rename":
            n = slot_arg(infos, a.slot)
            gp_check_name(a.name)
            s.preset_name(n, a.name)
            print(f"{name}: preset {n} is now {s.preset_info(n).name!r}"
                  + (" (the firmware's name)" if a.name == "" else ""))
            return 0
    raise GemifiError(f"unknown preset command {what!r}")


def gp_check_name(name):
    try:
        gp.check_preset_name(name)
    except ValueError as e:
        raise GemifiError(str(e)) from None


# ---------------- presets around a firmware upload (sysex.md 4.5 "After a firmware upload") ----------------

def auto_backup_name(h):
    return f"gemifi-backup-{h.board_id:04x}-{gp.fmt_version(h.fw_version)}-{time.strftime('%Y%m%d-%H%M%S')}.json"


def flash_backup(a, s, h):
    """Before an upload, on an open app session: save every preset and the globals to a file, unless
    --no-backup. The default (no --backup) does it only on a device with presets, to an automatic name in
    the current directory. Returns the path or None."""
    if getattr(a, "no_backup", False):
        return None
    path = getattr(a, "backup", None)
    if path is None and not has_presets(h):
        return None
    if not h.caps & gp.CAP_CONFIG:
        if path:
            raise GemifiError("--backup: this firmware has no config to back up")
        return None
    path = path or auto_backup_name(h)
    doc = backup_doc(s, h, s.get_schema())
    write_backup(doc, path)
    print(f"backup: saved {backup_summary(doc)} to {path} (gemifi.py restore {path} puts it back)")
    return path


def ask_presets_choice(backup):
    undo = f" The backup {backup} can bring them back." if backup else ""
    try:
        ans = input(f"Keep your edits on top of the new presets, or use the firmware's presets (drops your "
                    f"edits and names).{undo} [K]eep / [u]se: ").strip().lower()
    except EOFError:
        ans = ""
    return "use" if ans in ("u", "use") else "keep"


def presets_after_upload(a, s, h, backup=None):
    """After an upload: if a slot's stored edits were made on another presets revision, keep them
    (--keep-presets, the default answer) or PRESET_RESET every slot (--use-firmware-presets). Asks when
    neither flag is given. Returns 'keep', 'use' or None (nothing to decide)."""
    if not has_presets(h):
        return None
    quiet_presets(s)
    infos = s.preset_infos()
    stale = [i for i in infos if i.stale]
    if not stale:
        return None
    print(f"The presets in this firmware changed (revision {infos[0].revision}). Your stored edits were made on "
          "another revision in: " + ", ".join(f"{i.slot} {i.name} (revision {i.stored_revision})" for i in stale))
    if getattr(a, "use_firmware_presets", False):
        choice = "use"
    elif getattr(a, "keep_presets", False):
        choice = "keep"
    else:
        choice = ask_presets_choice(backup)
    if choice == "use":
        s.preset_reset(gp.PRESET_ALL)
        print("presets: every slot reset to the firmware's presets" + (f" (undo: gemifi.py restore {backup})"
                                                                        if backup else ""))
    else:
        print("presets: kept your edits on top of the new presets")
    return choice


def is_gfw(img):
    return gi.find_hdr_offset(img) is not None


def ab_args(a):
    """--board and --version for a chip-format image (no .gfw header to take them from)."""
    if a.board is None or a.version is None:
        raise GemifiError(f"{a.file} has no .gfw header: give --board ID and --version X.Y.Z "
                          "(an A/B device such as an ESP32 takes the chip's own .bin)")
    if not 0 <= a.board <= 0x3FFF:
        raise GemifiError(f"--board 0x{a.board:x} doesn't fit a u14 (max 0x3FFF)")
    try:
        return a.board, gi.parse_version(a.version)
    except ValueError as e:
        raise GemifiError(f"--version {a.version}: {e}") from None


def cmd_flash(a, backend_factory):
    with open(a.file, "rb") as f:
        img = f.read()
    gfw = is_gfw(img)
    if (gfw and gi.parse_header(img, gi.find_hdr_offset(img)).board_id == mu.BOARD_ID) or \
            (not gfw and a.board == mu.BOARD_ID):
        if code61_route(a, backend_factory) == "vendor":
            return flash_vendor(a, backend_factory, img)
        if not gfw:
            raise GemifiError(f"{a.file}: the gemifi bootloader needs a .gfw image (mkimage.py), not a raw .bin")
    if gfw:
        try:
            hdr = gi.check_image(img, a.board)
        except gi.ImageError as e:
            raise GemifiError(f"{a.file}: not a valid .gfw: {e}") from None
        print(f"{a.file}: board 0x{hdr.board_id:04x}, version {gi.fmt_version(hdr.fw_version)}, "
              f"{hdr.image_size} bytes, crc32 0x{hdr.crc32:08x}")
    else:
        if not img:
            raise GemifiError(f"{a.file} is empty")
        board, version = ab_args(a)
        padded = gp.pad4(img)
        print(f"{a.file}: chip image, board 0x{board:04x}, version {gi.fmt_version(version)}, {len(img)} bytes"
              + (f" (padded to {len(padded)})" if len(padded) != len(img) else "")
              + f", crc32 0x{gp.ab_crc32(padded):08x}")
        if img[0] != 0xE9:
            print(f"  warning: first byte is 0x{img[0]:02x}, not 0xE9: this doesn't look like an ESP app image "
                  "(a merged factory image is for the first flash with esptool, not for OTA)", file=sys.stderr)
    if a.dry_run:
        if gfw:
            plan = gp.chunk_plan(hdr.image_size, a.max_chunk, board_max_size(hdr.board_id))
        else:
            plan = gp.linear_chunk_plan(len(padded), a.max_chunk)
        print(f"dry run: {len(plan)} FW_DATA chunks with max_chunk {a.max_chunk}"
              + ("" if gfw else " (A/B device: plain order from 0)") + ":")
        for off, n in plan:
            print(f"  0x{off:04x} +{n}")
        return 0

    backend = backend_factory()
    t0 = time.monotonic()
    boot_wait = a.wait if a.wait is not None else 6.0
    boot = None if getattr(backend, "fixed", False) else find_port(backend, "boot", a.boot_port)
    backup = app = None
    if boot and getattr(a, "backup", None) and not getattr(a, "no_backup", False):
        raise GemifiError(f"--backup: {boot} is the bootloader, so there is nothing to read; start the app first "
                          "or pass --no-backup")
    if not boot:
        app = find_port(backend, "app", a.port)
        if not app:
            raise GemifiError("no gemifi app or bootloader port found (try `gemifi.py ports`)")
        with backend.open(app) as port:
            s = Session(port, retries=a.retries, verbose=a.verbose)
            h = s.hello()
            if h.role == gp.ROLE_BOOT:
                if getattr(a, "backup", None) and not getattr(a, "no_backup", False):
                    raise GemifiError(f"--backup: {app} answers as the bootloader, so there is nothing to read")
                boot, app = app, None          # a serial port answering as a bootloader
            elif is_ab(h):
                if gfw:
                    raise GemifiError(f"{app} takes uploads in the app (A/B slots, no gemifi bootloader): give it "
                                      "the chip's own image (e.g. the ESP app .bin) with --board and --version, "
                                      "not a .gfw")
                backup = flash_backup(a, s, h)
                return flash_ab(a, backend, app, s, h, img, board, version, t0, backup)
            else:
                if not gfw:
                    raise GemifiError(f"{app} uploads through the gemifi bootloader, which needs a .gfw image "
                                      "(mkimage.py), not a raw .bin")
                if h.board_id != hdr.board_id:
                    raise GemifiError(f"device board_id 0x{h.board_id:04x} != image 0x{hdr.board_id:04x}")
                backup = flash_backup(a, s, h)
                print(f"{app}: app {gp.fmt_version(h.fw_version)}; rebooting into the bootloader")
                s.reboot(gp.REBOOT_BOOT)
        if app:
            boot = wait_port(backend, "boot", boot_wait, a.boot_port, gone=app)
    if app is None and not getattr(a, "no_backup", False):
        print("note: the device is in the bootloader, so no backup of its settings was made")

    if not gfw:
        raise GemifiError(f"{boot} is a gemifi bootloader, which needs a .gfw image (mkimage.py), not a raw .bin")
    with backend.open(boot) as port:
        s = Session(port, retries=a.retries, verbose=a.verbose)
        h = s.hello()
        print(f"{boot}: bootloader {gp.fmt_version(h.fw_version)}, max_chunk {h.max_chunk}")
        if h.role != gp.ROLE_BOOT:
            raise GemifiError("that port answered as the app, not the bootloader")
        if not h.caps & gp.CAP_FW:
            raise GemifiError("bootloader doesn't advertise firmware upload")
        if h.board_id != hdr.board_id:
            raise GemifiError(f"device board_id 0x{h.board_id:04x} != image 0x{hdr.board_id:04x}")
        max_chunk, n = upload(s, img, hdr, progress=None if a.quiet else bar)
        print(f"uploaded {hdr.image_size} bytes in {n} chunks (max_chunk {max_chunk}, {s.resends} resends); CRC OK")
        s.reboot(gp.REBOOT_APP)

    app = wait_port(backend, "app", boot_wait, a.port, gone=boot)
    with backend.open(app) as port:
        s = Session(port, retries=a.retries, on_unsolicited=print_unsolicited, verbose=a.verbose)
        h = s.hello()
        print_hello(app, h)
        presets_after_upload(a, s, h, backup)
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


def flash_ab(a, backend, name, s, h, img, board, version, t0, backup=None):
    """A/B upload on an open session to the app (sysex.md 4.1, 4.2 "A/B devices"): FW_* to the app itself,
    then REBOOT(0) (never REBOOT(1)), then wait for HELLO and check the new fw_version."""
    print(f"{name}: app {gp.fmt_version(h.fw_version)} takes uploads itself (max_chunk {h.max_chunk})")
    if h.board_id != board:
        raise GemifiError(f"device board_id 0x{h.board_id:04x} != --board 0x{board:04x}; not uploading")
    max_chunk, n, size, crc = upload_ab(s, img, board, version, progress=None if a.quiet else bar)
    print(f"uploaded {size} bytes in {n} chunks (max_chunk {max_chunk}, {s.resends} resends); CRC 0x{crc:08x} OK")
    s.reboot(gp.REBOOT_APP)
    s.port.close()
    new, h2 = wait_hello(backend, a.wait if a.wait is not None else 15.0, a.port, gone=name, retries=a.retries)
    print_hello(new, h2)
    if h2.fw_version != version:
        raise GemifiError(f"the device runs {gp.fmt_version(h2.fw_version)} after the reboot, expected "
                          f"{gp.fmt_version(version)} (wrong --version, or the new slot didn't boot and it fell back)")
    if has_presets(h2):
        with backend.open(new) as port:
            presets_after_upload(a, Session(port, retries=a.retries, on_unsolicited=print_unsolicited,
                                            verbose=a.verbose), h2, backup)
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


def code61_route(a, backend_factory):
    """How a Code 61 image goes on: "gemifi" (its gemifi bootloader, FW_* as on the NUC) or "vendor"
    (M-Audio's bootloader, maudio_update.py). --via picks; auto asks the device, as the web updater does:
    a gemifi bootloader port, or an app with HELLO caps bit 7, means gemifi; anything else (the stock app,
    a gemifi app on M-Audio's bootloader, or the vendor bootloader itself) means vendor."""
    via = getattr(a, "via", "auto") or "auto"
    if via != "auto":
        return via
    if a.dry_run:
        return "gemifi"
    backend = backend_factory()
    if getattr(backend, "fixed", False):
        return "gemifi"
    if find_port(backend, "boot"):
        return "gemifi"
    app = find_port(backend, "app", a.port)
    if not app:
        return "vendor"
    with backend.open(app) as port:
        try:
            h = Session(port, retries=1, verbose=a.verbose).hello()
        except GemifiError:
            return "vendor"
    if h.role == gp.ROLE_BOOT or h.caps & gp.CAP_GEMIFI_BOOT:
        return "gemifi"
    return "vendor"


# ---------------- Code 61: M-Audio's bootloader (maudio_update.py) ----------------

def vendor_kw(a):
    return {"ack_timeout": a.ack_timeout, "probe_timeout": a.probe_timeout, "gap": a.gap / 1000.0,
            "record_retries": a.record_retries, "restart": not a.no_restart,
            "log": lambda m: print("  " + m, file=sys.stderr)}


def flash_vendor(a, backend_factory, img):
    """Board 0x0002: build the vendor update stream from the image and send it through the stock bootloader.
    A running gemifi app is asked for update mode with REBOOT(1); a running stock app with the start message.
    Either way the keyboard then needs a power cycle."""
    try:
        app, desc = mu.app_from_file(a.file, img)
        if a.board is not None and a.board != mu.BOARD_ID:
            raise GemifiError(f"{a.file} is for board 0x{mu.BOARD_ID:04x}, not --board 0x{a.board:04x}")
        version = gi.check_image(img, mu.BOARD_ID).fw_version if is_gfw(img) else \
            (gi.parse_version(a.version) if a.version else None)
        recs = mu.build_stream(app)
    except (mu.UpdateError, ValueError) as e:
        raise GemifiError(str(e)) from None
    print(f"{a.file}: {desc}")
    return send_vendor(a, backend_factory, recs, version)


def cmd_restore_stock(a, backend_factory):
    try:
        recs = mu.parse_stream(mu.load_hex(a.file))
    except mu.UpdateError as e:
        raise GemifiError(f"{a.file}: {e}") from None
    print(f"{a.file}: vendor update file")
    return send_vendor(a, backend_factory, recs, None, stock=True)


def send_vendor(a, backend_factory, recs, version, stock=False):
    errs = mu.check_stream(recs)
    if errs:
        raise GemifiError("refusing to send: " + "; ".join(errs))
    for line in mu.summary(recs):
        print(line)
    if a.dry_run:
        print("dry run: Code 61 vendor update; nothing sent")
        return 0
    backend = backend_factory()
    t0 = time.monotonic()
    kw = vendor_kw(a)
    wait = a.wait if a.wait is not None else 120.0
    gm = None if getattr(backend, "fixed", False) else find_port(backend, "app", a.port)
    if gm:
        with backend.open(gm) as port:
            s = Session(port, retries=a.retries, verbose=a.verbose)
            h = s.hello()
            if h.board_id != mu.BOARD_ID:
                raise GemifiError(f"{gm} is board 0x{h.board_id:04x}, not the Code 61 (0x{mu.BOARD_ID:04x})")
            print(f"{gm}: gemifi {h.role_name} {gp.fmt_version(h.fw_version)}; asking for update mode (REBOOT 1)")
            s.reboot(gp.REBOOT_BOOT)
        name = mu.wait_power_cycle(backend, gm, wait, a.boot_port)
    else:
        name = mu.find_vendor_port(backend, a.boot_port)
        if not name:
            raise GemifiError("no Code 61 port found: neither a gemifi app nor a port named 'Code'/'M-Audio' "
                              "(try `gemifi.py ports`, or --boot-port NAME)")
    try:
        name = mu.enter_bootloader(backend, name, kw, wait, a.boot_port)
        with backend.open(name) as port:
            mu.send_stream(port, recs, kw, None if a.quiet else mu.bar)
    except mu.UpdateError as e:
        raise GemifiError(f"{e}. The bootloader keeps the update request, so power-cycle the keyboard and run "
                          "the same command again (or restore-stock).") from None
    if stock:
        print(f"done in {time.monotonic() - t0:.1f} s. The keyboard should restart on the stock firmware "
              "(power-cycle it if it doesn't).")
        return 0
    try:
        new, h = wait_hello(backend, a.after_wait, a.port, gone=name, retries=a.retries)
    except GemifiError:
        raise GemifiError(f"the update was accepted, but no gemifi app answered within {a.after_wait:.0f} s. "
                          "Power-cycle the keyboard, then run `gemifi.py hello`.") from None
    print_hello(new, h)
    if version is not None and h.fw_version != version:
        raise GemifiError(f"the device runs {gp.fmt_version(h.fw_version)}, expected {gp.fmt_version(version)}")
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


# ---------------- layout and raw readings (sysex.md 4.7, 4.8) ----------------

RAW_KIND_NAMES = {gp.EV_BUTTON: "buttons", gp.EV_KEY: "matrix", gp.EV_ANALOG: "analog", gp.EV_ENCODER: "encoders",
                  gp.EV_PAD: "pads"}
MONITOR_KINDS = {"buttons": gp.MON_BUTTONS, "matrix": gp.MON_MATRIX, "analog": gp.MON_ANALOG,
                 "encoders": gp.MON_ENCODERS, "pads": gp.MON_PADS}
MONITOR_RENEW_S = 2.0                # sysex.md 4.8: well inside the device's 5 s timeout


def cmd_layout(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        if not h.caps & gp.CAP_LAYOUT:
            raise GemifiError(f"{name}: this firmware has no layout (HELLO caps bit 8; the YAML has no layout:)")
        lay = gp.parse_layout(s.get_window_bytes(gp.GET_LAYOUT, "GET_LAYOUT"))
    if a.json:
        print(json.dumps({"width": lay.width, "height": lay.height,
                          "items": [dataclasses.asdict(it) | {"extra": it.extra.hex(" ")} for it in lay.items]},
                         indent=1))
        return 0
    print(f"{name}: {lay.width} x {lay.height} units, {len(lay.items)} items")
    for it in lay.items:
        what = f"{gp.KIND_NAMES.get(it.kind, f'kind{it.kind}')} {it.index}"
        extra = ""
        if it.shape == gp.SHAPE_KEYS:
            extra = f"  {it.n_keys} keys from {gp.note_name(it.first_note)} ({it.first_note})"
        elif it.shape == gp.SHAPE_XY:
            extra = f"  Y: analog {it.y_index}"
        print(f"  {what:<12} {gp.LAYOUT_SHAPES.get(it.shape, f'shape {it.shape}'):<12} at {it.x:>3},{it.y:<3} "
              f"{it.w} x {it.h}{extra}")
    return 0


def raw_text(m):
    """A RAW message as one line."""
    k = RAW_KIND_NAMES.get(m.kind, f"kind{m.kind}")
    if m.kind == gp.EV_KEY:
        rows = ["".join("#" if m.values[r * m.cols + c] else "." for c in range(m.cols)) for r in range(m.rows)]
        return f"{k:<8} rows {m.first}..{m.first + m.rows - 1}: " + " ".join(rows)
    if m.kind == gp.EV_BUTTON:
        down = [str(m.first + i) for i, v in enumerate(m.values) if v]
        return f"{k:<8} {m.first}..{m.first + len(m.values) - 1}: pressed " + (", ".join(down) or "none")
    vals = ["-" if m.kind == gp.EV_ANALOG and v == gp.RAW_NONE else str(v) for v in m.values]
    return f"{k:<8} {m.first}..{m.first + len(m.values) - 1}: " + " ".join(vals)


def cmd_monitor(a, backend):
    """Raw readings until Ctrl-C (or --seconds): a line for each message that differs from that kind's last."""
    name, port, s, h = config_session(a, backend)
    want = 0
    for k in a.kinds or list(MONITOR_KINDS):
        want |= MONITOR_KINDS[k]
    with port:
        if not h.caps & gp.CAP_MONITOR:
            raise GemifiError(f"{name}: this firmware has no raw readings (HELLO caps bit 9)")

        def start(kinds):
            f = expect_ok(s.request(gp.MONITOR, gp.req_monitor(kinds)), "MONITOR")
            return gp.parse_monitor_reply(f.payload)

        kinds, interval, _timeout = start(want)
        names = [n for n, b in MONITOR_KINDS.items() if kinds & b]
        print(f"{name}: raw {', '.join(names) or 'nothing'} every {interval} ms (Ctrl-C to stop)", file=sys.stderr)
        last, renewed, t0 = {}, time.monotonic(), time.monotonic()
        try:
            while a.seconds is None or time.monotonic() - t0 < a.seconds:
                if time.monotonic() - renewed >= MONITOR_RENEW_S:
                    start(want)
                    renewed = time.monotonic()
                for msg in port.recv(0.2):
                    if not gp.is_gemifi(msg):
                        continue
                    try:
                        f = gp.parse_frame(msg)
                        m = gp.parse_raw(f.payload) if f.cmd == gp.RAW else None
                    except gp.ProtocolError as e:
                        print(f"[bad frame: {e}]")
                        continue
                    if m is None:
                        continue
                    key = (m.kind, m.first)
                    if last.get(key) != m.values:
                        last[key] = m.values
                        print(raw_text(m), flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            start(0)
    return 0


# ---------------- host feedback (sysex.md 4.6) ----------------

def feedback_session(a, backend):
    """Open the app port and check it has host feedback (HELLO caps bit 6). Returns (name, port, session,
    hello); the caller closes the port. Notifications are never sent to a device without the bit (4.6.2)."""
    name, port, s = open_session(a, backend, "app")
    try:
        h = s.hello()
        if h.role != gp.ROLE_APP:
            raise GemifiError(f"{name} is the bootloader; host feedback needs the app (gemifi.py reboot app)")
        if not h.caps & gp.CAP_FEEDBACK:
            raise GemifiError(f"{name}: this firmware has no host feedback (HELLO caps 0x{h.caps:04x}, "
                              "bit 6 not set); nothing sent")
    except BaseException:
        port.close()
        raise
    return name, port, s, h


def print_components(comps):
    rows = [(c.kind_name, str(c.index), c.id, c.name, c.role, c.extras_text()) for c in comps]
    head = ("kind", "index", "id", "name", "role", "extras")
    w = [max(len(r[i]) for r in rows + [head]) for i in range(len(head))]

    def line(cells):
        print("  " + "  ".join(c.rjust(w[i]) if i == 1 else c.ljust(w[i]) for i, c in enumerate(cells)).rstrip())
    line(head)
    for r in rows:
        line(r)


def cmd_components(a, backend):
    name, port, s, h = feedback_session(a, backend)
    with port:
        comps = s.components()
    if a.json:
        print(json.dumps([c.to_json() for c in comps], indent=1))
        return 0
    print(name)
    if not comps:
        print("  (no components)")
        return 0
    print_components(comps)
    return 0


class Resolver:
    """Turns a command-line reference into a component index: a number is taken as the index; anything else
    is a component id (YAML `id:`, any case), looked up with GET_COMPONENT on first use."""

    def __init__(self, s):
        self.s, self.cache = s, {}

    def comps(self, kind):
        if kind not in self.cache:
            self.cache[kind] = self.s.components((kind,))
        return self.cache[kind]

    def by_id(self, kind, ref):
        return next((c for c in self.comps(kind) if c.id.lower() == ref.lower()), None)

    def index(self, kind, ref):
        t = ref.strip()
        if t.isdigit():
            return int(t)
        c = self.by_id(kind, t)
        if c is None:
            raise GemifiError(f"no {gp.KIND_NAMES[kind]} with id {ref!r} (gemifi.py components lists them)")
        return c.index

    def led(self, ref):
        """An LED by index or id; a button's id means its LED (an encoder's id: its centre LED)."""
        t = ref.strip()
        if t.isdigit():
            return int(t)
        c = self.by_id(gp.KIND_LED, t)
        if c is not None:
            return c.index
        for kind, attr in ((gp.KIND_BUTTON, "led"), (gp.KIND_ENCODER, "center")):
            c = self.by_id(kind, t)
            if c is not None:
                if getattr(c, attr) == gp.NONE_INDEX:
                    raise GemifiError(f"{c.kind_name} {c.id!r} has no LED")
                return getattr(c, attr)
        raise GemifiError(f"no LED, button or encoder with id {ref!r} (gemifi.py components lists them)")


LEVEL_WORDS = {"off": 0, "blink": 1, "on": 127}


def level_arg(text):
    """An LED level: 0..127, or off / blink / on."""
    t = text.strip().lower()
    if t in LEVEL_WORDS:
        return LEVEL_WORDS[t]
    try:
        return gp._int(t)
    except ValueError:
        raise GemifiError(f"level {text!r}: a number 0..127 or off, blink, on") from None


def int_arg(text, what):
    try:
        return gp._int(text.strip())
    except ValueError:
        raise GemifiError(f"{what} {text!r} is not a number") from None


def fb_payload(a, r):
    """(cmd, payload, description) for `gemifi.py fb ...`; resolves ids through `r` (a Resolver)."""
    w = a.fb_cmd
    if w == "led":
        led, level = r.led(a.led), level_arg(a.level)
        return gp.FB_LED, gp.fb_led(led, level), f"LED {led} = {level}"
    if w == "rgb":
        led = r.led(a.led)
        rgb = [int_arg(x, n) for x, n in ((a.r, "r"), (a.g, "g"), (a.b, "b"))]
        return (gp.FB_LED_RGB, gp.fb_led_rgb(led, *rgb, blink=a.blink),
                f"LED {led} = rgb({', '.join(map(str, rgb))})" + (" blinking" if a.blink else ""))
    if w == "leds":
        first, levels = r.led(a.first), [level_arg(x) for x in a.levels]
        return gp.FB_LEDS, gp.fb_leds(first, levels), f"LEDs {first}..{first + len(levels) - 1} = {levels}"
    if w == "value":
        kind = gp.KIND_ANALOG if a.kind == "analog" else gp.KIND_ENCODER
        i, v = r.index(kind, a.index), int_arg(a.value, "value")
        return gp.FB_VALUE, gp.fb_value(kind, i, v), f"{a.kind} {i} value = {v}"
    if w == "ring":
        i, v = r.index(gp.KIND_ENCODER, a.encoder), int_arg(a.value, "value")
        return (gp.FB_RING, gp.fb_ring(i, gp.RING_MODES[a.mode], v, a.center),
                f"encoder {i} ring {a.mode} = {v}" + (" + centre" if a.center else ""))
    if w == "text":
        i = r.index(gp.KIND_DISPLAY, a.display)
        row, col = int_arg(a.row, "row"), int_arg(a.col, "col")
        return gp.FB_TEXT, gp.fb_text(i, row, col, a.text), f"display {i} ({row}, {col}) = {a.text!r}"
    what = a.what or "all"
    return gp.FB_RELEASE, gp.fb_release(gp.RELEASE_WHAT[what]), f"release {what}"


def cmd_fb(a, backend):
    name, port, s, h = feedback_session(a, backend)
    with port:
        try:
            cmd, payload, desc = fb_payload(a, Resolver(s))
        except ValueError as e:
            raise GemifiError(str(e)) from None
        s.notify(cmd, payload)          # never answered: nothing to wait for
    print(f"{name}: {gp.CMD_NAMES[cmd]} {desc}")
    return 0


def add_vendor_options(p):
    mu.add_sender_options(p, wait=False)
    p.add_argument("--after-wait", type=float, default=20.0,
                   help="Code 61: seconds to wait for the gemifi app after the update (20)")


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["auto", "rtmidi", "mido", "alsa"], default="auto")
    ap.add_argument("--port", help="use the first port whose name contains this (default: name contains 'gemifi')")
    ap.add_argument("--retries", type=int, default=gp.MAX_RESENDS,
                    help="resends per request (same seq) after a timeout, BAD_CHECKSUM or BUSY (default 5 = 6 sends)")
    ap.add_argument("--serial", metavar="DEV", help="talk raw MIDI over this serial port instead (e.g. /dev/ttyUSB0; "
                    "classic ESP32/ESP8266 with GM_MIDI_SERIAL; needs pyserial)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate for --serial (default 115200)")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports")
    sub.add_parser("hello")
    lg = sub.add_parser("log")
    lg.add_argument("--names", action="store_true", help="read the schema first so PARAM_CHANGED shows names")
    r = sub.add_parser("reboot")
    r.add_argument("mode", choices=["app", "boot"])
    r.add_argument("--wait", type=float, default=0, help="seconds to wait for the new port (0 = don't)")
    f = sub.add_parser("flash")
    f.add_argument("file")
    f.add_argument("--board", type=lambda s: int(s, 0),
                   help=".gfw: refuse images for another board_id; .bin (A/B device): the board_id to send (required)")
    f.add_argument("--version", help=".bin (A/B device): the image's fw_version as X.Y.Z (required)")
    f.add_argument("--boot-port", help="substring for the bootloader port (default 'gemifi boot'; the Code 61 vendor "
                   "route: 'Code' or 'M-Audio')")
    f.add_argument("--wait", type=float, default=None,
                   help="seconds to wait for the device after a reboot (default 6; 15 for A/B devices, since an "
                        "ESP8266 copies the new image into place before it starts; Code 61 vendor route: 120 for "
                        "the power cycle)")
    f.add_argument("--via", choices=("auto", "gemifi", "vendor"), default="auto",
                   help="Code 61: the gemifi bootloader or M-Audio's (default auto: ask the device; a dry run "
                        "shows the gemifi plan)")
    f.add_argument("--dry-run", action="store_true", help="validate and print the chunk plan; no MIDI")
    f.add_argument("--max-chunk", type=int, default=256, help="max_chunk for --dry-run")
    f.add_argument("-q", "--quiet", action="store_true")
    f.add_argument("--backup", metavar="FILE.json",
                   help="save every preset and the globals to this file before the upload (default on a device "
                        "with presets: gemifi-backup-<board>-<version>-<time>.json in the current directory)")
    f.add_argument("--no-backup", action="store_true", help="don't save a backup before the upload")
    g = f.add_mutually_exclusive_group()
    g.add_argument("--keep-presets", action="store_true",
                   help="if the firmware's presets changed, keep your stored edits on top of them (no question)")
    g.add_argument("--use-firmware-presets", action="store_true",
                   help="if the firmware's presets changed, reset every slot to them (PRESET_RESET all)")
    add_vendor_options(f)
    f = sub.add_parser("restore-stock", help="Code 61: send an M-Audio update file (FirmwareFile*.hex.txt) as it is")
    f.add_argument("file", metavar="FirmwareFile.hex.txt")
    f.add_argument("--boot-port", help="substring for the keyboard's port (default 'Code' or 'M-Audio')")
    f.add_argument("--wait", type=float, default=None, help="seconds to wait for the power cycle (120)")
    f.add_argument("--dry-run", action="store_true", help="check the file and print a summary; no MIDI")
    f.add_argument("-q", "--quiet", action="store_true")
    add_vendor_options(f)
    c = sub.add_parser("schema", help="print the device's param schema")
    c.add_argument("--json", action="store_true", help="print the parsed schema as JSON")
    c.add_argument("--raw", metavar="OUT.bin", help="also save the raw schema bytes")
    c = sub.add_parser("get", help="read param values")
    c.add_argument("refs", nargs="*", metavar="NAME|ID[:INDEX]", help="(none = every value of every param)")
    c = sub.add_parser("set", help="set param values (live until commit)")
    c.add_argument("assignments", nargs="+", metavar="NAME[:INDEX]=VALUE",
                   help="VALUE: a number, an enum option label (any case), on/off, or a note name (60 = C4)")
    c.add_argument("--commit", action="store_true", help="store in flash afterwards")
    sub.add_parser("commit", help="store the current values in flash")
    c = sub.add_parser("factory-reset", help="all values back to defaults; erases the stored profile")
    c.add_argument("--yes", action="store_true", help="don't ask")
    c = sub.add_parser("backup", help="save every value by name to a JSON file")
    c.add_argument("file", metavar="OUT.json")
    c = sub.add_parser("restore", help="set values from a backup file, matched by name")
    c.add_argument("file", metavar="IN.json")
    c.add_argument("--commit", action="store_true", help="store in flash afterwards (presets are always stored)")
    sub.add_parser("presets", help="list the preset slots")
    c = sub.add_parser("preset", help="select, save, reset, rename, delete or move a preset")
    ps = c.add_subparsers(dest="preset_cmd", required=True)
    x = ps.add_parser("select", help="make a preset active (drops unsaved edits of the current one)")
    x.add_argument("slot", metavar="N|NAME")
    x.add_argument("--discard", action="store_true", help="don't ask when the current preset has unsaved edits")
    x = ps.add_parser("save", help="store the live values into a preset (default: the active one) and make it active")
    x.add_argument("slot", nargs="?", metavar="N|NAME")
    x.add_argument("--name", help="also rename it")
    x = ps.add_parser("reset", help="drop a preset's stored edits and name (back to the firmware's preset)")
    x.add_argument("slot", metavar="N|NAME|all")
    x.add_argument("--yes", action="store_true", help="don't ask")
    x = ps.add_parser("delete", help="remove a preset: the later ones move up one slot, the last slot is freed")
    x.add_argument("slot", metavar="N|NAME")
    x.add_argument("--yes", action="store_true", help="don't ask")
    x = ps.add_parser("move", help="reorder presets: one goes to slot TO, the ones between shift one slot")
    x.add_argument("slot", metavar="N|NAME")
    x.add_argument("to", metavar="TO|up|down", help="the slot it goes to (in use), or one step up or down")
    x.add_argument("--yes", action="store_true", help="don't ask when the active preset has unsaved edits")
    x = ps.add_parser("rename", help="give a preset a new name (\"\" = back to the firmware's name; a free slot "
                      "becomes a new preset, a copy of preset 1's firmware values)")
    x.add_argument("slot", metavar="N|NAME")
    x.add_argument("name", metavar="NAME", help=f"0..{gp.PRESET_NAME_MAX} printable ASCII characters")
    c = sub.add_parser("layout", help="print the device's physical layout (caps bit 8)")
    c.add_argument("--json", action="store_true", help="print it as JSON")
    c = sub.add_parser("monitor", help="print the raw readings: buttons, matrix, analog, encoders, pads (caps bit 9)")
    c.add_argument("kinds", nargs="*", choices=list(MONITOR_KINDS), metavar="KIND",
                   help="buttons, matrix, analog, encoders or pads (default: all)")
    c.add_argument("--seconds", type=float, help="stop after this long (default: Ctrl-C)")
    c = sub.add_parser("components", help="list the device's components (host feedback, caps bit 6)")
    c.add_argument("--json", action="store_true", help="print them as JSON")
    c = sub.add_parser("fb", help="send a host feedback notification (LEDs, values, rings, text)")
    fs = c.add_subparsers(dest="fb_cmd", required=True)
    x = fs.add_parser("led", help="set one LED's level")
    x.add_argument("led", metavar="LED", help="LED index, or the id of an LED, a button or an encoder (centre)")
    x.add_argument("level", metavar="LEVEL", help="0..127 (0 off, 1 blink, 2..127 on) or off/blink/on")
    x = fs.add_parser("rgb", help="set one LED's colour")
    x.add_argument("led", metavar="LED")
    for n in ("r", "g", "b"):
        x.add_argument(n, metavar=n.upper(), help="0..127")
    x.add_argument("--blink", action="store_true")
    x = fs.add_parser("leds", help="set several LEDs from FIRST on")
    x.add_argument("first", metavar="FIRST")
    x.add_argument("levels", nargs="+", metavar="LEVEL")
    x = fs.add_parser("value", help="set an analog input's pick-up target or an encoder's value")
    x.add_argument("kind", choices=["analog", "encoder"])
    x.add_argument("index", metavar="INDEX", help="index or component id")
    x.add_argument("value", metavar="VALUE", help="0..16383")
    x = fs.add_parser("ring", help="show a value on an encoder's LED ring")
    x.add_argument("encoder", metavar="ENCODER", help="index or component id")
    x.add_argument("mode", choices=list(gp.RING_MODES))
    x.add_argument("value", metavar="VALUE", help="0..16383")
    x.add_argument("--center", action="store_true", help="also light the centre LED")
    x = fs.add_parser("text", help="write text into a display")
    x.add_argument("display", metavar="DISPLAY", help="index or component id")
    x.add_argument("row", metavar="ROW")
    x.add_argument("col", metavar="COL")
    x.add_argument("text", metavar="TEXT", help=f"0..{gp.FB_TEXT_MAX} printable ASCII characters")
    x = fs.add_parser("release", help="hand feedback states back to the device")
    x.add_argument("what", nargs="?", choices=list(gp.RELEASE_WHAT), help="default all")
    return ap


COMMANDS = {"ports": cmd_ports, "hello": cmd_hello, "log": cmd_log, "reboot": cmd_reboot, "schema": cmd_schema,
            "get": cmd_get, "set": cmd_set, "commit": cmd_commit, "factory-reset": cmd_factory_reset,
            "backup": cmd_backup, "restore": cmd_restore, "presets": cmd_presets, "preset": cmd_preset,
            "components": cmd_components, "fb": cmd_fb, "layout": cmd_layout, "monitor": cmd_monitor}


def main(argv=None):
    a = build_parser().parse_args(argv)

    from gemifi_midi import MidiError, SerialBackend, get_backend

    def make_backend():
        return SerialBackend(a.serial, a.baud) if a.serial else get_backend(a.backend)
    try:
        if a.cmd == "flash":
            return cmd_flash(a, make_backend)
        if a.cmd == "restore-stock":
            return cmd_restore_stock(a, make_backend)
        backend = make_backend()
        return COMMANDS[a.cmd](a, backend)
    except (GemifiError, MidiError, gp.ProtocolError, OSError) as e:
        print(f"gemifi: ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
