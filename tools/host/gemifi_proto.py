"""gemifi SysEx protocol v1 codec (protocol/sysex.md). Pure functions, standard library only.

Frame:  F0 7D 67 6D  ver cmd seq len_lo len_hi  payload...  chk  F7
chk = (sum of ver..last payload byte) & 0x7F; len = payload length in SysEx bytes (u14).
"""
import re
import struct
import zlib
from dataclasses import dataclass, field

MANUFACTURER = bytes([0x7D])        # MIDI non-commercial ID; build constant, replace for a release
SIGNATURE = bytes([0x67, 0x6D])     # 'g' 'm'
HEADER = bytes([0xF0]) + MANUFACTURER + SIGNATURE
PROTO_VERSION = 1

# Requests 0x00-0x1F; replies are cmd | 0x40 (0x40-0x5F); host -> device notifications 0x20-0x3F (never
# answered, sysex.md 4.6.2); unsolicited 0x60-0x7F. No overlap.
# (FW_* were 0x20-0x23 before the 2026-10-30 renumbering.)
HELLO = 0x01
REBOOT = 0x10
FW_BEGIN = 0x18
FW_DATA = 0x19
FW_END = 0x1A
FW_ABORT = 0x1B
# Config (app only, sysex.md 4.3)
GET_SCHEMA = 0x02
GET_PARAM = 0x03
SET_PARAM = 0x04
GET_PARAMS = 0x09
SET_PARAMS = 0x0A
COMMIT = 0x07
FACTORY_RESET = 0x08
# Presets (app only, sysex.md 4.5)
PRESET_INFO = 0x0B
PRESET_SAVE = 0x0C
PRESET_RESET = 0x0D
PRESET_NAME = 0x0E
# Host feedback (app only, sysex.md 4.6)
GET_COMPONENT = 0x11
NOTIFY_FIRST, NOTIFY_LAST = 0x20, 0x3F
FB_LED = 0x20
FB_LED_RGB = 0x21
FB_LEDS = 0x22
FB_VALUE = 0x23
FB_RING = 0x24
FB_TEXT = 0x25
FB_RELEASE = 0x26
REPLY = 0x40
UNSOLICITED_FIRST = 0x60
# Unsolicited device -> host
LOG = 0x60
EVENT = 0x61
PARAM_CHANGED = 0x62
PRESET_CHANGED = 0x63

CMD_NAMES = {HELLO: "HELLO", REBOOT: "REBOOT", FW_BEGIN: "FW_BEGIN", FW_DATA: "FW_DATA",
             FW_END: "FW_END", FW_ABORT: "FW_ABORT", GET_SCHEMA: "GET_SCHEMA", GET_PARAM: "GET_PARAM",
             SET_PARAM: "SET_PARAM", GET_PARAMS: "GET_PARAMS",
             SET_PARAMS: "SET_PARAMS", COMMIT: "COMMIT", FACTORY_RESET: "FACTORY_RESET", LOG: "LOG",
             EVENT: "EVENT", PARAM_CHANGED: "PARAM_CHANGED", PRESET_INFO: "PRESET_INFO",
             PRESET_SAVE: "PRESET_SAVE", PRESET_RESET: "PRESET_RESET", PRESET_NAME: "PRESET_NAME",
             PRESET_CHANGED: "PRESET_CHANGED", GET_COMPONENT: "GET_COMPONENT", FB_LED: "FB_LED",
             FB_LED_RGB: "FB_LED_RGB", FB_LEDS: "FB_LEDS", FB_VALUE: "FB_VALUE", FB_RING: "FB_RING",
             FB_TEXT: "FB_TEXT", FB_RELEASE: "FB_RELEASE"}

STATUS = {0: "OK", 1: "BAD_CHECKSUM", 2: "BAD_CMD", 3: "BAD_LEN", 4: "BAD_STATE", 5: "BAD_ADDR",
          6: "FLASH_ERR", 7: "BAD_IMAGE", 8: "BAD_BOARD", 9: "BUSY", 10: "BAD_PARAM", 11: "BAD_VALUE"}
(OK, BAD_CHECKSUM, BAD_CMD, BAD_LEN, BAD_STATE, BAD_ADDR, FLASH_ERR, BAD_IMAGE, BAD_BOARD, BUSY,
 BAD_PARAM, BAD_VALUE) = range(12)

ROLE_APP, ROLE_BOOT = 0, 1
REBOOT_APP, REBOOT_BOOT = 0, 1
CAP_FW, CAP_CONFIG, CAP_LOG, CAP_EVENT = 1, 2, 4, 8
CAP_PRESETS = 32               # sysex.md 4.1 bit 5 (bit 4 is not assigned)
CAP_FEEDBACK = 1 << 6          # sysex.md 4.1 bit 6: host feedback (GET_COMPONENT, FB_*)
LOG_LEVELS = {0: "ERROR", 1: "WARN", 2: "INFO", 3: "DEBUG"}
EVENT_TYPES = {1: "button", 2: "key", 3: "analog", 4: "encoder"}
EV_BUTTON, EV_KEY, EV_ANALOG, EV_ENCODER = 1, 2, 3, 4

APROM_SIZE = 32 * 1024
PAGE_SIZE = 512
REPLY_TIMEOUT_S = 0.200
LONG_REPLY_TIMEOUT_S = 2.000   # sysex.md 1: FW_BEGIN, FW_END, COMMIT, FACTORY_RESET, PRESET_SAVE/RESET/NAME
LONG_TIMEOUT_CMDS = (FW_BEGIN, FW_END, COMMIT, FACTORY_RESET, PRESET_SAVE, PRESET_RESET, PRESET_NAME)
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


S32_MIN, S32_MAX = -(1 << 31), (1 << 31) - 1


def s32(v):
    """Signed 32-bit: the u32 of its two's-complement bit pattern (-1 -> 7F 7F 7F 7F 0F)."""
    if not S32_MIN <= v <= S32_MAX:
        raise ValueError(f"{v} does not fit in a signed 32-bit int")
    return u32(v & 0xFFFFFFFF)


def dec_s32(d):
    v = dec_u32(d)
    return v - (1 << 32) if v & 0x80000000 else v


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
    def is_notification(self):
        """Host -> device notification (0x20-0x3F, sysex.md 4.6.2): never answered."""
        return NOTIFY_FIRST <= self.cmd <= NOTIFY_LAST

    @property
    def is_unsolicited(self):
        return self.cmd >= UNSOLICITED_FIRST

    @property
    def status(self):
        return self.payload[0] if self.payload else None


# Reply payload lengths when status is OK (None = any). A non-OK reply is the status byte only (sysex.md 3).
REPLY_OK_LEN = {HELLO: None, REBOOT: 1, FW_BEGIN: 3, FW_DATA: 5, FW_END: 1, FW_ABORT: 1,
                GET_SCHEMA: None, GET_PARAM: 10, SET_PARAM: 10, GET_PARAMS: None, SET_PARAMS: 7,
                COMMIT: 1, FACTORY_RESET: 1, PRESET_INFO: None, PRESET_SAVE: 1, PRESET_RESET: 1, PRESET_NAME: 1,
                GET_COMPONENT: None}


def is_notification(cmd):
    """0x20-0x3F: host -> device, never answered; send without waiting (sysex.md 4.6.2)."""
    return NOTIFY_FIRST <= cmd <= NOTIFY_LAST


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


SCHEMA_WINDOW = 64     # sysex.md 4: GET_SCHEMA `max` is 1..64 unpacked bytes


MAX_RUN = 16           # sysex.md 4: GET_PARAMS / SET_PARAMS `n` is 1..16


def req_get_schema(offset, max_bytes=SCHEMA_WINDOW):
    if not 1 <= max_bytes <= SCHEMA_WINDOW:
        raise ValueError(f"GET_SCHEMA max must be 1..{SCHEMA_WINDOW}")
    return u28(offset) + u14(max_bytes)


def req_get_param(pid, index=0):
    return u14(pid) + u14(index)


def req_set_param(pid, index, value):
    return u14(pid) + u14(index) + s32(value)


def req_get_params(pid, index, n):
    if not 1 <= n <= MAX_RUN:
        raise ValueError(f"GET_PARAMS n must be 1..{MAX_RUN}")
    return u14(pid) + u14(index) + u14(n)


def req_set_params(pid, index, values):
    if not 1 <= len(values) <= MAX_RUN:
        raise ValueError(f"SET_PARAMS takes 1..{MAX_RUN} values")
    return u14(pid) + u14(index) + u14(len(values)) + b"".join(s32(v) for v in values)


def req_commit():
    return b""


def req_factory_reset():
    return b""


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
    node: str | None = None          # node name (sysex.md 4.1.1); None = the device didn't send one

    @property
    def role_name(self):
        return {ROLE_APP: "app", ROLE_BOOT: "bootloader"}.get(self.role, f"role{self.role}")

    @property
    def uid_hex(self):
        return self.uid.hex().upper()


HELLO_REPLY_LEN = 1 + 1 + 2 + 5 + 1 + 2 + 2 + 14   # status + fields, without the optional node name
NODE_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?")   # sysex.md 4.1.1, as in the builder


def fmt_version(v):
    return f"{v >> 24}.{(v >> 16) & 0xFF}.{v & 0xFFFF}"


def parse_node_name(rest):
    """The optional HELLO `node` field (sysex.md 4.1.1) from the bytes after `uid`; None if absent or bad."""
    if not rest or rest[0] == 0 or 1 + rest[0] > len(rest):
        return None
    name = bytes(rest[1:1 + rest[0]]).decode("ascii", "replace")
    return name if NODE_NAME_RE.fullmatch(name) else None


def parse_hello(payload):
    if len(payload) < HELLO_REPLY_LEN:
        raise ProtocolError(f"HELLO reply payload is {len(payload)} bytes, expected at least {HELLO_REPLY_LEN}")
    p = payload[1:]
    return Hello(role=p[0], board_id=dec_u14(p[1:3]), fw_version=dec_u32(p[3:8]), proto_version=p[8],
                 max_chunk=dec_u14(p[9:11]), caps=dec_u14(p[11:13]), uid=unpack7(p[13:27]),
                 node=parse_node_name(payload[HELLO_REPLY_LEN:]))


def build_hello_reply(h, status=OK):
    """Device side; used by tests and the simulator."""
    return (bytes([status, h.role]) + u14(h.board_id) + u32(h.fw_version) + bytes([h.proto_version]) +
            u14(h.max_chunk) + u14(h.caps) + pack7(h.uid) +
            (bytes([len(h.node)]) + h.node.encode("ascii") if h.node else b""))


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

    @property
    def delta(self):
        """Encoder events: `value` is a signed delta, 14-bit two's complement."""
        return self.value - 0x4000 if self.value & 0x2000 else self.value

    def __str__(self):
        v = f"{self.delta:+d}" if self.type == EV_ENCODER else str(self.value)
        return f"[{self.t_ms:8d} ms] EVENT {EVENT_TYPES.get(self.type, f'type{self.type}')} {self.index} = {v}"


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


# ---- config replies and PARAM_CHANGED (sysex.md 4, 4.3) ----

@dataclass
class ParamValue:
    """One (id, index, value): the GET_PARAM / SET_PARAM reply and the PARAM_CHANGED payload."""
    id: int
    index: int
    value: int

    def __str__(self):
        return f"PARAM_CHANGED id {self.id}[{self.index}] = {self.value}"


def _param_value(p, what):
    if len(p) != 9:
        raise ProtocolError(f"{what} needs 9 bytes (id u14, index u14, value s32), got {len(p)}")
    return ParamValue(dec_u14(p[0:2]), dec_u14(p[2:4]), dec_s32(p[4:9]))


def _param_value_bytes(pid, index, value):
    return u14(pid) + u14(index) + s32(value)


def parse_param_reply(payload):
    """GET_PARAM / SET_PARAM reply payload (status OK) -> ParamValue."""
    if not payload or payload[0] != OK:
        raise ProtocolError("not an OK reply")
    return _param_value(payload[1:], "GET_PARAM/SET_PARAM reply")


def build_param_reply(pid, index, value, status=OK):
    """Device side; used by tests and the simulator."""
    return bytes([status]) + (_param_value_bytes(pid, index, value) if status == OK else b"")


def parse_param_changed(payload):
    return _param_value(payload, "PARAM_CHANGED")


def build_param_changed(pid, index, value):
    return _param_value_bytes(pid, index, value)


def parse_params_reply(payload):
    """GET_PARAMS reply payload (status OK) -> (id, index, [values])."""
    if len(payload) < 7 or payload[0] != OK:
        raise ProtocolError("GET_PARAMS reply too short or not OK")
    n = dec_u14(payload[5:7])
    if len(payload) != 7 + 5 * n:
        raise ProtocolError(f"GET_PARAMS reply says n={n} but has {len(payload) - 7} value bytes")
    return dec_u14(payload[1:3]), dec_u14(payload[3:5]), [dec_s32(payload[7 + 5 * i:12 + 5 * i]) for i in range(n)]


def build_params_reply(pid, index, values, status=OK):
    """Device side (GET_PARAMS)."""
    if status != OK:
        return bytes([status])
    return bytes([OK]) + u14(pid) + u14(index) + u14(len(values)) + b"".join(s32(v) for v in values)


def parse_set_params_reply(payload):
    """SET_PARAMS reply payload (status OK) -> (id, index, n)."""
    if len(payload) != 7 or payload[0] != OK:
        raise ProtocolError("SET_PARAMS reply must be OK + 6 bytes")
    return dec_u14(payload[1:3]), dec_u14(payload[3:5]), dec_u14(payload[5:7])


def build_set_params_reply(pid, index, n, status=OK):
    """Device side (SET_PARAMS)."""
    return bytes([status]) + (u14(pid) + u14(index) + u14(n) if status == OK else b"")


def parse_schema_reply(payload):
    """GET_SCHEMA reply payload (status OK) -> (total, offset, unpacked data)."""
    if len(payload) < 9 or payload[0] != OK:
        raise ProtocolError("GET_SCHEMA reply too short or not OK")
    return dec_u28(payload[1:5]), dec_u28(payload[5:9]), unpack7(payload[9:]) if len(payload) > 9 else b""


def build_schema_reply(schema_bytes, offset, max_bytes):
    """Device side: the window [offset, offset+max) of the schema (empty at or past the end)."""
    data = bytes(schema_bytes[offset:offset + max_bytes])
    return bytes([OK]) + u28(len(schema_bytes)) + u28(offset) + pack7(data)


# ---- presets (sysex.md 4.5) ----

P_PRESET_ACTIVE, P_PRESET_PROGRAM, P_PRESET_DIRTY = 17, 18, 19   # fixed param ids
PRESET_NAME_MAX = 16
PI_STORED, PI_DIRTY, PI_YAML = 1, 2, 4      # PRESET_INFO flags
PRESET_ALL = 0                              # PRESET_RESET slot 0 = every slot; PRESET_INFO slot 0 = the active one


def check_preset_name(name):
    """A preset name: 0..16 printable ASCII characters (0x20..0x7E). Returns the bytes; raises ValueError."""
    try:
        b = name.encode("ascii")
    except UnicodeEncodeError:
        b = None
    if b is None or len(b) > PRESET_NAME_MAX or any(not 0x20 <= c <= 0x7E for c in b):
        raise ValueError(f"preset name {name!r}: 0..{PRESET_NAME_MAX} printable ASCII characters")
    return b


def enc_preset_name(name):
    """'string' (sysex.md 2): u8 len (0..16) + 7-bit ASCII."""
    b = check_preset_name(name)
    return bytes([len(b)]) + b


def dec_preset_name(data, what="name"):
    """'string' at the start of data -> (str, bytes used). Raises ProtocolError (length only; the device
    checks the characters)."""
    if not data:
        raise ProtocolError(f"{what}: missing length byte")
    n = data[0]
    if n > PRESET_NAME_MAX:
        raise ProtocolError(f"{what}: length {n} > {PRESET_NAME_MAX}")
    if len(data) < 1 + n:
        raise ProtocolError(f"{what}: length {n} but {len(data) - 1} bytes follow")
    if any(c & 0x80 for c in data[1:1 + n]):
        raise ProtocolError(f"{what}: byte with bit 7 set")
    return bytes(data[1:1 + n]).decode("ascii"), 1 + n


def req_preset_info(slot=0):
    return u14(slot)


def req_preset_save(slot):
    return u14(slot)


def req_preset_reset(slot=PRESET_ALL):
    return u14(slot)


def req_preset_name(slot, name):
    return u14(slot) + enc_preset_name(name)


def dec_preset_name_req(payload):
    """Device side: PRESET_NAME request -> (slot, name). Raises ProtocolError on a bad layout."""
    if len(payload) < 3:
        raise ProtocolError("PRESET_NAME request too short")
    name, used = dec_preset_name(payload[2:])
    if 2 + used != len(payload):
        raise ProtocolError("PRESET_NAME request has bytes after the name")
    return dec_u14(payload[0:2]), name


@dataclass
class PresetInfo:
    slot: int
    count: int
    active: int
    flags: int
    revision: int            # the firmware's presets revision
    stored_revision: int     # the revision the slot's stored edits were saved under (0 = none stored)
    program: int             # Program Change sent on select, -1 = none
    name: str

    @property
    def stored(self):
        return bool(self.flags & PI_STORED)

    @property
    def dirty(self):
        return bool(self.flags & PI_DIRTY)

    @property
    def yaml(self):
        return bool(self.flags & PI_YAML)

    @property
    def is_active(self):
        return self.slot == self.active

    @property
    def stale(self):
        """Stored edits made under another presets revision (sysex.md 4.5, after an upload)."""
        return self.stored and self.stored_revision != self.revision


PRESET_INFO_FIXED = 1 + 2 + 2 + 2 + 1 + 2 + 2 + 5    # status .. program; the name follows


def parse_preset_info(payload):
    """PRESET_INFO reply payload (status OK) -> PresetInfo."""
    if len(payload) < PRESET_INFO_FIXED + 1 or payload[0] != OK:
        raise ProtocolError("PRESET_INFO reply too short or not OK")
    p = payload
    name, used = dec_preset_name(p[PRESET_INFO_FIXED:], "PRESET_INFO name")
    if PRESET_INFO_FIXED + used != len(p):
        raise ProtocolError("PRESET_INFO reply has bytes after the name")
    return PresetInfo(slot=dec_u14(p[1:3]), count=dec_u14(p[3:5]), active=dec_u14(p[5:7]), flags=p[7],
                      revision=dec_u14(p[8:10]), stored_revision=dec_u14(p[10:12]), program=dec_s32(p[12:17]),
                      name=name)


def build_preset_info_reply(info, status=OK):
    """Device side; used by tests, the simulator and the vectors."""
    if status != OK:
        return bytes([status])
    return (bytes([OK]) + u14(info.slot) + u14(info.count) + u14(info.active) + bytes([info.flags]) +
            u14(info.revision) + u14(info.stored_revision) + s32(info.program) + enc_preset_name(info.name))


@dataclass
class PresetChanged:
    slot: int
    dirty: int

    def __str__(self):
        return f"PRESET_CHANGED preset {self.slot}" + (" (unsaved edits)" if self.dirty else "")


def parse_preset_changed(payload):
    if len(payload) != 3:
        raise ProtocolError(f"PRESET_CHANGED payload must be 3 bytes, got {len(payload)}")
    return PresetChanged(dec_u14(payload[0:2]), payload[2])


def build_preset_changed(slot, dirty):
    return u14(slot) + bytes([1 if dirty else 0])


# ---- host feedback (sysex.md 4.6) ----

# Component kinds (GET_COMPONENT `kind`, 4.6.1): 1..4 are the EVENT type numbers.
KIND_BUTTON, KIND_KEYS, KIND_ANALOG, KIND_ENCODER, KIND_LED, KIND_DISPLAY = 1, 2, 3, 4, 5, 6
KINDS = (KIND_BUTTON, KIND_KEYS, KIND_ANALOG, KIND_ENCODER, KIND_LED, KIND_DISPLAY)
KIND_NAMES = {KIND_BUTTON: "button", KIND_KEYS: "keys", KIND_ANALOG: "analog", KIND_ENCODER: "encoder",
              KIND_LED: "led", KIND_DISPLAY: "display"}
NONE_INDEX = 0x3FFF            # "no LED" in a button's `led` and an encoder's `center`
LSTRING_MAX = 31               # sysex.md 2: lstring = u8 len (0..31) + ASCII
COLOURS = {1: "single", 2: "bicolour", 3: "RGB"}
DISPLAY_TYPES = {1: "characters", 2: "segments"}
RING_DOT, RING_BOOST, RING_WRAP, RING_SPREAD = 0, 1, 2, 3
RING_MODES = {"dot": RING_DOT, "boost": RING_BOOST, "wrap": RING_WRAP, "spread": RING_SPREAD}
REL_ALL, REL_LEDS, REL_VALUES, REL_TEXT = 0, 1, 2, 3
RELEASE_WHAT = {"all": REL_ALL, "leds": REL_LEDS, "values": REL_VALUES, "text": REL_TEXT}
FB_LEDS_MAX = 64               # FB_LEDS `n` is 1..64
FB_TEXT_MAX = 64               # FB_TEXT text is <= 64 bytes
VALUE_MAX = 0x3FFF             # FB_VALUE / FB_RING `value` is 0..16383


def enc_lstring(s, what="lstring"):
    """'lstring' (sysex.md 2): u8 len (0..31) + printable ASCII. The host refuses anything longer (only the
    device cuts, see lstring_cut). Raises ValueError."""
    try:
        b = s.encode("ascii")
    except UnicodeEncodeError:
        b = None
    if b is None or len(b) > LSTRING_MAX or any(not 0x20 <= c <= 0x7E for c in b):
        raise ValueError(f"{what} {s!r}: 0..{LSTRING_MAX} printable ASCII characters")
    return bytes([len(b)]) + b


def lstring_cut(s):
    """What the device sends for a longer string: its first 31 characters (sysex.md 4.6.1 "cut")."""
    return s[:LSTRING_MAX]


def dec_lstring(data, what="lstring"):
    """'lstring' at the start of data -> (str, bytes used). Accepts len 0..31. Raises ProtocolError."""
    if not data:
        raise ProtocolError(f"{what}: missing length byte")
    n = data[0]
    if n > LSTRING_MAX:
        raise ProtocolError(f"{what}: length {n} > {LSTRING_MAX}")
    if len(data) < 1 + n:
        raise ProtocolError(f"{what}: length {n} but {len(data) - 1} bytes follow")
    if any(c & 0x80 for c in data[1:1 + n]):
        raise ProtocolError(f"{what}: byte with bit 7 set")
    return bytes(data[1:1 + n]).decode("ascii"), 1 + n


def _check_index(v, what):
    """A component index to address: u14, but not 0x3FFF (that means "none")."""
    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v < NONE_INDEX:
        raise ValueError(f"{what} {v!r} is outside 0..{NONE_INDEX - 1}")


def _check_byte(v, what, hi=0x7F):
    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= hi:
        raise ValueError(f"{what} {v!r} is outside 0..{hi}")


def req_get_component(kind, index):
    """GET_COMPONENT request: kind (1 byte), index u14."""
    _check_byte(kind, "kind")
    return bytes([kind]) + u14(index)


@dataclass
class Component:
    """One GET_COMPONENT reply (sysex.md 4.6.1). Only the fields of its kind are on the wire; the others keep
    their defaults. `extra` holds the bytes after the common fields for a kind this tool doesn't know."""
    kind: int
    index: int
    count: int
    id: str
    name: str = ""
    role: str = ""
    led: int = NONE_INDEX          # button: its LED's index
    keys: int = 0                  # key matrix: number of keys
    first_note: int = 0            # key matrix: note of the first key
    ring: int = 0                  # encoder: LEDs in its ring (0 = none)
    center: int = NONE_INDEX       # encoder: centre LED index
    colours: int = 1               # LED: 1 single, 2 bicolour, 3 RGB
    cols: int = 0                  # display
    rows: int = 0                  # display
    display_type: int = 0          # display: 1 characters, 2 segments (the spec's `type`)
    extra: bytes = b""

    @property
    def kind_name(self):
        return KIND_NAMES.get(self.kind, f"kind{self.kind}")

    def extras_text(self):
        """The kind's extra fields in words, for tables."""
        k = self.kind
        if k == KIND_BUTTON:
            return "no LED" if self.led == NONE_INDEX else f"LED {self.led}"
        if k == KIND_KEYS:
            return f"{self.keys} keys from {note_name(self.first_note)} ({self.first_note})"
        if k == KIND_ENCODER:
            ring = f"ring of {self.ring}" if self.ring else "no ring"
            return ring + ("" if self.center == NONE_INDEX else f", centre LED {self.center}")
        if k == KIND_LED:
            return COLOURS.get(self.colours, f"colours {self.colours}")
        if k == KIND_DISPLAY:
            return f"{self.cols}x{self.rows} " + DISPLAY_TYPES.get(self.display_type, f"type {self.display_type}")
        return self.extra.hex(" ") if self.extra else ""

    def to_json(self):
        j = {"kind": self.kind, "kind_name": self.kind_name, "index": self.index, "count": self.count,
             "id": self.id, "name": self.name, "role": self.role}
        k = self.kind
        if k == KIND_BUTTON:
            j["led"] = None if self.led == NONE_INDEX else self.led
        elif k == KIND_KEYS:
            j.update(keys=self.keys, first_note=self.first_note)
        elif k == KIND_ENCODER:
            j.update(ring=self.ring, center=None if self.center == NONE_INDEX else self.center)
        elif k == KIND_LED:
            j["colours"] = self.colours
        elif k == KIND_DISPLAY:
            j.update(cols=self.cols, rows=self.rows, type=self.display_type)
        elif self.extra:
            j["extra"] = self.extra.hex(" ")
        return j


# Bytes of each kind's extra fields after `role` (sysex.md 4.6.1 table).
COMPONENT_EXTRA_LEN = {KIND_BUTTON: 2, KIND_KEYS: 3, KIND_ANALOG: 0, KIND_ENCODER: 3, KIND_LED: 1, KIND_DISPLAY: 3}


def parse_component(payload):
    """GET_COMPONENT reply payload (status OK) -> Component. Bytes after a known kind's fields are ignored
    (room for later fields, as for HELLO). Raises ProtocolError."""
    if len(payload) < 6 or payload[0] != OK:
        raise ProtocolError("GET_COMPONENT reply too short or not OK")
    kind, index, count = payload[1], dec_u14(payload[2:4]), dec_u14(payload[4:6])
    pos = 6
    strs = []
    for what in ("id", "name", "role"):
        s, used = dec_lstring(payload[pos:], f"GET_COMPONENT {what}")
        strs.append(s)
        pos += used
    c = Component(kind, index, count, *strs)
    rest = bytes(payload[pos:])
    need = COMPONENT_EXTRA_LEN.get(kind)
    if need is None:
        c.extra = rest
        return c
    if len(rest) < need:
        raise ProtocolError(f"GET_COMPONENT {c.kind_name}: {len(rest)} bytes of fields, need {need}")
    if any(b & 0x80 for b in rest):
        raise ProtocolError("byte with bit 7 set inside SysEx")
    if kind == KIND_BUTTON:
        c.led = dec_u14(rest[0:2])
    elif kind == KIND_KEYS:
        c.keys, c.first_note = dec_u14(rest[0:2]), rest[2]
    elif kind == KIND_ENCODER:
        c.ring, c.center = rest[0], dec_u14(rest[1:3])
    elif kind == KIND_LED:
        c.colours = rest[0]
    elif kind == KIND_DISPLAY:
        c.cols, c.rows, c.display_type = rest[0], rest[1], rest[2]
    return c


def build_component_reply(c, status=OK):
    """Device side (tests, the simulator, the vectors). Strings over 31 characters are cut, as the device does."""
    if status != OK:
        return bytes([status])
    out = bytes([OK, c.kind]) + u14(c.index) + u14(c.count)
    for what, s in (("id", c.id), ("name", c.name), ("role", c.role)):
        out += enc_lstring(lstring_cut(s), what)
    k = c.kind
    if k == KIND_BUTTON:
        out += u14(c.led)
    elif k == KIND_KEYS:
        _check_byte(c.first_note, "first_note")
        out += u14(c.keys) + bytes([c.first_note])
    elif k == KIND_ENCODER:
        _check_byte(c.ring, "ring")
        out += bytes([c.ring]) + u14(c.center)
    elif k == KIND_LED:
        _check_byte(c.colours, "colours")
        out += bytes([c.colours])
    elif k == KIND_DISPLAY:
        for v, what in ((c.cols, "cols"), (c.rows, "rows"), (c.display_type, "type")):
            _check_byte(v, what)
        out += bytes([c.cols, c.rows, c.display_type])
    elif k != KIND_ANALOG:
        out += bytes(c.extra)
    return out


# -- notifications (sysex.md 4.6.2): payload builders; send them with Session.notify, which never waits --

def fb_led(led, level):
    """FB_LED: led u14, level (0 off, 1 blink, 2..127 on; bicolour 2..63 colour 1, 64..127 colour 2)."""
    _check_index(led, "led")
    _check_byte(level, "level")
    return u14(led) + bytes([level])


def fb_led_rgb(led, r, g, b, blink=False):
    """FB_LED_RGB: led u14, r, g, b (0..127 each), flags (bit 0 blink)."""
    _check_index(led, "led")
    for v, what in ((r, "r"), (g, "g"), (b, "b")):
        _check_byte(v, what)
    return u14(led) + bytes([r, g, b, 1 if blink else 0])


def fb_leds(first, levels):
    """FB_LEDS: first u14, n (1..64), then n levels (LEDs first .. first+n-1)."""
    levels = list(levels)
    _check_index(first, "first")
    if not 1 <= len(levels) <= FB_LEDS_MAX:
        raise ValueError(f"FB_LEDS takes 1..{FB_LEDS_MAX} levels, got {len(levels)}")
    _check_index(first + len(levels) - 1, "last LED")
    for v in levels:
        _check_byte(v, "level")
    return u14(first) + bytes([len(levels)] + levels)


def fb_value(kind, index, value):
    """FB_VALUE: kind (3 analog, 4 encoder), index u14, value u14 (0..16383)."""
    if kind not in (KIND_ANALOG, KIND_ENCODER):
        raise ValueError(f"FB_VALUE kind must be {KIND_ANALOG} (analog) or {KIND_ENCODER} (encoder), got {kind!r}")
    _check_index(index, "index")
    _check_byte(value, "value", VALUE_MAX)
    return bytes([kind]) + u14(index) + u14(value)


def fb_ring(encoder, mode, value, center=False):
    """FB_RING: encoder u14, mode (0 dot, 1 boost/cut, 2 wrap, 3 spread), value u14, flags (bit 0 centre LED)."""
    _check_index(encoder, "encoder")
    if mode not in RING_MODES.values():
        raise ValueError(f"ring mode {mode!r}: 0 dot, 1 boost/cut, 2 wrap, 3 spread")
    _check_byte(value, "value", VALUE_MAX)
    return u14(encoder) + bytes([mode]) + u14(value) + bytes([1 if center else 0])


def fb_text(display, row, col, text):
    """FB_TEXT: display u14, row, col (1 byte each), text (0..64 bytes of printable ASCII, no length byte)."""
    _check_index(display, "display")
    _check_byte(row, "row")
    _check_byte(col, "col")
    try:
        b = text.encode("ascii")
    except UnicodeEncodeError:
        b = None
    if b is None or len(b) > FB_TEXT_MAX or any(not 0x20 <= c <= 0x7E for c in b):
        raise ValueError(f"FB_TEXT text {text!r}: 0..{FB_TEXT_MAX} printable ASCII characters")
    return u14(display) + bytes([row, col]) + b


def fb_release(what=REL_ALL):
    """FB_RELEASE: what (0 everything, 1 LEDs, 2 values, 3 text)."""
    if what not in RELEASE_WHAT.values():
        raise ValueError(f"FB_RELEASE what {what!r}: 0 all, 1 LEDs, 2 values, 3 text")
    return bytes([what])


@dataclass
class Notification:
    """A decoded FB_* notification (device side: the simulator and tests)."""
    cmd: int
    fields: dict

    @property
    def name(self):
        return CMD_NAMES.get(self.cmd, f"0x{self.cmd:02x}")


def parse_notification(cmd, payload):
    """FB_* payload -> Notification. Raises ProtocolError for a malformed or unknown one (the device drops
    those without a reply, sysex.md 4.6.2)."""
    p = bytes(payload)
    if any(b & 0x80 for b in p):
        raise ProtocolError("byte with bit 7 set inside SysEx")

    def need(n):
        if len(p) != n:
            raise ProtocolError(f"{CMD_NAMES.get(cmd, hex(cmd))} payload must be {n} bytes, got {len(p)}")

    if cmd == FB_LED:
        need(3)
        f = {"led": dec_u14(p[0:2]), "level": p[2]}
    elif cmd == FB_LED_RGB:
        need(6)
        f = {"led": dec_u14(p[0:2]), "r": p[2], "g": p[3], "b": p[4], "blink": p[5] & 1}
    elif cmd == FB_LEDS:
        if len(p) < 4 or not 1 <= p[2] <= FB_LEDS_MAX:
            raise ProtocolError("FB_LEDS needs first u14, n 1..64 and n levels")
        need(3 + p[2])
        f = {"first": dec_u14(p[0:2]), "levels": list(p[3:])}
    elif cmd == FB_VALUE:
        need(5)
        if p[0] not in (KIND_ANALOG, KIND_ENCODER):
            raise ProtocolError(f"FB_VALUE kind {p[0]}")
        f = {"kind": p[0], "index": dec_u14(p[1:3]), "value": dec_u14(p[3:5])}
    elif cmd == FB_RING:
        need(6)
        if p[2] > RING_SPREAD:
            raise ProtocolError(f"FB_RING mode {p[2]}")
        f = {"encoder": dec_u14(p[0:2]), "mode": p[2], "value": dec_u14(p[3:5]), "center": p[5] & 1}
    elif cmd == FB_TEXT:
        if not 4 <= len(p) <= 4 + FB_TEXT_MAX:
            raise ProtocolError("FB_TEXT needs display u14, row, col and 0..64 text bytes")
        f = {"display": dec_u14(p[0:2]), "row": p[2], "col": p[3], "text": p[4:].decode("ascii")}
    elif cmd == FB_RELEASE:
        need(1)
        if p[0] > REL_TEXT:
            raise ProtocolError(f"FB_RELEASE what {p[0]}")
        f = {"what": p[0]}
    else:
        raise ProtocolError(f"unknown notification 0x{cmd:02x}")
    return Notification(cmd, f)


# ---- schema (sysex.md 4.4) ----

SCHEMA_VERSION = 1
T_INT, T_BOOL, T_ENUM, T_NOTE = 0, 1, 2, 3
TYPE_NAMES = {T_INT: "int", T_BOOL: "bool", T_ENUM: "enum", T_NOTE: "note"}
F_READ_ONLY, F_VOLATILE, F_PER_INDEX_DEFAULT, F_INDEX_LABELS, F_HAS_UNIT, F_PRESET = 1, 2, 4, 8, 16, 32
FLAG_NAMES = ((F_READ_ONLY, "read-only"), (F_VOLATILE, "volatile"), (F_PER_INDEX_DEFAULT, "per-index-default"),
              (F_INDEX_LABELS, "index-labels"), (F_HAS_UNIT, "unit"), (F_PRESET, "preset"))
MAX_COUNT = 0xFFFF
MAX_STRING = 63


@dataclass
class Param:
    id: int
    type: int
    group: int
    count: int
    flags: int
    min: int
    max: int
    defaults: list          # one entry, or `count` entries with PER_INDEX_DEFAULT
    name: str
    label: str
    unit: str = ""                                     # only with HAS_UNIT
    index_labels: list = field(default_factory=list)   # `count` strings with INDEX_LABELS, else []
    options: list = field(default_factory=list)        # enum only: option i has value min + i

    @property
    def type_name(self):
        """Unknown types are shown as plain ints (sysex.md 4.4)."""
        return TYPE_NAMES.get(self.type, "int")

    @property
    def read_only(self):
        return bool(self.flags & F_READ_ONLY)

    @property
    def volatile(self):
        return bool(self.flags & F_VOLATILE)

    @property
    def per_preset(self):
        """PRESET flag (sysex.md 4.4 bit 5): one value per preset; GET/SET_PARAM reach the active one."""
        return bool(self.flags & F_PRESET)

    def default(self, index=0):
        return self.defaults[index] if len(self.defaults) > 1 else self.defaults[0]

    def index_label(self, index):
        return self.index_labels[index] if index < len(self.index_labels) else str(index)

    def to_json(self):
        return {"id": self.id, "type": self.type, "group": self.group, "count": self.count, "flags": self.flags,
                "min": self.min, "max": self.max, "defaults": list(self.defaults), "name": self.name,
                "label": self.label, "unit": self.unit, "index_labels": list(self.index_labels), "options": list(self.options)}


@dataclass
class Schema:
    version: int
    groups: list
    params: list

    def by_name(self, name):
        return next((p for p in self.params if p.name == name), None)

    def by_id(self, pid):
        return next((p for p in self.params if p.id == pid), None)

    def to_json(self):
        return {"version": self.version, "groups": list(self.groups), "params": [p.to_json() for p in self.params]}

    @classmethod
    def from_json(cls, j):
        return cls(j["version"], list(j["groups"]), [Param(**p) for p in j["params"]])


def make_param(pid, name, ptype, group, min, max=None, default=0, label="", count=1, index_labels=None,
               options=None, flags=0, unit=""):
    """Convenience constructor that derives the layout flags: a list `default` sets PER_INDEX_DEFAULT,
    `index_labels` sets INDEX_LABELS, `unit` sets HAS_UNIT; an enum's max defaults to min + len(options) - 1."""
    defaults = list(default) if isinstance(default, (list, tuple)) else [default]
    if isinstance(default, (list, tuple)):
        flags |= F_PER_INDEX_DEFAULT
    if index_labels:
        flags |= F_INDEX_LABELS
    if unit:
        flags |= F_HAS_UNIT
    options = list(options or [])
    if max is None:
        if ptype != T_ENUM:
            raise ValueError("max is required for non-enum params")
        max = min + len(options) - 1
    return Param(pid, ptype, group, count, flags, min, max, defaults, name, label or name, unit,
                 list(index_labels or []), options)


class _Reader:
    def __init__(self, data):
        self.data, self.pos = bytes(data), 0

    def take(self, n, what):
        if self.pos + n > len(self.data):
            raise ProtocolError(f"schema truncated at byte {self.pos} of {len(self.data)} reading {what}")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self, what):
        return self.take(1, what)[0]

    def u16(self, what):
        return struct.unpack("<H", self.take(2, what))[0]

    def i32(self, what):
        return struct.unpack("<i", self.take(4, what))[0]

    def string(self, what):
        n = self.u8(what + " length")
        return self.take(n, what).decode("ascii", "replace")


def parse_schema(data):
    """Binary schema v1 (sysex.md 4.4) -> Schema. Unknown flag bits are kept but ignored, unknown types are
    treated as int (no enum trailer). Raises ProtocolError on truncated or inconsistent data."""
    r = _Reader(data)
    version = r.u8("schema_version")
    if version != SCHEMA_VERSION:
        raise ProtocolError(f"unsupported schema version {version} (this tool reads version {SCHEMA_VERSION})")
    group_count = r.u8("group_count")
    param_count = r.u16("param_count")
    groups = [r.string(f"group {i} label") for i in range(group_count)]
    params = []
    for i in range(param_count):
        where = f"param {i}"
        pid = r.u16(f"{where} id")
        where = f"param {i} (id {pid})"
        ptype = r.u8(f"{where} type")
        group = r.u8(f"{where} group")
        count = r.u16(f"{where} count")
        flags = r.u8(f"{where} flags")
        if count < 1:
            raise ProtocolError(f"{where}: count is 0")
        if group >= group_count:
            raise ProtocolError(f"{where}: group {group} but only {group_count} groups")
        pmin = r.i32(f"{where} min")
        pmax = r.i32(f"{where} max")
        ndef = count if flags & F_PER_INDEX_DEFAULT else 1
        defaults = [r.i32(f"{where} default {k}") for k in range(ndef)]
        name = r.string(f"{where} name")
        label = r.string(f"{where} label")
        unit = r.string(f"{where} unit") if flags & F_HAS_UNIT else ""
        index_labels = [r.string(f"{where} index label {k}") for k in range(count)] if flags & F_INDEX_LABELS else []
        options = []
        if ptype == T_ENUM:
            n = r.u8(f"{where} option_count")
            options = [r.string(f"{where} option {k}") for k in range(n)]
        params.append(Param(pid, ptype, group, count, flags, pmin, pmax, defaults, name, label, unit,
                            index_labels, options))
    return Schema(version, groups, params)


def _enc_string(s, what):
    b = s.encode("ascii")
    if len(b) > MAX_STRING or any(c & 0x80 for c in b):
        raise ValueError(f"{what} {s!r}: strings are 0..{MAX_STRING} bytes of 7-bit ASCII")
    return bytes([len(b)]) + b


def build_schema(schema):
    """Schema -> binary v1 (the firmware's layout; used by tests, the simulator and the vectors)."""
    out = bytearray(struct.pack("<BBH", schema.version, len(schema.groups), len(schema.params)))
    for g in schema.groups:
        out += _enc_string(g, "group label")
    for p in schema.params:
        if not 1 <= p.id <= 0x3FFF or not 1 <= p.count <= MAX_COUNT or not 0 <= p.group < len(schema.groups):
            raise ValueError(f"param {p.name}: id, count or group out of range")
        ndef = p.count if p.flags & F_PER_INDEX_DEFAULT else 1
        if len(p.defaults) != ndef:
            raise ValueError(f"param {p.name}: {len(p.defaults)} defaults, layout needs {ndef}")
        if bool(p.flags & F_INDEX_LABELS) != bool(p.index_labels) or (p.index_labels and len(p.index_labels) != p.count):
            raise ValueError(f"param {p.name}: INDEX_LABELS flag needs exactly `count` index labels")
        if bool(p.flags & F_HAS_UNIT) != bool(p.unit):
            raise ValueError(f"param {p.name}: HAS_UNIT flag and unit string disagree")
        if p.options and p.type != T_ENUM:
            raise ValueError(f"param {p.name}: options on a non-enum param")
        out += struct.pack("<HBBHBii", p.id, p.type, p.group, p.count, p.flags, p.min, p.max)
        out += b"".join(struct.pack("<i", d) for d in p.defaults)
        out += _enc_string(p.name, "name") + _enc_string(p.label, "label")
        if p.flags & F_HAS_UNIT:
            out += _enc_string(p.unit, "unit")
        for s in p.index_labels:
            out += _enc_string(s, "index label")
        if p.type == T_ENUM:
            out.append(len(p.options))
            for s in p.options:
                out += _enc_string(s, "option")
    return bytes(out)


# ---- values as text (MIDI note 60 = C4, the Yamaha/scientific-pitch convention) ----

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
_NOTE_SEMITONE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_name(n):
    """MIDI note number -> name, 60 = C4 (so 0 = C-1, 127 = G9)."""
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def parse_note(s):
    """'C4' -> 60, 'F#4' -> 66, 'Bb3' -> 58, 'C-1' -> 0. Raises ValueError."""
    s = s.strip()
    if len(s) >= 2 and s[0].upper() in _NOTE_SEMITONE:
        acc, rest = 0, s[1:]
        if rest[:1] == "#":
            acc, rest = 1, rest[1:]
        elif rest[:1] == "b":
            acc, rest = -1, rest[1:]
        if rest.lstrip("-").isdigit() and rest.count("-") <= 1:
            n = (int(rest) + 1) * 12 + _NOTE_SEMITONE[s[0].upper()] + acc
            if 0 <= n <= 127:
                return n
            raise ValueError(f"note {s} is outside MIDI 0..127")
    raise ValueError(f"{s!r} is not a note name (like C4, F#3 or Bb2; 60 = C4)")


def format_value(p, v, unit=True):
    """A value as text: enum by option label, note by name, bool as on/off, otherwise the number
    (followed by the param's unit, if it has one and `unit`)."""
    if p.type == T_ENUM:
        i = v - p.min
        return p.options[i] if 0 <= i < len(p.options) else str(v)
    if p.type == T_NOTE and 0 <= v <= 127:
        return f"{note_name(v)} ({v})"
    if p.type == T_BOOL and v in (0, 1):
        return "on" if v else "off"
    return f"{v} {p.unit}" if unit and p.unit else str(v)


_BOOL_WORDS = {"on": 1, "off": 0, "true": 1, "false": 0, "yes": 1, "no": 0}


def _int(s):
    try:
        return int(s, 10)
    except ValueError:
        return int(s, 0)        # 0x.., 0b..


def parse_value(p, text, check_range=True):
    """Text -> int for param p: ints everywhere, enum option labels (case-insensitive), note names for
    note params (60 = C4), on/off/true/false/yes/no for bool. Raises ValueError."""
    t = text.strip()
    if p.unit and t.lower().endswith(p.unit.lower()) and len(t) > len(p.unit):
        t = t[:-len(p.unit)].strip()          # "80ms" / "80 ms" for a param with unit "ms"
    v = None
    if p.type == T_ENUM:
        hits = [i for i, o in enumerate(p.options) if o.lower() == t.lower()]
        if hits:
            v = p.min + hits[0]
    elif p.type == T_NOTE:
        try:
            v = parse_note(t)
        except ValueError:
            pass
    elif p.type == T_BOOL and t.lower() in _BOOL_WORDS:
        v = _BOOL_WORDS[t.lower()]
    if v is None:
        try:
            v = _int(t)
        except ValueError:
            hint = ""
            if p.type == T_ENUM:
                hint = " (options: " + ", ".join(p.options) + ")"
            elif p.type == T_NOTE:
                hint = " (a number or a note name like C4; 60 = C4)"
            raise ValueError(f"{p.name}: can't read {text!r}{hint}") from None
    if check_range and not p.min <= v <= p.max:
        raise ValueError(f"{p.name}: {v} is outside {p.min}..{p.max}")
    return v


def parse_index(p, text):
    """An index as a number or an index label (case-insensitive). Raises ValueError."""
    t = text.strip()
    hits = [i for i, s in enumerate(p.index_labels) if s.lower() == t.lower()]
    if hits:
        i = hits[0]
    else:
        try:
            i = int(t, 10)
        except ValueError:
            raise ValueError(f"{p.name}: no index {text!r}" +
                             (f" (labels: {', '.join(p.index_labels)})" if p.index_labels else "")) from None
    if not 0 <= i < p.count:
        raise ValueError(f"{p.name}: index {i} is outside 0..{p.count - 1}")
    return i


def find_param(schema, ref):
    """'name', 'id', 'name:index' or 'id:index' -> (Param, index or None). Raises ValueError."""
    key, sep, idx = ref.partition(":")
    key = key.strip()
    p = schema.by_name(key)
    if p is None and key.isdigit():
        p = schema.by_id(int(key))
    if p is None:
        raise ValueError(f"no param {key!r} in this device's schema")
    return p, (parse_index(p, idx) if sep else None)


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


def ab_chunk(max_chunk):
    """Chunk length for an A/B device (sysex.md 4.2, "A/B devices"): the largest multiple of 4 <= max_chunk."""
    n = max_chunk - max_chunk % 4
    if n < 4:
        raise ValueError(f"max_chunk {max_chunk} is below 4")
    return n


def pad4(data):
    """The image as sent to an A/B device: padded with 0xFF to a multiple of 4 (FW_BEGIN size = the padded size)."""
    return bytes(data) + b"\xff" * (-len(data) % 4)


def ab_crc32(data):
    """CRC-32 (zlib) over the whole padded file; no header field is skipped (there is no header)."""
    return zlib.crc32(pad4(data)) & 0xFFFFFFFF


def linear_chunk_plan(size, max_chunk):
    """[(offset, length)] for an A/B device: plain order from offset 0, every length a multiple of 4 and
    <= max_chunk; no page rule. `size` is the padded size."""
    if size <= 0 or size % 4:
        raise ValueError("size must be non-zero and a multiple of 4 (pad the image first)")
    step = ab_chunk(max_chunk)
    return [(o, min(step, size - o)) for o in range(0, size, step)]


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
