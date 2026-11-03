#!/usr/bin/env python3
"""M-Audio Code series (Code 61, board 0x0002): the stock bootloader's SysEx update protocol.

gemifi doesn't replace the bootloader on these keyboards. It replaces only the app (0x08002400 and up) through
M-Audio's own update protocol (research/devices/m-audio-code61/analysis/update-protocol.md and
custom-firmware-path.md). This module builds that update stream from a gemifi image and sends it.

  maudio_update.py info  FILE                  summary of an update file, a .gfw or a .bin; checks every checksum
  maudio_update.py build IN.gfw|IN.bin OUT.hex.txt   update file in the vendor format
  maudio_update.py syx   IN OUT.syx            the exact SysEx messages the updater sends
  maudio_update.py send  IN [--port NAME] [--dry-run]   send to the keyboard's bootloader
  maudio_update.py request [--port NAME]       ask the running stock app for update mode, then power-cycle

IN is a vendor update file (FirmwareFile*.hex.txt, sent as it is), a gemifi .gfw for board 0x0002, or a raw
app .bin linked at 0x08002400.

The update stream
-----------------
The file is the whole stream as one line of hex. It is a list of records:

  +0   u16 BE   length of the rest of the record
  +2   6A 04 DA 3B                       record signature
  +6   commands, 6 bytes each: op, offset u24 BE (from 0x08000000), length u16 BE (bytes);
       op 0A (load) is followed by `length` data bytes
  -6   00 00 00 00
  -2   u16 LE   checksum = (-sum(record[2:-2])) & 0xFFFF

  op 09 erase the 1 KB page at offset    0A load data into the buffer     0B program the buffer at offset
  op 0D verify? (only in the last record)    0E finish? (only in the last record)

(The research notes read the command as op, page u16 BE, count u16 BE x 256. For page-aligned values the two
readings give the same bytes; this module only builds page-aligned commands.)

Stock layout, mirrored by build_stream():
  one data record per 1 KB page that holds the app (09 erase, 0A load 1024, 0B program),
  one erase-only record (09) per page after the app up to 0x0801AFFF,
  and the last record for page 0x0801B000: FF except the image checksum at 0x0801B3FE, then 0B, 0D, 0E.
The image checksum is the negated 16-bit sum of 0x08002400..0x0801B3FD (unwritten flash counts as FF),
little-endian, so it is written last.

On the wire: the start message F0 5A 57 6E 28 3C 4E 45 F7, then each record as SysEx messages of up to
11 record bytes in uppercase ASCII hex (F0 + 22 chars + F7), 3 ms apart. After the last message of a record
the bootloader answers F0 cc F7: 45 or 1A = OK, 15 = NAK. The stock app answers the start message with
F0 15 F7 and sets the update-request flag (8 bytes 55 5A A5 AA 59 95 3A A3 at 0x0801FC00); the bootloader
acts on it at the next power-on.

Standard library only (plus gemifi_midi's backends for `send` and `request`).
"""
import argparse
import os
import struct
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemifi_image as gi  # noqa: E402

BOARD_ID = 0x0002
FLASH = 0x08000000
APP_BASE = 0x08002400
PAGE = 0x400
CK_PAGE = 0x0801B000            # last page written; holds the image checksum
CK_ADDR = 0x0801B3FE            # image checksum, u16 LE
APP_MAX = CK_ADDR - APP_BASE    # 102,398 bytes
SETTINGS = 0x0801B400           # vendor settings "EEPROM"; never written by an update
FLAG_PAGE = 0x0801FC00          # update-request flag page (written by the app, not by the host)
FLAG_MAGIC = bytes.fromhex("555aa5aa59953aa3")

SIGNATURE = bytes.fromhex("6a04da3b")
END = bytes(4)
START = bytes.fromhex("f05a576e283c4e45f7")     # ASCII "ZWn(<NE" in SysEx, no manufacturer ID
CHUNK = 11                                      # record bytes per SysEx message

OP_ERASE, OP_LOAD, OP_PROGRAM, OP_VERIFY, OP_FINISH = 0x09, 0x0A, 0x0B, 0x0D, 0x0E
OP_NAMES = {OP_ERASE: "erase", OP_LOAD: "load", OP_PROGRAM: "program", OP_VERIFY: "verify?", OP_FINISH: "finish?"}

R_IN_UPDATE, R_OK, R_OK2, R_NAK = 0x20, 0x45, 0x1A, 0x15
REPLIES = {R_IN_UPDATE: "in update mode", R_OK: "OK", R_OK2: "OK (1A)",
           R_NAK: "NAK (from the app: update requested, power-cycle)"}

# Timing from the stock updater (update-protocol.md).
PROBE_TIMEOUT_S = 2.0
ACK_TIMEOUT_S = 10.0
GAP_S = 0.003


class UpdateError(ValueError):
    pass


# ---------------- records ----------------

@dataclass
class Cmd:
    op: int
    addr: int           # absolute flash address (FLASH + the u24 offset; 0x08000000 for a load)
    length: int         # bytes
    data: bytes = b""   # OP_LOAD only

    def pack(self):
        off = self.addr - FLASH
        if not 0 <= off < 1 << 24 or not 0 <= self.length < 1 << 16:
            raise UpdateError(f"command {self.op:02x}: address 0x{self.addr:08x} / length {self.length} out of range")
        return bytes([self.op]) + off.to_bytes(3, "big") + self.length.to_bytes(2, "big") + self.data

    def __str__(self):
        a = f" 0x{self.addr:08x}" if self.op != OP_LOAD else ""
        return f"{OP_NAMES.get(self.op, f'op{self.op:02x}')}{a}" + (f" +{self.length}" if self.length else "")


@dataclass
class Record:
    cmds: list = field(default_factory=list)

    def body(self):
        return SIGNATURE + b"".join(c.pack() for c in self.cmds) + END

    def pack(self):
        body = self.body()
        return struct.pack(">H", len(body) + 2) + body + struct.pack("<H", (-sum(body)) & 0xFFFF)

    @property
    def addr(self):
        return self.cmds[0].addr if self.cmds else 0

    def kind(self):
        ops = [c.op for c in self.cmds]
        if ops == [OP_ERASE]:
            return "erase"
        if OP_FINISH in ops:
            return "final"
        if ops == [OP_ERASE, OP_LOAD, OP_PROGRAM]:
            return "data"
        return "other"


def checksum(rec):
    """Per-record checksum: negated 16-bit sum of every byte after the length field, without the checksum."""
    return (-sum(rec[2:-2])) & 0xFFFF


def split_records(blob):
    """Cut a decoded update stream into raw records (each starts with its length - 2, big-endian)."""
    out, i = [], 0
    while i < len(blob):
        if i + 2 > len(blob):
            raise UpdateError(f"stream ends inside a length field at byte {i}")
        n = struct.unpack_from(">H", blob, i)[0] + 2
        if n < 2 + len(SIGNATURE) + len(END) + 2 or i + n > len(blob):
            raise UpdateError(f"record at byte {i}: bad length {n}")
        out.append(bytes(blob[i:i + n]))
        i += n
    return out


def parse_record(rec):
    """Raw record bytes -> Record. Checks the length, signature, command list and checksum."""
    if len(rec) < 14 or struct.unpack_from(">H", rec, 0)[0] + 2 != len(rec):
        raise UpdateError("record length field doesn't match its size")
    if rec[2:6] != SIGNATURE:
        raise UpdateError(f"record signature {rec[2:6].hex()} is not {SIGNATURE.hex()}")
    want = struct.unpack_from("<H", rec, len(rec) - 2)[0]
    if want != checksum(rec):
        raise UpdateError(f"record checksum 0x{want:04x}, computed 0x{checksum(rec):04x}")
    if rec[-6:-2] != END:
        raise UpdateError("record doesn't end with 00 00 00 00 before its checksum")
    r, i, end = Record(), 6, len(rec) - 6
    while i < end:
        if i + 6 > end:
            raise UpdateError(f"command at record byte {i} runs past the end")
        op, off, n = rec[i], int.from_bytes(rec[i + 1:i + 4], "big"), int.from_bytes(rec[i + 4:i + 6], "big")
        i += 6
        data = b""
        if op == OP_LOAD:
            if i + n > end:
                raise UpdateError(f"load of {n} bytes runs past the end of the record")
            data = rec[i:i + n]
            i += n
        elif op not in OP_NAMES:
            raise UpdateError(f"unknown command op 0x{op:02x}")
        r.cmds.append(Cmd(op, FLASH + off, n, data))
    return r


def parse_stream(blob):
    """Decoded stream -> [Record]; raises UpdateError naming the record that's wrong."""
    out = []
    for k, raw in enumerate(split_records(blob)):
        try:
            out.append(parse_record(raw))
        except UpdateError as e:
            raise UpdateError(f"record {k}: {e}") from None
    return out


def pack_stream(records):
    return b"".join(r.pack() for r in records)


def load_hex(path):
    """A vendor FirmwareFile (one line of hex) -> decoded bytes."""
    with open(path) as f:
        text = "".join(f.read().split())
    try:
        return bytes.fromhex(text)
    except ValueError:
        raise UpdateError(f"{path}: not a hex update file") from None


def is_hex_file(data):
    head = data[:64].strip()
    return bool(head) and all(chr(c) in "0123456789abcdefABCDEF \r\n\t" for c in head)


# ---------------- flash image ----------------

def apply(records, flash=None):
    """Run the records against a flash model {page address: bytearray(1024)}, as the bootloader would (as far
    as we know it). Returns the flash dict. Pages never written stay absent (= erased FF)."""
    flash = {} if flash is None else flash
    for r in records:
        buf = b""
        for c in r.cmds:
            if c.op == OP_ERASE:
                flash[c.addr] = bytearray(b"\xff" * PAGE)
            elif c.op == OP_LOAD:
                buf = c.data
            elif c.op == OP_PROGRAM:
                for i in range(0, c.length, PAGE):
                    pg = flash.setdefault(c.addr + i, bytearray(b"\xff" * PAGE))
                    pg[:] = buf[i:i + PAGE].ljust(PAGE, b"\xff")
    return flash


def read(flash, addr, n):
    out = bytearray()
    while n > 0:
        pg = addr & ~(PAGE - 1)
        o = addr - pg
        k = min(n, PAGE - o)
        out += flash[pg][o:o + k] if pg in flash else b"\xff" * k
        addr += k
        n -= k
    return bytes(out)


def image_checksum(app):
    """Checksum stored at 0x0801B3FE for an app image loaded at 0x08002400 (rest of the range = FF)."""
    if len(app) > APP_MAX:
        raise UpdateError(f"app is {len(app)} bytes; the most that fits is {APP_MAX}")
    return (-(sum(app) + 0xFF * (APP_MAX - len(app)))) & 0xFFFF


def flash_checksum(flash):
    """(computed, stored) image checksum of a flash model."""
    return image_checksum(read(flash, APP_BASE, APP_MAX)), struct.unpack("<H", read(flash, CK_ADDR, 2))[0]


def app_from_records(records):
    """The app bytes a stream writes: from 0x08002400 up to the end of the last data record."""
    flash = apply(records)
    data_pages = [r.addr for r in records if r.kind() == "data"]
    if not data_pages:
        return b""
    return read(flash, APP_BASE, max(data_pages) + PAGE - APP_BASE)


# ---------------- building ----------------

def _data_record(addr, page):
    return Record([Cmd(OP_ERASE, addr, 0), Cmd(OP_LOAD, FLASH, PAGE, page), Cmd(OP_PROGRAM, addr, PAGE)])


def build_stream(app):
    """App image (linked at 0x08002400, at most 102,398 bytes) -> [Record] laid out like the stock file."""
    app = bytes(app)
    if not app:
        raise UpdateError("app image is empty")
    if len(app) > APP_MAX:
        raise UpdateError(f"app is {len(app)} bytes; the app area ends at 0x{CK_ADDR - 1:08x} ({APP_MAX} bytes)")
    recs = []
    end = APP_BASE + len(app)
    for addr in range(APP_BASE, CK_PAGE, PAGE):
        if addr < end:
            recs.append(_data_record(addr, app[addr - APP_BASE:addr - APP_BASE + PAGE].ljust(PAGE, b"\xff")))
        else:
            recs.append(Record([Cmd(OP_ERASE, addr, 0)]))
    last = bytearray(app[CK_PAGE - APP_BASE:].ljust(PAGE, b"\xff"))
    last[CK_ADDR - CK_PAGE:] = struct.pack("<H", image_checksum(app))
    recs.append(Record([Cmd(OP_ERASE, CK_PAGE, 0), Cmd(OP_LOAD, FLASH, PAGE, bytes(last)), Cmd(OP_PROGRAM, CK_PAGE, PAGE),
                        Cmd(OP_VERIFY, CK_PAGE, 0), Cmd(OP_FINISH, CK_PAGE, 0)]))
    return recs


def check_stream(records):
    """Safety checks before sending: only the app area and the checksum page are touched, and the image
    checksum is right. Returns a list of problems (empty = fine)."""
    errs = []
    for k, r in enumerate(records):
        for c in r.cmds:
            if c.op in (OP_ERASE, OP_PROGRAM):
                lo, hi = c.addr, c.addr + max(c.length, PAGE)
                if lo % PAGE or lo < APP_BASE or hi > SETTINGS:
                    errs.append(f"record {k}: {c} is outside 0x{APP_BASE:08x}..0x{SETTINGS - 1:08x}")
    if not records or records[-1].kind() != "final":
        errs.append("the last record doesn't finish the update (0E)")
    else:
        calc, stored = flash_checksum(apply(records))
        if calc != stored:
            errs.append(f"image checksum at 0x{CK_ADDR:08x} is 0x{stored:04x}, should be 0x{calc:04x}")
    return errs


def app_from_file(path, data=None):
    """A gemifi .gfw for board 0x0002, or a raw .bin linked at 0x08002400 -> (app bytes, description)."""
    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    off = gi.find_hdr_offset(data)
    if off is not None:
        try:
            h = gi.check_image(data, BOARD_ID)
        except gi.ImageError as e:
            raise UpdateError(f"{path}: not a valid .gfw for the Code 61 (board 0x{BOARD_ID:04x}): {e}") from None
        return data, (f"gemifi .gfw, board 0x{h.board_id:04x}, version {gi.fmt_version(h.fw_version)}, "
                      f"{h.image_size} bytes, crc32 0x{h.crc32:08x}")
    if not data:
        raise UpdateError(f"{path} is empty")
    errs = gi.check_vectors(data, len(data), APP_BASE, gi.BOARDS[BOARD_ID].sram_end) if len(data) >= 8 else ["too short"]
    if errs:
        raise UpdateError(f"{path}: doesn't look like an app linked at 0x{APP_BASE:08x}: {'; '.join(errs)}")
    return data, f"raw app image, {len(data)} bytes"


def load_input(path):
    """Any input file -> (records, description, is_vendor_file)."""
    with open(path, "rb") as f:
        data = f.read()
    if is_hex_file(data):
        recs = parse_stream(load_hex(path))
        return recs, f"vendor update file, {len(recs)} records", True
    app, desc = app_from_file(path, data)
    return build_stream(app), desc, False


def summary(records):
    kinds = {}
    for r in records:
        kinds[r.kind()] = kinds.get(r.kind(), 0) + 1
    data = [r.addr for r in records if r.kind() == "data"]
    calc, stored = flash_checksum(apply(records))
    msgs = sum(-(-len(r.pack()) // CHUNK) for r in records) + 1
    lines = [f"  {len(records)} records: " + ", ".join(f"{n} {k}" for k, n in kinds.items())]
    if data:
        lines.append(f"  data pages 0x{min(data):08x}..0x{max(data) + PAGE - 1:08x}")
    lines.append(f"  image checksum 0x{stored:04x} (computed 0x{calc:04x}{', OK' if calc == stored else ', WRONG'})")
    lines.append(f"  {msgs} SysEx messages ({len(records)} acks)")
    return lines


# ---------------- wire ----------------

def record_messages(rec):
    """One record -> its SysEx messages (F0 + up to 22 uppercase hex chars + F7)."""
    return [b"\xf0" + rec[i:i + CHUNK].hex().upper().encode() + b"\xf7" for i in range(0, len(rec), CHUNK)]


def syx_messages(records):
    """Every message the stock updater sends, in order: start, then the records."""
    out = [START]
    for r in records:
        out += record_messages(r.pack())
    return out


def reply_code(msg):
    msg = bytes(msg)
    return msg[1] if len(msg) == 3 and msg[0] == 0xF0 and msg[2] == 0xF7 else None


class Sender:
    """Drives the vendor protocol on an open port (gemifi_midi Port: send(bytes), recv(timeout) -> [sysex]).

    ok_codes: replies that count as an ack (45 and 1A; what each one means isn't known).
    record_retries: resends of a whole record after a NAK or a timeout. The stock updater never resends, so the
    default is 0. A resend is only plausible because every record starts by erasing its own page.
    restart: after the probe is answered, send the start message once more and wait for its ack (10 s) before
    the records, as the updater appears to do (probe state, then "start update"). False = records right after
    the probe ack.
    """

    def __init__(self, port, ack_timeout=ACK_TIMEOUT_S, probe_timeout=PROBE_TIMEOUT_S, gap=GAP_S,
                 ok_codes=(R_OK, R_OK2), record_retries=0, restart=True, log=None):
        self.port, self.ack_timeout, self.probe_timeout, self.gap = port, ack_timeout, probe_timeout, gap
        self.ok_codes, self.record_retries, self.restart = tuple(ok_codes), record_retries, restart
        self.log = log or (lambda s: None)
        self.seen = []          # every reply code received, in order
        self.resends = 0

    def wait_reply(self, timeout):
        """First F0 cc F7 other than the unsolicited 20, or None on a timeout."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            for m in self.port.recv(left):
                c = reply_code(m)
                if c is None:
                    continue        # other SysEx (a gemifi LOG, an identity reply): not ours
                self.seen.append(c)
                if c == R_IN_UPDATE:
                    self.log("device says: in update mode (F0 20 F7)")
                    continue
                return c

    def probe(self, timeout=None):
        """Send the start message once; return the reply code or None."""
        self.port.send(START)
        return self.wait_reply(self.probe_timeout if timeout is None else timeout)

    def start(self):
        if not self.restart:
            return
        c = self.probe(self.ack_timeout)
        if c not in self.ok_codes:
            raise UpdateError("bootloader didn't acknowledge the start message"
                              + (f" (replied {c:02x})" if c is not None else " (no reply in "
                                 f"{self.ack_timeout:.0f} s)"))

    def send_record(self, raw):
        msgs = record_messages(raw)
        for attempt in range(self.record_retries + 1):
            if attempt:
                self.resends += 1
            for i, m in enumerate(msgs):
                if i and self.gap:
                    time.sleep(self.gap)
                self.port.send(m)
            c = self.wait_reply(self.ack_timeout)
            if c in self.ok_codes:
                return c
            why = "no reply" if c is None else "NAK" if c == R_NAK else f"reply {c:02x}"
            self.log(f"record at 0x{parse_addr(raw):08x}: {why}" + (", resending" if attempt < self.record_retries else ""))
        raise UpdateError(f"record at 0x{parse_addr(raw):08x}: {why} after {self.record_retries + 1} tries")

    def send_records(self, records, progress=None):
        raws = [r.pack() for r in records]
        for k, raw in enumerate(raws):
            if self.gap:
                time.sleep(self.gap)
            self.send_record(raw)
            if progress:
                progress(k + 1, len(raws), parse_addr(raw))


def parse_addr(raw):
    return FLASH + int.from_bytes(raw[7:10], "big")


def bar(i, n, addr):
    w = 30
    k = w * i // n
    sys.stderr.write(f"\r  [{'#' * k}{'.' * (w - k)}] {i}/{n} records (0x{addr:08x})")
    if i == n:
        sys.stderr.write("\n")
    sys.stderr.flush()


# ---------------- ports and the update-mode dance ----------------

VENDOR_NAMES = ("code", "m-audio", "maudio")


def find_vendor_port(backend, match=None):
    """The keyboard's port when it isn't running gemifi: --port's substring, else a name with 'Code' or
    'M-Audio' in it (the bootloader's USB name isn't known yet), never a gemifi port. None if not exactly one."""
    names = backend.ports()
    if getattr(backend, "fixed", False):
        return names[0] if names else None
    if match:
        hits = [n for n in names if match.lower() in n.lower()]
    else:
        hits = [n for n in names if any(k in n.lower() for k in VENDOR_NAMES) and "gemifi" not in n.lower()]
    return hits[0] if len(hits) == 1 or (match and hits) else None


def wait_power_cycle(backend, name, timeout, match=None, say=print):
    """Ask for a power cycle, wait for `name` to vanish, then for a vendor port to appear. Returns its name."""
    say("Now power-cycle the keyboard: turn it off (or unplug USB), wait two seconds, turn it on again.")
    deadline = time.monotonic() + timeout
    while name and time.monotonic() < deadline and name in backend.ports():
        time.sleep(0.1)
    if name and name in backend.ports():
        raise UpdateError(f"{name} didn't go away within {timeout:.0f} s; power-cycle the keyboard and run the "
                          "command again")
    while time.monotonic() < deadline:
        n = find_vendor_port(backend, match)
        if n:
            time.sleep(0.5)       # let it finish enumerating
            return n
        time.sleep(0.1)
    raise UpdateError(f"no keyboard port appeared within {timeout:.0f} s (try --port NAME; `gemifi.py ports`)")


PROBES = 5      # start messages per port before giving up (the stock updater probes every 2 s)


def enter_bootloader(backend, name, sender_kw, timeout, match=None, say=print, probes=PROBES):
    """Probe `name` with the start message (up to `probes` times while nothing answers). If the stock app answers
    15 (request stored), ask for a power cycle and probe the new port. Returns the name of a port whose
    bootloader acknowledged the probe."""
    for _ in range(2):
        with backend.open(name) as port:
            s = Sender(port, **sender_kw)
            for _ in range(probes):
                c = s.probe()
                if c is not None:
                    break
        if c in s.ok_codes:
            say(f"{name}: bootloader in update mode (F0 {c:02X} F7)")
            return name
        if c == R_NAK:
            say(f"{name}: the app took the update request (F0 15 F7)")
            name = wait_power_cycle(backend, name, timeout, match, say)
            continue
        raise UpdateError(f"{name}: no answer to {probes} start messages" if c is None else
                          f"{name}: unexpected reply F0 {c:02X} F7 to the start message")
    raise UpdateError(f"{name}: still the app after the power cycle (it answered F0 15 F7 again)")


def send_stream(port, records, sender_kw, progress=None, say=print):
    """Start message (unless restart=False), then every record with its ack. The port's bootloader must have
    answered a probe already."""
    s = Sender(port, **sender_kw)
    s.start()
    t0 = time.monotonic()
    s.send_records(records, progress)
    say(f"sent {len(records)} records in {time.monotonic() - t0:.1f} s ({s.resends} resends)")
    return s


# ---------------- CLI ----------------

def sender_args(a):
    return {"ack_timeout": a.ack_timeout, "probe_timeout": a.probe_timeout, "gap": a.gap / 1000.0,
            "record_retries": a.record_retries, "restart": not a.no_restart,
            "log": lambda s: print("  " + s, file=sys.stderr)}


def add_sender_options(p, wait=True):
    p.add_argument("--ack-timeout", type=float, default=ACK_TIMEOUT_S, help="seconds to wait for each ack (10)")
    p.add_argument("--probe-timeout", type=float, default=PROBE_TIMEOUT_S, help="seconds per probe (2)")
    p.add_argument("--gap", type=float, default=GAP_S * 1000, help="milliseconds between messages (3)")
    p.add_argument("--record-retries", type=int, default=0,
                   help="resend a record after a NAK or timeout this often (default 0, as the stock updater)")
    p.add_argument("--no-restart", action="store_true",
                   help="send the records straight after the probe ack, without a second start message")
    if wait:
        p.add_argument("--wait", type=float, default=120.0, help="seconds to wait for the power cycle (120)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["auto", "rtmidi", "mido", "alsa"], default="auto")
    ap.add_argument("--port", help="substring of the keyboard's port name (default: 'Code' or 'M-Audio')")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("file")
    p = sub.add_parser("build")
    p.add_argument("file")
    p.add_argument("out")
    p = sub.add_parser("syx")
    p.add_argument("file")
    p.add_argument("out")
    p = sub.add_parser("send")
    p.add_argument("file")
    p.add_argument("--dry-run", action="store_true", help="check and print the plan; no MIDI")
    p.add_argument("--yes", action="store_true", help="don't ask")
    add_sender_options(p)
    p = sub.add_parser("request")
    add_sender_options(p)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "request":
            return cmd_request(a)
        recs, desc, vendor = load_input(a.file)
        print(f"{a.file}: {desc}")
        if a.cmd == "info":
            for line in summary(recs):
                print(line)
            errs = check_stream(recs)
            for e in errs:
                print(f"  problem: {e}")
            return 1 if errs else 0
        errs = check_stream(recs)
        if errs:
            raise UpdateError("; ".join(errs))
        if a.cmd == "build":
            with open(a.out, "w") as f:
                f.write(pack_stream(recs).hex().upper())
            print(f"wrote {a.out}")
            return 0
        if a.cmd == "syx":
            msgs = syx_messages(recs)
            with open(a.out, "wb") as f:
                f.write(b"".join(msgs))
            print(f"wrote {len(msgs)} messages ({len(recs)} reply waits) to {a.out}")
            return 0
        for line in summary(recs):
            print(line)
        if a.dry_run:
            print("dry run: nothing sent")
            return 0
        return cmd_send(a, recs)
    except (UpdateError, gi.ImageError, OSError) as e:
        print(f"maudio_update: ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


def _backend(a):
    from gemifi_midi import get_backend
    return get_backend(a.backend)


def cmd_request(a):
    backend = _backend(a)
    name = find_vendor_port(backend, a.port)
    if not name:
        raise UpdateError("no keyboard port found (try --port NAME)")
    enter_bootloader(backend, name, sender_args(a), a.wait, a.port)
    print("the bootloader is waiting for an update (send one with `maudio_update.py send FILE`)")
    return 0


def cmd_send(a, recs):
    backend = _backend(a)
    name = find_vendor_port(backend, a.port)
    if not name:
        raise UpdateError("no keyboard port found (try --port NAME)")
    if not a.yes and not confirm(f"Replace the app on {name}? The vendor bootloader and settings stay."):
        print("cancelled")
        return 1
    kw = sender_args(a)
    name = enter_bootloader(backend, name, kw, a.wait, a.port)
    with backend.open(name) as port:
        send_stream(port, recs, kw, bar)
    print("done. The keyboard should restart into the new firmware (if not, power-cycle it).")
    return 0


def confirm(prompt):
    try:
        return input(f"{prompt} Type 'yes' to continue: ").strip().lower() == "yes"
    except EOFError:
        return False


if __name__ == "__main__":
    sys.exit(main())
