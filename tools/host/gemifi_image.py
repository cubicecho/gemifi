"""gemifi .gfw image header (protocol/sysex.md section 5). Standard library only.

32-byte little-endian header at 0xC0, right after the 48-entry vector table:
  magic 'GMFI' (0x49464D47), hdr_version u16 = 1, board_id u16, image_size u32 (multiple of 4),
  crc32 u32 (zlib CRC-32 over [0, image_size) with this field taken as 0), fw_version u32
  (major<<24 | minor<<16 | patch), proto_version u32 = 1, reserved[2] = 0xFFFFFFFF.
"""
import struct
import zlib
from dataclasses import dataclass

HDR_OFFSET = 0xC0
HDR_FMT = "<IHHIIIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)   # 32
MAGIC = 0x49464D47                    # b"GMFI" little-endian
HDR_VERSION = 1
PROTO_VERSION = 1
CRC_OFFSET = HDR_OFFSET + 12
APROM_SIZE = 32 * 1024
SRAM_BASE, SRAM_END = 0x20000000, 0x20001000
BOARDS = {0x0001: ("M-Audio Keystation Mini 32 MK3", APROM_SIZE)}


class ImageError(ValueError):
    pass


@dataclass
class Header:
    magic: int
    hdr_version: int
    board_id: int
    image_size: int
    crc32: int
    fw_version: int
    proto_version: int
    reserved0: int
    reserved1: int

    def pack(self):
        return struct.pack(HDR_FMT, self.magic, self.hdr_version, self.board_id, self.image_size, self.crc32,
                           self.fw_version, self.proto_version, self.reserved0, self.reserved1)


def parse_version(s):
    parts = s.split(".")
    if len(parts) != 3:
        raise ValueError("version must be major.minor.patch")
    ma, mi, pa = (int(p, 10) for p in parts)
    if not (0 <= ma <= 255 and 0 <= mi <= 255 and 0 <= pa <= 0xFFFF):
        raise ValueError("version out of range (major, minor 0..255; patch 0..65535)")
    return ma << 24 | mi << 16 | pa


def fmt_version(v):
    return f"{v >> 24}.{(v >> 16) & 0xFF}.{v & 0xFFFF}"


def image_crc(img):
    """CRC-32 over the whole image with the header's crc32 field taken as 0."""
    b = bytearray(img)
    b[CRC_OFFSET:CRC_OFFSET + 4] = b"\0\0\0\0"
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def parse_header(img):
    if len(img) < HDR_OFFSET + HDR_SIZE:
        raise ImageError("image too short to hold a header")
    return Header(*struct.unpack_from(HDR_FMT, img, HDR_OFFSET))


def check_vectors(img, size):
    errs = []
    sp, rv = struct.unpack_from("<II", img, 0)
    if not (SRAM_BASE < sp <= SRAM_END):
        errs.append(f"initial SP 0x{sp:08x} is not in SRAM")
    if not (rv & 1):
        errs.append(f"reset vector 0x{rv:08x} is not a Thumb address")
    if (rv & ~1) >= size:
        errs.append(f"reset vector 0x{rv:08x} is outside the image")
    return errs


def make_image(raw, board_id, fw_version, check_vec=True):
    """Return the .gfw bytes: raw padded with 0xFF to a multiple of 4, header filled at 0xC0."""
    if not 0 <= board_id <= 0x3FFF:
        raise ImageError(f"board_id 0x{board_id:x} doesn't fit a u14 (max 0x3FFF)")
    if board_id not in BOARDS:
        raise ImageError(f"unknown board_id 0x{board_id:04x} (known: {', '.join(f'0x{b:04x}' for b in BOARDS)})")
    limit = BOARDS[board_id][1]
    img = bytearray(raw)
    if len(img) < HDR_OFFSET + HDR_SIZE:
        raise ImageError(f"input is {len(img)} bytes; it must reach past the header (0x{HDR_OFFSET + HDR_SIZE:x})")
    img += b"\xff" * (-len(img) % 4)
    if len(img) > limit:
        raise ImageError(f"image is {len(img)} bytes; APROM is {limit} bytes")
    old = bytes(img[HDR_OFFSET:HDR_OFFSET + HDR_SIZE])
    if not (old == b"\0" * HDR_SIZE or old == b"\xff" * HDR_SIZE or old[:4] == b"GMFI"):
        raise ImageError("bytes at 0xC0..0xDF are not a blank or existing header: is the "
                         ".gemifi_hdr section placed at 0xC0 in the linker script?")
    if check_vec:
        errs = check_vectors(img, len(img))
        if errs:
            raise ImageError("; ".join(errs))
    hdr = Header(MAGIC, HDR_VERSION, board_id, len(img), 0, fw_version, PROTO_VERSION, 0xFFFFFFFF, 0xFFFFFFFF)
    img[HDR_OFFSET:HDR_OFFSET + HDR_SIZE] = hdr.pack()
    hdr.crc32 = image_crc(img)
    img[HDR_OFFSET:HDR_OFFSET + HDR_SIZE] = hdr.pack()
    return bytes(img)


def check_image(img, board_id=None, check_vec=True):
    """Validate a .gfw the way the bootloader will (sysex.md section 5). Returns the Header or raises."""
    h = parse_header(img)
    errs = []
    if h.magic != MAGIC:
        errs.append(f"magic 0x{h.magic:08x} is not 'GMFI'")
    if h.hdr_version != HDR_VERSION:
        errs.append(f"hdr_version {h.hdr_version} is not {HDR_VERSION}")
    if h.board_id > 0x3FFF:
        errs.append(f"board_id 0x{h.board_id:04x} doesn't fit a u14 (max 0x3FFF)")
    elif h.board_id not in BOARDS:
        errs.append(f"unknown board_id 0x{h.board_id:04x}")
    if board_id is not None and h.board_id != board_id:
        errs.append(f"board_id 0x{h.board_id:04x} is not the expected 0x{board_id:04x}")
    limit = BOARDS.get(h.board_id, ("", APROM_SIZE))[1]
    if h.image_size % 4 or h.image_size == 0 or h.image_size > limit:
        errs.append(f"image_size {h.image_size} is not a non-zero multiple of 4 <= {limit}")
    if h.image_size != len(img):
        errs.append(f"image_size {h.image_size} != file size {len(img)} (a .gfw is exactly image_size bytes)")
    if h.proto_version != PROTO_VERSION:
        errs.append(f"proto_version {h.proto_version} is not {PROTO_VERSION}")
    if (h.reserved0, h.reserved1) != (0xFFFFFFFF, 0xFFFFFFFF):
        errs.append("reserved words are not 0xFFFFFFFF")
    if not errs:
        crc = image_crc(img[:h.image_size])
        if crc != h.crc32:
            errs.append(f"CRC 0x{h.crc32:08x} in header, computed 0x{crc:08x}")
        if check_vec:
            errs += check_vectors(img, h.image_size)
    if errs:
        raise ImageError("; ".join(errs))
    return h
