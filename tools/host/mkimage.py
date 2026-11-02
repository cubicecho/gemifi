#!/usr/bin/env python3
"""Make or check a gemifi .gfw firmware image (protocol/sysex.md section 5).

  mkimage.py <in.bin> <out.gfw> --board <id> --version <maj.min.patch>
             [--hdr-offset 0x130] [--base 0x08002400] [--max-size 102398]
  mkimage.py --check <file.gfw> [--board <id>]

Building pads the raw binary with 0xFF to a multiple of 4, fills the 32-byte header (magic 'GMFI',
hdr_version 1, board_id, image_size, CRC-32 with the crc field as 0, fw_version, proto_version 1,
reserved 0xFFFFFFFF x2) and refuses images over the board's limit. Where the header goes, the link address
and the limit come from the board table in gemifi_image.py; the flags override them:

  board 0x0001 Keystation Mini 32 MK3 (NUC1xx)  header 0xC0,  base 0x0,        max 32768
  board 0x0002 Code 61 (STM32F1)                header 0x130, base 0x08002400, max 102398

The header bytes must be blank (0x00 or 0xFF) or an existing header, so a link that put code there is caught.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemifi_image as gi  # noqa: E402


def describe(h):
    name = gi.BOARDS.get(h.board_id, ("?",))[0]
    plat = gi.BOARDS[h.board_id].platform if h.board_id in gi.BOARDS else "?"
    name = f"{name}, {plat}"
    return (f"board 0x{h.board_id:04x} ({name}), version {gi.fmt_version(h.fw_version)}, "
            f"{h.image_size} bytes, crc32 0x{h.crc32:08x}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="raw binary (linked at the board's base address)")
    ap.add_argument("output", nargs="?", help="output .gfw")
    ap.add_argument("--board", type=lambda s: int(s, 0), help="board_id (Keystation Mini 32 MK3 = 1, Code 61 = 2)")
    ap.add_argument("--version", help="firmware version major.minor.patch")
    num = lambda s: int(s, 0)  # noqa: E731
    ap.add_argument("--hdr-offset", type=num, help="header offset in the image (default: from the board)")
    ap.add_argument("--base", type=num, help="flash address the image is linked at (default: from the board)")
    ap.add_argument("--max-size", type=num, help="largest image in bytes (default: from the board)")
    ap.add_argument("--check", metavar="FILE", help="validate an existing .gfw and exit")
    ap.add_argument("--no-vector-check", action="store_true",
                    help="skip the initial-SP / reset-vector sanity check")
    a = ap.parse_args(argv)
    vec = not a.no_vector_check
    lay = {"hdr_offset": a.hdr_offset, "base": a.base, "max_size": a.max_size}
    try:
        if a.check:
            with open(a.check, "rb") as f:
                h = gi.check_image(f.read(), a.board, vec, **lay)
            print(f"OK: {a.check}: {describe(h)}")
            return 0
        if not (a.input and a.output and a.board is not None and a.version):
            ap.error("need <in.bin> <out.gfw> --board <id> --version <maj.min.patch> (or --check FILE)")
        with open(a.input, "rb") as f:
            img = gi.make_image(f.read(), a.board, gi.parse_version(a.version), vec, **lay)
        gi.check_image(img, a.board, vec, **lay)
        b = gi.BOARDS[a.board]
        for k, v in lay.items():
            if v is not None and v != getattr(b, k):
                print(f"mkimage: warning: --{k.replace('_', '-')} 0x{v:x} differs from board 0x{a.board:04x}'s "
                      f"0x{getattr(b, k):x}; gemifi.py and the firmware use the board table", file=sys.stderr)
        tmp = a.output + ".tmp"
        with open(tmp, "wb") as f:
            f.write(img)
        os.replace(tmp, a.output)
        print(f"{a.output}: {describe(gi.parse_header(img, gi.find_hdr_offset(img, a.board)))}")
        return 0
    except (gi.ImageError, ValueError, OSError) as e:
        print(f"mkimage: ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
