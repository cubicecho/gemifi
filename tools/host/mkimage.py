#!/usr/bin/env python3
"""Make or check a gemifi .gfw firmware image (protocol/sysex.md section 5).

  mkimage.py <in.bin> <out.gfw> --board <id> --version <maj.min.patch>
  mkimage.py --check <file.gfw> [--board <id>]

Building pads the raw APROM binary with 0xFF to a multiple of 4, fills the 32-byte header at 0xC0
(magic 'GMFI', hdr_version 1, board_id, image_size, CRC-32 with the crc field as 0, fw_version,
proto_version 1, reserved 0xFFFFFFFF x2) and refuses images over 32 KB. The bytes at 0xC0..0xDF must
be blank (0x00 or 0xFF) or an existing header, so a link that put code there is caught.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gemifi_image as gi  # noqa: E402


def describe(h):
    name = gi.BOARDS.get(h.board_id, ("?",))[0]
    return (f"board 0x{h.board_id:04x} ({name}), version {gi.fmt_version(h.fw_version)}, "
            f"{h.image_size} bytes, crc32 0x{h.crc32:08x}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="raw APROM binary linked at 0x0")
    ap.add_argument("output", nargs="?", help="output .gfw")
    ap.add_argument("--board", type=lambda s: int(s, 0), help="board_id (Keystation Mini 32 MK3 = 1)")
    ap.add_argument("--version", help="firmware version major.minor.patch")
    ap.add_argument("--check", metavar="FILE", help="validate an existing .gfw and exit")
    ap.add_argument("--no-vector-check", action="store_true",
                    help="skip the initial-SP / reset-vector sanity check")
    a = ap.parse_args(argv)
    vec = not a.no_vector_check
    try:
        if a.check:
            with open(a.check, "rb") as f:
                h = gi.check_image(f.read(), a.board, vec)
            print(f"OK: {a.check}: {describe(h)}")
            return 0
        if not (a.input and a.output and a.board is not None and a.version):
            ap.error("need <in.bin> <out.gfw> --board <id> --version <maj.min.patch> (or --check FILE)")
        with open(a.input, "rb") as f:
            img = gi.make_image(f.read(), a.board, gi.parse_version(a.version), vec)
        gi.check_image(img, a.board, vec)
        tmp = a.output + ".tmp"
        with open(tmp, "wb") as f:
            f.write(img)
        os.replace(tmp, a.output)
        print(f"{a.output}: {describe(gi.parse_header(img))}")
        return 0
    except (gi.ImageError, ValueError, OSError) as e:
        print(f"mkimage: ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
