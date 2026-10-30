"""gemifi SysEx protocol v1 codec (protocol/sysex.md). Pure functions, standard library only.

Frame:  F0 7D 67 6D  ver cmd seq len_lo len_hi  payload...  chk  F7
chk = (sum of ver..last payload byte) & 0x7F; len = payload length in SysEx bytes (u14).
"""
from dataclasses import dataclass, field

MANUFACTURER = bytes([0x7D])        # MIDI non-commercial ID; build constant, replace for a release
SIGNATURE = bytes([0x67, 0x6D])     # 'g' 'm'
HEADER = bytes([0xF0]) + MANUFACTURER + SIGNATURE
PROTO_VERSION = 1

# Requests 0x00-0x1F; replies are cmd | 0x40 (0x40-0x5F); unsolicited 0x60-0x7F. No overlap.
# (FW_* were 0x20-0x23 before the 2026-10-30 renumbering.)
HELLO = 0x01
REBOOT = 0x10
FW_BEGIN = 0x18
FW_DATA = 0x19
FW_END = 0x1A
FW_ABORT = 0x1B
REPLY = 0x40
UNSOLICITED_FIRST = 0x60
# Unsolicited device -> host
LOG = 0x60
EVENT = 0x61

CMD_NAMES = {HELLO: "HELLO", REBOOT: "REBOOT", FW_BEGIN: "FW_BEGIN", FW_DATA: "FW_DATA",
             FW_END: "FW_END", FW_ABORT: "FW_ABORT", LOG: "LOG", EVENT: "EVENT"}

STATUS = {0: "OK", 1: "BAD_CHECKSUM", 2: "BAD_CMD", 3: "BAD_LEN", 4: "BAD_STATE", 5: "BAD_ADDR",
          6: "FLASH_ERR", 7: "BAD_IMAGE", 8: "BAD_BOARD", 9: "BUSY"}
OK, BAD_CHECKSUM, BAD_CMD, BAD_LEN, BAD_STATE, BAD_ADDR, FLASH_ERR, BAD_IMAGE, BAD_BOARD, BUSY = range(10)

ROLE_APP, ROLE_BOOT = 0, 1
REBOOT_APP, REBOOT_BOOT = 0, 1
CAP_FW, CAP_CONFIG, CAP_LOG, CAP_EVENT = 1, 2, 4, 8
LOG_LEVELS = {0: "ERROR", 1: "WARN", 2: "INFO", 3: "DEBUG"}
EVENT_TYPES = {1: "button", 2: "key", 3: "pot"}

APROM_SIZE = 32 * 1024
PAGE_SIZE = 512
REPLY_TIMEOUT_S = 0.200
LONG_REPLY_TIMEOUT_S = 2.000   # sysex.md 1: FW_BEGIN (page erase) and FW_END (read-back + CRC)
LONG_TIMEOUT_CMDS = (FW_BEGIN, FW_END)
MAX_RESENDS = 5                # sysex.md 1: up to 5 resends of the same seq, so 6 sends in all
MAX_TRIES = MAX_RESENDS + 1

IDENTITY_REQUEST = bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])


class ProtocolError(ValueError):
    pass


# ---- 7-bit integer encodings (least significant group first) ----

def _enc(value, nbytes, bits):
    if not 0 <= value < (1 << bits):
        raise ValueError(f"{value} does not fit in {bits} bits")
    return bytes((value >> (7 * i)) & 0x7F for i in range(nbytes))


def _dec(data, nbytes):
    if len(data) < nbytes:
        raise ProtocolError(f"need {nbytes} bytes, got {len(data)}")
    v = 0
    for i in range(nbytes):
        b = data[i]
        if b & 0x80:
            raise ProtocolError("byte with bit 7 set inside SysEx")
        v |= b << (7 * i)
    return v


def u14(v):
    return _enc(v, 2, 14)


def u28(v):
    return _enc(v, 4, 28)


def u32(v):
    return _enc(v, 5, 32)


def dec_u14(d):
    return _dec(d, 2)


def dec_u28(d):
    return _dec(d, 4)


def dec_u32(d):
    v = _dec(d, 5)
    if v >> 32:
        raise ProtocolError("u32 top byte has bits above bit 3 set")
    return v


# ---- 8->7 packing (Korg/Elektron: MSB byte first, then up to 7 low-7-bit bytes) ----

def pack7(data):
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        msbs = 0
        for j, b in enumerate(group):
            msbs |= ((b >> 7) & 1) << j
        out.append(msbs)
        out.extend(b & 0x7F for b in group)
    return bytes(out)


def unpack7(data):
    out = bytearray()
    for i in range(0, len(data), 8):
        group = data[i:i + 8]
        if len(group) < 2:
            raise ProtocolError("packed data ends with a lone MSB byte")
        if any(b & 0x80 for b in group):
            raise ProtocolError("byte with bit 7 set inside SysEx")
        msbs = group[0]
        for j, b in enumerate(group[1:]):
            out.append(b | (((msbs >> j) & 1) << 7))
    return bytes(out)


def packed_len(n):
    """SysEx bytes needed for n raw bytes."""
    return n + (n + 6) // 7


def unpacked_len(n):
    full, rem = divmod(n, 8)
    if rem == 1:
        raise ProtocolError("invalid packed length")
    return full * 7 + (rem - 1 if rem else 0)


# ---- framing ----

def checksum(body):
    """body = ver..last payload byte."""
    return sum(body) & 0x7F


def build_frame(cmd, seq, payload=b"", ver=PROTO_VERSION):
    payload = bytes(payload)
    if any(b & 0x80 for b in payload):
        raise ValueError("payload must be 7-bit (pack binary data first)")
    if not 0 <= seq <= 0x7F or not 0 <= cmd <= 0x7F:
        raise ValueError("cmd and seq must be 0..127")
    body = bytes([ver, cmd, seq]) + u14(len(payload)) + payload
    return HEADER + body + bytes([checksum(body), 0xF7])


@dataclass
class Frame:
    ver: int
    cmd: int
    seq: int
    payload: bytes = b""

    @property
    def is_reply(self):
        return REPLY <= self.cmd < UNSOLICITED_FIRST

    @property
    def is_unsolicited(self):
        return self.cmd >= UNSOLICITED_FIRST

    @property
    def status(self):
        return self.payload[0] if self.payload else None


# Reply payload lengths when status is OK (None = any). A non-OK reply is the status byte only (sysex.md 3).
REPLY_OK_LEN = {HELLO: None, REBOOT: 1, FW_BEGIN: 3, FW_DATA: 5, FW_END: 1, FW_ABORT: 1}


def reply_matches(frame, cmd, seq):
    """True if `frame` is a well-formed reply to request (cmd, seq), payload length included."""
    if frame.cmd != (cmd | REPLY) or frame.seq != seq or not frame.payload:
        return False
    want = REPLY_OK_LEN.get(cmd)
    if frame.payload[0] == OK:
        return want is None or len(frame.payload) == want
    return len(frame.payload) == 1


def is_gemifi(msg):
    msg = bytes(msg)
    return len(msg) >= 4 and msg[:4] == HEADER


def parse_frame(msg):
    """Parse one complete SysEx message (F0..F7). Raises ProtocolError."""
    msg = bytes(msg)
    if len(msg) < 11 or msg[0] != 0xF0 or msg[-1] != 0xF7:
        raise ProtocolError("not a complete SysEx message")
    if msg[:4] != HEADER:
        raise ProtocolError("not a gemifi message")
    inner = msg[1:-1]
    if any(b & 0x80 for b in inner):
        raise ProtocolError("byte with bit 7 set inside SysEx")
    body, chk = msg[4:-2], msg[-2]
    ver, cmd, seq = body[0], body[1], body[2]
    n = dec_u14(body[3:5])
    payload = body[5:]
    if len(payload) != n:
        raise ProtocolError(f"length field {n} but payload has {len(payload)} bytes")
    if checksum(body) != chk:
        raise ProtocolError(f"bad checksum: got 0x{chk:02x}, expected 0x{checksum(body):02x}")
    if ver != PROTO_VERSION:
        raise ProtocolError(f"unsupported protocol version {ver}")
    return Frame(ver, cmd, seq, bytes(payload))


# ---- request payloads ----

def req_hello():
    return b""


def req_reboot(mode):
    return bytes([mode])


def req_fw_begin(size, crc32, board_id, fw_version):
    return u32(size) + u32(crc32) + u14(board_id) + u32(fw_version)


def req_fw_data(offset, data):
    if not data or len(data) % 4:
        raise ValueError("FW_DATA data must be 1.. bytes and a multiple of 4")
    return u28(offset) + pack7(data)


# ---- reply / message decoding ----

@dataclass
class Hello:
    role: int
    board_id: int
    fw_version: int
    proto_version: int
    max_chunk: int
    caps: int
    uid: bytes = field(default=b"")

    @property
    def role_name(self):
        return {ROLE_APP: "app", ROLE_BOOT: "bootloader"}.get(self.role, f"role{self.role}")

    @property
    def uid_hex(self):
        return self.uid.hex().upper()


HELLO_REPLY_LEN = 1 + 1 + 2 + 5 + 1 + 2 + 2 + 14   # status + fields


def fmt_version(v):
    return f"{v >> 24}.{(v >> 16) & 0xFF}.{v & 0xFFFF}"


def parse_hello(payload):
    if len(payload) != HELLO_REPLY_LEN:
        raise ProtocolError(f"HELLO reply payload is {len(payload)} bytes, expected {HELLO_REPLY_LEN}")
    p = payload[1:]
    return Hello(role=p[0], board_id=dec_u14(p[1:3]), fw_version=dec_u32(p[3:8]), proto_version=p[8],
                 max_chunk=dec_u14(p[9:11]), caps=dec_u14(p[11:13]), uid=unpack7(p[13:27]))


def build_hello_reply(h, status=OK):
    """Device side; used by tests and the simulator."""
    return (bytes([status, h.role]) + u14(h.board_id) + u32(h.fw_version) + bytes([h.proto_version]) +
            u14(h.max_chunk) + u14(h.caps) + pack7(h.uid))


@dataclass
class LogMsg:
    level: int
    t_ms: int
    text: str

    def __str__(self):
        return f"[{self.t_ms:8d} ms] {LOG_LEVELS.get(self.level, f'L{self.level}'):5s} {self.text}"


@dataclass
class EventMsg:
    type: int
    index: int
    value: int
    t_ms: int

    def __str__(self):
        return f"[{self.t_ms:8d} ms] EVENT {EVENT_TYPES.get(self.type, f'type{self.type}')} {self.index} = {self.value}"


def parse_log(payload):
    if len(payload) < 5:
        raise ProtocolError("LOG payload too short")
    return LogMsg(payload[0], dec_u28(payload[1:5]), bytes(payload[5:]).decode("ascii", "replace"))


def build_log(level, t_ms, text):
    return bytes([level]) + u28(t_ms) + text.encode("ascii")[:64]


def parse_event(payload):
    if len(payload) != 9:
        raise ProtocolError("EVENT payload must be 9 bytes")
    return EventMsg(payload[0], dec_u14(payload[1:3]), dec_u14(payload[3:5]), dec_u28(payload[5:9]))


def build_event(etype, index, value, t_ms):
    return bytes([etype]) + u14(index) + u14(value) + u28(t_ms)


# ---- upload plan (sysex.md 4.2) ----

def valid_max_chunk(m):
    """sysex.md 4: max_chunk is a multiple of 4 that divides 512 (4, 8, 16, ... 512)."""
    return 4 <= m <= PAGE_SIZE and m % 4 == 0 and PAGE_SIZE % m == 0


def chunk_plan(size, max_chunk):
    """[(offset, length)]: 0x200 upward in order, then page 0 last; no chunk crosses a 512-byte page;
    every length is a multiple of 4 and <= max_chunk."""
    if size <= 0 or size % 4 or size > APROM_SIZE:
        raise ValueError("size must be 1..32 KB and a multiple of 4")
    if not valid_max_chunk(max_chunk):
        raise ValueError(f"max_chunk {max_chunk} is not a multiple of 4 that divides 512")
    step = max_chunk

    def page_chunks(page_start):
        end = min(page_start + PAGE_SIZE, size)
        return [(o, min(step, end - o)) for o in range(page_start, end, step)]

    plan = []
    for page in range(PAGE_SIZE, size, PAGE_SIZE):
        plan += page_chunks(page)
    return plan + page_chunks(0)


class SysexAssembler:
    """Collects complete SysEx messages from a raw MIDI byte stream (realtime bytes are dropped,
    other status bytes abort a partial SysEx)."""

    def __init__(self):
        self.buf = None

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:
                continue
            if b == 0xF0:
                self.buf = bytearray([b])
            elif self.buf is not None:
                if b == 0xF7:
                    self.buf.append(b)
                    out.append(bytes(self.buf))
                    self.buf = None
                elif b & 0x80:
                    self.buf = None
                else:
                    self.buf.append(b)
        return out
