"""gemifi .gfw image header (protocol/sysex.md section 5). Standard library only.

32-byte little-endian header right after the vector table. Where that is depends on the board (BOARDS):
0xC0 on NUC1xx (48 vectors, image linked at 0x0), 0x130 on STM32F1 (76 vectors, linked at 0x08002400).
Header fields:
  magic 'GMFI' (0x49464D47), hdr_version u16 = 1, board_id u16, image_size u32 (multiple of 4),
  crc32 u32 (zlib CRC-32 over [0, image_size) with this field taken as 0), fw_version u32
  (major<<24 | minor<<16 | patch), proto_version u32 = 1, reserved[2] = 0xFFFFFFFF.
"""
import struct
import zlib
from dataclasses import dataclass
from typing import NamedTuple

HDR_OFFSET = 0xC0
HDR_FMT = "<IHHIIIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)   # 32
MAGIC = 0x49464D47                    # b"GMFI" little-endian
HDR_VERSION = 1
PROTO_VERSION = 1
CRC_OFFSET = HDR_OFFSET + 12
APROM_SIZE = 32 * 1024
SRAM_BASE, SRAM_END = 0x20000000, 0x20001000


class Board(NamedTuple):
    """Per-board image layout. b[0] is the name and b[1] the size limit, as before."""
    name: str
    max_size: int              # largest image in bytes
    platform: str = "nuvoton-nuc1xx"
    hdr_offset: int = HDR_OFFSET
    base: int = 0              # flash address the image is linked at
    sram_end: int = SRAM_END   # the initial SP must be in (SRAM_BASE, sram_end]
    area: str = "APROM"        # what the size limit is called in messages


CODE61_BASE = 0x08002400
CODE61_MAX = 0x0801B3FE - CODE61_BASE      # 102,398: up to the vendor image checksum at 0x0801B3FE
BOARDS = {
    0x0001: Board("M-Audio Keystation Mini 32 MK3", APROM_SIZE),
    # Vendor bootloader below 0x08002400 stays; the app goes in through its SysEx update (maudio_update.py).
    # SRAM: the part is an STM32F103-class chip (maybe a clone); 64 KB is the upper bound of the family.
    0x0002: Board("M-Audio Code 61", CODE61_MAX, "stm32f1", 0x130, CODE61_BASE, 0x20010000, "the app area"),
}
HDR_OFFSETS = sorted({b.hdr_offset for b in BOARDS.values()})


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


class Layout(NamedTuple):
    hdr_offset: int
    base: int
    max_size: int
    sram_end: int
    area: str


def layout(board_id, hdr_offset=None, base=None, max_size=None):
    """The board's layout, with any of hdr_offset / base / max_size overridden (mkimage flags)."""
    b = BOARDS.get(board_id, BOARDS[0x0001])
    return Layout(b.hdr_offset if hdr_offset is None else hdr_offset, b.base if base is None else base,
                  b.max_size if max_size is None else max_size, b.sram_end,
                  b.area if max_size is None else "the limit")


def image_crc(img, hdr_offset=HDR_OFFSET):
    """CRC-32 over the whole image with the header's crc32 field taken as 0."""
    b = bytearray(img)
    b[hdr_offset + 12:hdr_offset + 16] = b"\0\0\0\0"
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def parse_header(img, hdr_offset=HDR_OFFSET):
    if len(img) < hdr_offset + HDR_SIZE:
        raise ImageError("image too short to hold a header")
    return Header(*struct.unpack_from(HDR_FMT, img, hdr_offset))


def find_hdr_offset(img, board_id=None):
    """Where the header of this .gfw is: the board's own offset, else the known offset holding 'GMFI' whose
    board_id says that offset. None if no known offset holds the magic."""
    if board_id in BOARDS:
        off = BOARDS[board_id].hdr_offset
        if img[off:off + 4] == b"GMFI":
            return off
    hits = [o for o in HDR_OFFSETS if img[o:o + 4] == b"GMFI"]
    for o in hits:
        bid = struct.unpack_from("<H", img, o + 6)[0]
        if bid in BOARDS and BOARDS[bid].hdr_offset == o:
            return o
    return hits[0] if hits else None


def check_vectors(img, size, base=0, sram_end=SRAM_END):
    errs = []
    sp, rv = struct.unpack_from("<II", img, 0)
    if not (SRAM_BASE < sp <= sram_end):
        errs.append(f"initial SP 0x{sp:08x} is not in SRAM")
    if not (rv & 1):
        errs.append(f"reset vector 0x{rv:08x} is not a Thumb address")
    if not base <= (rv & ~1) < base + size:
        errs.append(f"reset vector 0x{rv:08x} is outside the image (0x{base:08x}..0x{base + size:08x})")
    return errs


def make_image(raw, board_id, fw_version, check_vec=True, hdr_offset=None, base=None, max_size=None):
    """Return the .gfw bytes: raw padded with 0xFF to a multiple of 4, header filled in at the board's offset
    (or hdr_offset). base and max_size override the board's link address and size limit."""
    if not 0 <= board_id <= 0x3FFF:
        raise ImageError(f"board_id 0x{board_id:x} doesn't fit a u14 (max 0x3FFF)")
    if board_id not in BOARDS:
        raise ImageError(f"unknown board_id 0x{board_id:04x} (known: {', '.join(f'0x{b:04x}' for b in BOARDS)})")
    lay = layout(board_id, hdr_offset, base, max_size)
    off = lay.hdr_offset
    img = bytearray(raw)
    if len(img) < off + HDR_SIZE:
        raise ImageError(f"input is {len(img)} bytes; it must reach past the header (0x{off + HDR_SIZE:x})")
    img += b"\xff" * (-len(img) % 4)
    if len(img) > lay.max_size:
        raise ImageError(f"image is {len(img)} bytes; {lay.area} is {lay.max_size} bytes")
    old = bytes(img[off:off + HDR_SIZE])
    if not (old == b"\0" * HDR_SIZE or old == b"\xff" * HDR_SIZE or old[:4] == b"GMFI"):
        raise ImageError(f"bytes at 0x{off:X}..0x{off + HDR_SIZE - 1:X} are not a blank or existing header: is the "
                         f".gemifi_hdr section placed at 0x{off:X} in the linker script?")
    if check_vec:
        errs = check_vectors(img, len(img), lay.base, lay.sram_end)
        if errs:
            raise ImageError("; ".join(errs))
    hdr = Header(MAGIC, HDR_VERSION, board_id, len(img), 0, fw_version, PROTO_VERSION, 0xFFFFFFFF, 0xFFFFFFFF)
    img[off:off + HDR_SIZE] = hdr.pack()
    hdr.crc32 = image_crc(img, off)
    img[off:off + HDR_SIZE] = hdr.pack()
    return bytes(img)


def check_image(img, board_id=None, check_vec=True, hdr_offset=None, base=None, max_size=None):
    """Validate a .gfw the way the bootloader will (sysex.md section 5). Returns the Header or raises.
    The header is looked for at the board's offset (hdr_offset overrides)."""
    explicit = hdr_offset is not None
    if not explicit:
        hdr_offset = find_hdr_offset(img, board_id)
        if hdr_offset is None:
            hdr_offset = BOARDS[board_id].hdr_offset if board_id in BOARDS else HDR_OFFSET
    h = parse_header(img, hdr_offset)
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
    lay = layout(h.board_id, hdr_offset, base, max_size)
    if not explicit and h.board_id in BOARDS and BOARDS[h.board_id].hdr_offset != hdr_offset and not errs:
        errs.append(f"header at 0x{hdr_offset:x}, but board 0x{h.board_id:04x} has it at "
                    f"0x{BOARDS[h.board_id].hdr_offset:x}")
    limit = lay.max_size
    if h.image_size % 4 or h.image_size == 0 or h.image_size > limit:
        errs.append(f"image_size {h.image_size} is not a non-zero multiple of 4 <= {limit}")
    if h.image_size != len(img):
        errs.append(f"image_size {h.image_size} != file size {len(img)} (a .gfw is exactly image_size bytes)")
    if h.proto_version != PROTO_VERSION:
        errs.append(f"proto_version {h.proto_version} is not {PROTO_VERSION}")
    if (h.reserved0, h.reserved1) != (0xFFFFFFFF, 0xFFFFFFFF):
        errs.append("reserved words are not 0xFFFFFFFF")
    if not errs:
        crc = image_crc(img[:h.image_size], hdr_offset)
        if crc != h.crc32:
            errs.append(f"CRC 0x{h.crc32:08x} in header, computed 0x{crc:08x}")
        if check_vec:
            errs += check_vectors(img, h.image_size, lay.base, lay.sram_end)
    if errs:
        raise ImageError("; ".join(errs))
    return h
