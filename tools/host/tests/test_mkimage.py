import io
import os
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout

HOST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HOST)
import gemifi_image as gi  # noqa: E402
import mkimage  # noqa: E402


def raw_image(n=254, sp=0x20000FF0, reset=0xE1):
    raw = bytearray((i * 7 + 3) & 0xFF for i in range(n))
    struct.pack_into("<II", raw, 0, sp, reset)
    raw[0xC0:0xE0] = bytes(32)
    return bytes(raw)


class TestHeader(unittest.TestCase):
    def test_fields(self):
        img = gi.make_image(raw_image(), 1, gi.parse_version("1.2.3"))
        self.assertEqual(len(img), 256)                    # padded to a multiple of 4
        self.assertEqual(img[254:256], b"\xff\xff")        # with 0xFF
        self.assertEqual(img[0xC0:0xC4], b"GMFI")
        magic, hv, board, size, crc, ver, pv, r0, r1 = struct.unpack_from("<IHHIIIIII", img, 0xC0)
        self.assertEqual(magic, 0x49464D47)
        self.assertEqual((hv, board, size, ver, pv, r0, r1), (1, 1, 256, 0x01020003, 1, 0xFFFFFFFF, 0xFFFFFFFF))
        zeroed = bytearray(img); zeroed[0xCC:0xD0] = bytes(4)
        self.assertEqual(crc, zlib.crc32(bytes(zeroed)))
        self.assertEqual(gi.check_image(img, 1).crc32, crc)

    def test_vector_and_size_checks(self):
        with self.assertRaisesRegex(gi.ImageError, "APROM"):
            gi.make_image(raw_image(32 * 1024 + 4), 1, 0)
        gi.make_image(raw_image(32 * 1024), 1, 0)               # exactly 32 KB is fine
        with self.assertRaisesRegex(gi.ImageError, "SP"):
            gi.make_image(raw_image(sp=0x10000000), 1, 0)
        with self.assertRaisesRegex(gi.ImageError, "Thumb"):
            gi.make_image(raw_image(reset=0xE0), 1, 0)
        with self.assertRaisesRegex(gi.ImageError, "outside"):
            gi.make_image(raw_image(reset=0x1001), 1, 0)
        gi.make_image(raw_image(sp=0), 1, 0, check_vec=False)
        with self.assertRaisesRegex(gi.ImageError, "board"):
            gi.make_image(raw_image(), 7, 0)
        with self.assertRaisesRegex(gi.ImageError, "u14"):
            gi.make_image(raw_image(), 0x4000, 0)
        with self.assertRaisesRegex(gi.ImageError, "past the header"):
            gi.make_image(bytes(100), 1, 0)

    def test_refuses_code_in_header_slot(self):
        raw = bytearray(raw_image()); raw[0xC4] = 0x12
        with self.assertRaisesRegex(gi.ImageError, "0xC0"):
            gi.make_image(bytes(raw), 1, 0)

    def test_restamp_existing(self):
        a = gi.make_image(raw_image(), 1, gi.parse_version("1.0.0"))
        b = gi.make_image(a, 1, gi.parse_version("2.0.0"))
        self.assertEqual(gi.check_image(b).fw_version, 0x02000000)

    def test_check_detects_corruption(self):
        img = bytearray(gi.make_image(raw_image(), 1, 0))
        img[0x50] ^= 1
        with self.assertRaisesRegex(gi.ImageError, "CRC"):
            gi.check_image(bytes(img))
        img = gi.make_image(raw_image(), 1, 0)
        with self.assertRaisesRegex(gi.ImageError, "board_id"):
            gi.check_image(img, board_id=2)
        with self.assertRaisesRegex(gi.ImageError, "file size"):
            gi.check_image(img + b"\xff\xff\xff\xff")

    def test_version(self):
        self.assertEqual(gi.parse_version("1.2.3"), 0x01020003)
        self.assertEqual(gi.fmt_version(0x01020003), "1.2.3")
        for bad in ("1.2", "256.0.0", "1.2.65536", "a.b.c"):
            with self.assertRaises(ValueError):
                gi.parse_version(bad)


class TestCli(unittest.TestCase):
    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = mkimage.main(list(args))
        return rc, out.getvalue() + err.getvalue()

    def test_build_and_check(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "app.bin"), os.path.join(d, "app.gfw")
            with open(src, "wb") as f:
                f.write(raw_image())
            rc, out = self.run_cli(src, dst, "--board", "1", "--version", "0.1.0")
            self.assertEqual(rc, 0, out)
            rc, out = self.run_cli("--check", dst)
            self.assertEqual(rc, 0, out)
            self.assertIn("version 0.1.0", out)
            rc, out = self.run_cli("--check", dst, "--board", "2")
            self.assertEqual(rc, 1)
            with open(src, "wb") as f:
                f.write(raw_image(40000))
            rc, out = self.run_cli(src, dst + "2", "--board", "0x0001", "--version", "0.1.0")
            self.assertEqual(rc, 1)
            self.assertFalse(os.path.exists(dst + "2"))


if __name__ == "__main__":
    unittest.main()
