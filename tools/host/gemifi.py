#!/usr/bin/env python3
"""gemifi host tool: talk to a gemifi device over USB MIDI SysEx (protocol/sysex.md).

  gemifi.py ports                     list MIDI ports (gemifi ones are marked)
  gemifi.py hello                     print HELLO from the device (app or bootloader)
  gemifi.py log                       print LOG / EVENT messages live (Ctrl-C to stop)
  gemifi.py reboot app|boot           ask the device to reboot into the app or the bootloader
  gemifi.py flash FILE.gfw            upload firmware through the bootloader
  gemifi.py flash FILE.gfw --dry-run  check the image and print the upload plan; no MIDI

Backends: python-rtmidi, then mido, then raw ALSA /dev/snd/midiC*D* (Linux, no dependencies).
Choose one with --backend. --port picks a port by substring instead of the "gemifi" name match.
"""
import argparse
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


# ---------------- request / reply session ----------------

class Session:
    """Stop-and-wait requests (sysex.md 1): send, wait for cmd|0x40 with the same seq (200 ms, or
    2000 ms for FW_BEGIN/FW_END); on a timeout, BAD_CHECKSUM or BUSY resend the identical frame (same
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


def expect_ok(f, what):
    if f.status != gp.OK:
        raise GemifiError(f"{what}: device replied {gp.STATUS.get(f.status, f'status {f.status}')}")
    return f


# ---------------- port discovery ----------------

def is_boot_name(n):
    return "gemifi boot" in n.lower()


def is_app_name(n):
    return "gemifi" in n.lower() and not is_boot_name(n)


def find_port(backend, kind, match=None):
    """kind: 'boot', 'app' or 'any' (app first). match: substring that overrides the name rules."""
    names = backend.ports()
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

def upload(sess, img, hdr, progress=None):
    """Run FW_BEGIN / FW_DATA... / FW_END on a session connected to the bootloader."""
    f = expect_ok(sess.request(gp.FW_BEGIN, gp.req_fw_begin(hdr.image_size, hdr.crc32, hdr.board_id,
                                                            hdr.fw_version)), "FW_BEGIN")
    max_chunk = gp.dec_u14(f.payload[1:3])
    try:
        if not gp.valid_max_chunk(max_chunk):
            raise GemifiError(f"device max_chunk {max_chunk} is not a multiple of 4 that divides 512")
        plan = gp.chunk_plan(hdr.image_size, max_chunk)
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


def print_unsolicited(f):
    try:
        if f.cmd == gp.LOG:
            print(gp.parse_log(f.payload), flush=True)
        elif f.cmd == gp.EVENT:
            print(gp.parse_event(f.payload), flush=True)
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
                            print_unsolicited(f)
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
        new = wait_port(backend, "boot" if mode else "app", a.wait, gone=name)
        print(f"up: {new}")
    return 0


def cmd_flash(a, backend_factory):
    with open(a.file, "rb") as f:
        img = f.read()
    try:
        hdr = gi.check_image(img, a.board)
    except gi.ImageError as e:
        raise GemifiError(f"{a.file}: not a valid .gfw: {e}") from None
    print(f"{a.file}: board 0x{hdr.board_id:04x}, version {gi.fmt_version(hdr.fw_version)}, "
          f"{hdr.image_size} bytes, crc32 0x{hdr.crc32:08x}")
    if a.dry_run:
        plan = gp.chunk_plan(hdr.image_size, a.max_chunk)
        print(f"dry run: {len(plan)} FW_DATA chunks with max_chunk {a.max_chunk}:")
        for off, n in plan:
            print(f"  0x{off:04x} +{n}")
        return 0

    backend = backend_factory()
    t0 = time.monotonic()
    boot = find_port(backend, "boot", a.boot_port)
    if not boot:
        app = find_port(backend, "app", a.port)
        if not app:
            raise GemifiError("no gemifi app or bootloader port found (try `gemifi.py ports`)")
        with backend.open(app) as port:
            s = Session(port, retries=a.retries, verbose=a.verbose)
            h = s.hello()
            if h.board_id != hdr.board_id:
                raise GemifiError(f"device board_id 0x{h.board_id:04x} != image 0x{hdr.board_id:04x}")
            print(f"{app}: app {gp.fmt_version(h.fw_version)}; rebooting into the bootloader")
            s.reboot(gp.REBOOT_BOOT)
        boot = wait_port(backend, "boot", a.wait, a.boot_port, gone=app)

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

    app = wait_port(backend, "app", a.wait, a.port, gone=boot)
    with backend.open(app) as port:
        print_hello(app, Session(port, retries=a.retries).hello())
    print(f"done in {time.monotonic() - t0:.1f} s")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["auto", "rtmidi", "mido", "alsa"], default="auto")
    ap.add_argument("--port", help="use the first port whose name contains this (default: name contains 'gemifi')")
    ap.add_argument("--retries", type=int, default=gp.MAX_RESENDS,
                    help="resends per request (same seq) after a timeout, BAD_CHECKSUM or BUSY (default 5 = 6 sends)")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports")
    sub.add_parser("hello")
    sub.add_parser("log")
    r = sub.add_parser("reboot")
    r.add_argument("mode", choices=["app", "boot"])
    r.add_argument("--wait", type=float, default=0, help="seconds to wait for the new port (0 = don't)")
    f = sub.add_parser("flash")
    f.add_argument("file")
    f.add_argument("--board", type=lambda s: int(s, 0), help="refuse images for another board_id")
    f.add_argument("--boot-port", help="substring for the bootloader port (default 'gemifi boot')")
    f.add_argument("--wait", type=float, default=6.0, help="seconds to wait for re-enumeration")
    f.add_argument("--dry-run", action="store_true", help="validate and print the chunk plan; no MIDI")
    f.add_argument("--max-chunk", type=int, default=256, help="max_chunk for --dry-run")
    f.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args(argv)

    from gemifi_midi import get_backend, MidiError
    try:
        if a.cmd == "flash":
            return cmd_flash(a, lambda: get_backend(a.backend))
        backend = get_backend(a.backend)
        return {"ports": cmd_ports, "hello": cmd_hello, "log": cmd_log, "reboot": cmd_reboot}[a.cmd](a, backend)
    except (GemifiError, MidiError, gp.ProtocolError, OSError) as e:
        print(f"gemifi: ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
