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

Config (app firmware, sysex.md 4.3):
  gemifi.py schema [--json] [--raw OUT.bin]   print the device's param schema
  gemifi.py get [NAME|ID[:INDEX]]...          read values (no args: every value of every param)
  gemifi.py set NAME[:INDEX]=VALUE... [--commit]
                                      set values live; VALUE is a number, an enum option label
                                      (any case), on/off for bool, or a note name (MIDI 60 = C4)
  gemifi.py commit                    store the current values in flash
  gemifi.py factory-reset [--yes]     back to defaults and erase the stored profile
  gemifi.py backup OUT.json           save every value by name
  gemifi.py restore IN.json [--commit]

Backends: python-rtmidi, then mido, then raw ALSA /dev/snd/midiC*D* (Linux, no dependencies).
Choose one with --backend. --port picks a port by substring instead of the "gemifi" name match.
--serial /dev/ttyUSB0 [--baud 115200] talks raw MIDI bytes over a serial port instead (classic ESP32 and
ESP8266 builds with GM_MIDI_SERIAL; needs pyserial). Every command works over it.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402


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
                    if gp.reply_matches(f, cmd, seq):
                        if f.status in (gp.BAD_CHECKSUM, gp.BUSY):
                            resend = True      # not completed on the device: same seq again
                            if f.status == gp.BUSY:
                                time.sleep(min(self.timeout, 0.05))
                            break
                        return f
                    if self.verbose:
                        print(f"  ignoring frame cmd 0x{f.cmd:02x} seq {f.seq}", file=sys.stderr)
                if resend:
                    break
        raise NoReply(f"no reply to {gp.CMD_NAMES.get(cmd, hex(cmd))} after {tries} tries")

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
        data = bytearray()
        total = None
        while total is None or len(data) < total:
            f = expect_ok(self.request(gp.GET_SCHEMA, gp.req_get_schema(len(data), window)), "GET_SCHEMA")
            t, off, chunk = gp.parse_schema_reply(f.payload)
            if off != len(data):
                raise GemifiError(f"GET_SCHEMA at {len(data)}: device echoed offset {off}")
            if total is not None and t != total:
                raise GemifiError(f"GET_SCHEMA: total changed from {total} to {t} mid-read")
            total = t
            if len(chunk) > window or (not chunk and len(data) < total) or len(data) + len(chunk) > total:
                raise GemifiError(f"GET_SCHEMA at {len(data)}: {len(chunk)} bytes returned (total {total})")
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


def upload(sess, img, hdr, progress=None):
    """Run FW_BEGIN / FW_DATA... / FW_END on a session connected to the bootloader (sysex.md 4.2):
    0x200 upward, page 0 last, no chunk across a 512-byte page."""
    def plan(max_chunk):
        if not gp.valid_max_chunk(max_chunk):
            raise GemifiError(f"device max_chunk {max_chunk} is not a multiple of 4 that divides 512")
        return gp.chunk_plan(hdr.image_size, max_chunk)
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
    print(f"  board_id    0x{h.board_id:04x} ({gi.BOARDS.get(h.board_id, ('unknown',))[0]})")
    print(f"  fw_version  {gp.fmt_version(h.fw_version)}")
    print(f"  protocol    {h.proto_version}")
    caps = [n for b, n in ((gp.CAP_FW, "fw-upload"), (gp.CAP_CONFIG, "config"), (gp.CAP_LOG, "log"),
                           (gp.CAP_EVENT, "event")) if h.caps & b]
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
                print(pad + "options: " + ", ".join(f"{p.min + i}={o}" for i, o in enumerate(p.options)))
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


BACKUP_FORMAT = 1


def cmd_backup(a, backend):
    name, port, s, h = config_session(a, backend)
    with port:
        schema = s.get_schema()
        values = {}
        for p in schema.params:
            vals = s.get_values(p)
            values[p.name] = vals[0] if p.count == 1 else vals
    doc = {"gemifi_backup": BACKUP_FORMAT, "board_id": h.board_id, "fw_version": gp.fmt_version(h.fw_version),
           "values": values}
    with open(a.file, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"{name}: saved {len(values)} params ({sum(p.count for p in schema.params)} values) to {a.file}")
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


def cmd_restore(a, backend):
    with open(a.file) as f:
        doc = json.load(f)
    values = doc.get("values") if isinstance(doc, dict) else None
    if not isinstance(values, dict):
        raise GemifiError(f"{a.file}: not a gemifi backup (no \"values\" object)")
    name, port, s, h = config_session(a, backend)
    with port:
        if "board_id" in doc and doc["board_id"] != h.board_id:
            print(f"  warning: backup is from board 0x{doc['board_id']:04x}, this is 0x{h.board_id:04x}; "
                  "matching by name anyway")
        schema = s.get_schema()
        n_set, n_err = restore_values(s, schema, values)
        print(f"{name}: restored {n_set} values" + (f", {n_err} errors" if n_err else ""))
        maybe_commit(s, a, n_set)
    return 1 if n_err else 0


def is_gfw(img):
    return len(img) >= gi.HDR_OFFSET + 4 and img[gi.HDR_OFFSET:gi.HDR_OFFSET + 4] == b"GMFI"


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
            plan = gp.chunk_plan(hdr.image_size, a.max_chunk)
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
    if not boot:
        app = find_port(backend, "app", a.port)
        if not app:
            raise GemifiError("no gemifi app or bootloader port found (try `gemifi.py ports`)")
        with backend.open(app) as port:
            s = Session(port, retries=a.retries, verbose=a.verbose)
            h = s.hello()
            if h.role == gp.ROLE_BOOT:
                boot, app = app, None          # a serial port answering as a bootloader
            elif is_ab(h):
                if gfw:
                    raise GemifiError(f"{app} takes uploads in the app (A/B slots, no gemifi bootloader): give it "
                                      "the chip's own image (e.g. the ESP app .bin) with --board and --version, "
                                      "not a .gfw")
                return flash_ab(a, backend, app, s, h, img, board, version, t0)
            else:
                if not gfw:
                    raise GemifiError(f"{app} uploads through the gemifi bootloader, which needs a .gfw image "
                                      "(mkimage.py), not a raw .bin")
                if h.board_id != hdr.board_id:
                    raise GemifiError(f"device board_id 0x{h.board_id:04x} != image 0x{hdr.board_id:04x}")
                print(f"{app}: app {gp.fmt_version(h.fw_version)}; rebooting into the bootloader")
                s.reboot(gp.REBOOT_BOOT)
        if app:
            boot = wait_port(backend, "boot", boot_wait, a.boot_port, gone=app)

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
        print_hello(app, Session(port, retries=a.retries).hello())
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


def flash_ab(a, backend, name, s, h, img, board, version, t0):
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
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


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
    f.add_argument("--boot-port", help="substring for the bootloader port (default 'gemifi boot')")
    f.add_argument("--wait", type=float, default=None,
                   help="seconds to wait for the device after a reboot (default 6; 15 for A/B devices, since an "
                        "ESP8266 copies the new image into place before it starts)")
    f.add_argument("--dry-run", action="store_true", help="validate and print the chunk plan; no MIDI")
    f.add_argument("--max-chunk", type=int, default=256, help="max_chunk for --dry-run")
    f.add_argument("-q", "--quiet", action="store_true")
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
    c.add_argument("--commit", action="store_true", help="store in flash afterwards")
    return ap


COMMANDS = {"ports": cmd_ports, "hello": cmd_hello, "log": cmd_log, "reboot": cmd_reboot, "schema": cmd_schema,
            "get": cmd_get, "set": cmd_set, "commit": cmd_commit, "factory-reset": cmd_factory_reset,
            "backup": cmd_backup, "restore": cmd_restore}


def main(argv=None):
    a = build_parser().parse_args(argv)

    from gemifi_midi import MidiError, SerialBackend, get_backend

    def make_backend():
        return SerialBackend(a.serial, a.baud) if a.serial else get_backend(a.backend)
    try:
        if a.cmd == "flash":
            return cmd_flash(a, make_backend)
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
