"""A simulated M-Audio Code 61 for host-tool tests: the stock app, the vendor bootloader's SysEx update protocol
(research/devices/m-audio-code61/analysis/update-protocol.md, as far as it is known), and a gemifi app on board
0x0002. The bootloader side is written independently of maudio_update.py's parser, and strictly: it checks the
start message, the hex chunks, the record signature and checksum, only touches the app area, never programs a
page that isn't erased, and on 0D checks the image checksum at 0x0801B3FE."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import SimDevice  # noqa: E402

FLASH, SIZE = 0x08000000, 128 * 1024
APP_BASE, CK_PAGE, CK_ADDR, SETTINGS, FLAG = 0x08002400, 0x0801B000, 0x0801B3FE, 0x0801B400, 0x0801FC00
START = bytes.fromhex("f05a576e283c4e45f7")
SIG = bytes.fromhex("6a04da3b")
MAGIC = bytes.fromhex("555aa5aa59953aa3")
HEX = b"0123456789ABCDEF"


class SimCode61:
    """mode: 'app' (stock app: answers START with 15 and sets the flag), 'boot' (vendor bootloader).
    ok: the ack code (0x45 or 0x1A). nak_records / drop_ack_records: record numbers (from 0, counting every
    received record, resends included) that get a NAK / no reply. chatter: send F0 20 F7 before every ack."""

    def __init__(self, mode="boot", ok=0x45, nak_records=(), drop_ack_records=(), chatter=False):
        self.mem = bytearray(b"\xa5" * 0x2400) + bytearray(b"\xff" * (SIZE - 0x2400))   # vendor bootloader = A5
        self.mem[SETTINGS - FLASH:FLAG - FLASH] = b"\x5c" * (FLAG - SETTINGS)          # vendor settings = 5C
        self.mode, self.ok, self.chatter = mode, ok, chatter
        self.nak_records, self.drop_ack_records = set(nak_records), set(drop_ack_records)
        self.started = False
        self.buf = bytearray()
        self.n_records = 0
        self.errors = []
        self.finished = False
        self.starts = 0
        self.ops = []

    @property
    def flag_set(self):
        return bytes(self.mem[FLAG - FLASH:FLAG - FLASH + 8]) == MAGIC

    def app(self, n):
        return bytes(self.mem[APP_BASE - FLASH:APP_BASE - FLASH + n])

    def image_checksum_ok(self):
        s = sum(self.mem[APP_BASE - FLASH:CK_ADDR - FLASH])
        return struct.unpack_from("<H", self.mem, CK_ADDR - FLASH)[0] == (-s) & 0xFFFF

    def handle(self, msg):
        """One SysEx in -> list of replies."""
        msg = bytes(msg)
        if msg == START:
            self.starts += 1
            if self.mode == "app":
                self.mem[FLAG - FLASH:FLAG - FLASH + 0x400] = MAGIC + b"\xff" * (0x400 - 8)
                return [b"\xf0\x15\xf7"]
            if self.mode == "boot":
                self.started, self.buf = True, bytearray()
                return [bytes([0xF0, self.ok, 0xF7])]
            return []
        if self.mode != "boot" or not self.started:
            return []
        body = msg[1:-1]
        if msg[0] != 0xF0 or msg[-1] != 0xF7 or not body or len(body) > 22 or len(body) % 2 or \
                any(c not in HEX for c in body):
            return self.fail(f"bad chunk {msg[:8].hex()}")
        self.buf += bytes.fromhex(body.decode())
        if len(self.buf) < 2:
            return []
        n = struct.unpack_from(">H", self.buf, 0)[0] + 2
        if len(self.buf) < n:
            return []
        if len(self.buf) > n:
            return self.fail("chunk runs past the end of the record")
        rec, self.buf = bytes(self.buf), bytearray()
        k = self.n_records
        self.n_records += 1
        if k in self.drop_ack_records:
            return []
        if k in self.nak_records:
            return [b"\xf0\x15\xf7"]
        err = self.record(rec)
        if err:
            return self.fail(err)
        return ([b"\xf0\x20\xf7"] if self.chatter else []) + [bytes([0xF0, self.ok, 0xF7])]

    def fail(self, why):
        self.errors.append(why)
        self.buf = bytearray()
        return [b"\xf0\x15\xf7"]

    def record(self, rec):
        if rec[2:6] != SIG:
            return "bad signature"
        if struct.unpack_from("<H", rec, len(rec) - 2)[0] != (-sum(rec[2:-2])) & 0xFFFF:
            return "bad record checksum"
        if rec[-6:-2] != bytes(4):
            return "no 00000000 before the checksum"
        i, end, buf = 6, len(rec) - 6, b""
        while i < end:
            op = rec[i]
            addr = FLASH + int.from_bytes(rec[i + 1:i + 4], "big")
            n = int.from_bytes(rec[i + 4:i + 6], "big")
            i += 6
            self.ops.append((op, addr, n))
            o = addr - FLASH
            if op == 0x0A:
                buf, i = rec[i:i + n], i + n
            elif op == 0x09:
                if addr % 0x400 or not APP_BASE <= addr < SETTINGS:
                    return f"erase outside the app area: 0x{addr:08x}"
                self.mem[o:o + 0x400] = b"\xff" * 0x400
            elif op == 0x0B:
                if addr % 0x400 or not APP_BASE <= addr or addr + n > SETTINGS or n != len(buf):
                    return f"bad program 0x{addr:08x} +{n}"
                if self.mem[o:o + n] != b"\xff" * n:
                    return f"program of a page that isn't erased: 0x{addr:08x}"
                self.mem[o:o + n] = buf
            elif op == 0x0D:
                if not self.image_checksum_ok():
                    return "image checksum wrong"
            elif op == 0x0E:
                self.finished = True
                self.mem[FLAG - FLASH:FLAG - FLASH + 0x400] = b"\xff" * 0x400
                self.mode = "done"
            else:
                return f"unknown op {op:02x}"
        return None


class _Port:
    def __init__(self, handler, name):
        self.handler, self.name = handler, name
        self.q, self.sent = [], []

    def send(self, msg):
        self.sent.append(bytes(msg))
        self.q += self.handler(bytes(msg))

    def recv(self, timeout):
        out, self.q = self.q, []
        return out

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *e):
        pass


class Code61Backend:
    """A Code 61 on a USB port that comes and goes. state: 'gemifi' (a gemifi app on board 2 answers on
    'gemifi (Code 61)'), 'stock' (the stock app on 'Code 61 MIDI'), or 'boot'. After the app asks for update
    mode (REBOOT 1 / F0 15 F7) the port vanishes for `gap` ports() calls (the power cycle), then the bootloader
    shows up on 'Code 61 MIDI'. After 0E the gemifi image in flash (if any) answers as the app."""
    label = "sim-code61"
    GM, VENDOR = "gemifi (Code 61) 1", "Code 61 MIDI 1"

    def __init__(self, state="gemifi", gap=3, **boot_kw):
        self.state, self.gap, self.countdown = state, gap, 0
        self.boot = SimCode61("app" if state == "stock" else "boot", **boot_kw)
        self.gm = SimDevice(role=gp.ROLE_APP, board_id=2, fw_version=0x00010000)
        self.events = []          # "REBOOT(1)", "F0 15 F7", "bootloader", "finished"

    def _tick(self):
        if self.state == "gemifi" and gp.REBOOT_BOOT in self.gm.reboots:
            self.state, self.countdown = "cycling", self.gap
            self.events.append("REBOOT(1)")
        elif self.state == "stock" and self.boot.flag_set:
            self.state, self.countdown = "cycling", self.gap
            self.events.append("F0 15 F7")
        elif self.state == "cycling":
            self.countdown -= 1
            if self.countdown <= 0:
                self.state, self.boot.mode, self.boot.started = "boot", "boot", False
                self.events.append("bootloader")
        elif self.state == "boot" and self.boot.finished:
            self.events.append("finished")
            img = self.boot.app(0x200)
            if img[0x130:0x134] == b"GMFI":
                self.gm = SimDevice(role=gp.ROLE_APP, board_id=2, fw_version=gi.parse_header(img, 0x130).fw_version)
                self.state = "gemifi"
            else:
                self.state = "stock"
                self.boot.mode = "app"

    def ports(self):
        self._tick()
        p = {"gemifi": [self.GM], "stock": [self.VENDOR], "boot": [self.VENDOR]}.get(self.state, [])
        return p

    def open(self, name):
        if name == self.GM and self.state == "gemifi":
            dev = self.gm
            return _Port(lambda m: [r] if (r := dev.handle(m)) is not None else [], name)
        if name == self.VENDOR and self.state in ("stock", "boot"):
            return _Port(self.boot.handle, name)
        raise RuntimeError(f"port {name!r} not there (state {self.state})")
