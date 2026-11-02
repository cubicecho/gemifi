"""Offline tests for isp-compare.py, isp-assemble.py and isp_dumpfile.py, with fake dump files.

Run: cd tools/swd/nuvoton-nuc1xx && python3 -m unittest   (or pytest tests/)
"""
import os
import subprocess
import sys
import tempfile
import unittest

SWD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SWD)
import isp_dumpfile  # noqa: E402

BASE = 0x00100000


def image(n=64):
    return bytes((i * 37 + 11) & 0xFF for i in range(n))


def pass_lines(data, base=BASE, size=None, skip=(), override=None):
    """'addr value' lines for data (0xFF padded to size), leaving out addresses in skip."""
    size = size or (len(data) + 3) // 4 * 4
    data = data + b"\xff" * (size - len(data))
    override = override or {}
    out = []
    for i in range(0, size, 4):
        a = base + i
        if a in skip:
            continue
        v = override.get(a, int.from_bytes(data[i:i + 4], "little"))
        out.append(f"{a:08x} {v:08x}\n")
    return "".join(out)


class Tmp(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.d = self.td.name
        os.makedirs(os.path.join(self.d, "isp"))

    def tearDown(self):
        self.td.cleanup()

    def write(self, rel, content, mode="w"):
        p = os.path.join(self.d, rel)
        with open(p, mode) as f:
            f.write(content)
        return p

    def run_py(self, script, *args):
        return subprocess.run([sys.executable, os.path.join(SWD, script), *map(str, args)],
                              capture_output=True, text=True, timeout=30)


class TestCompare(Tmp):
    def compare(self, passtext, data, *extra):
        img = self.write("img.bin", data, "wb")
        p = self.write("isp/ldrom-pass1.txt", passtext) if passtext is not None else os.path.join(self.d, "isp/ldrom-pass1.txt")
        return self.run_py("isp-compare.py", p, img, "--base", hex(BASE), *extra)

    def test_full_match_single_pass(self):
        r = self.compare(pass_lines(image()), image(), "--out", os.path.join(self.d, "rb.bin"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("VERIFY OK: all 16 words", r.stdout)
        with open(os.path.join(self.d, "rb.bin"), "rb") as f:
            self.assertEqual(f.read(), image())

    def test_odd_size_image_padded_with_ff(self):
        data = image(61)  # last word: 1 byte of data + 3 x 0xFF
        r = self.compare(pass_lines(data), data)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("64 bytes", r.stdout)

    def test_mismatch_reports_addresses(self):
        bad = {BASE + 0x10: 0xDEADBEEF, BASE + 0x3C: 0}
        r = self.compare(pass_lines(image(), override=bad), image())
        self.assertEqual(r.returncode, 1)
        self.assertIn("VERIFY FAILED: 2 of 16 words differ", r.stdout)
        self.assertIn("0x00100010  +0x0010", r.stdout)
        self.assertIn("deadbeef", r.stdout)
        self.assertIn("0x0010003c", r.stdout)

    def test_mismatch_list_is_capped(self):
        bad = {BASE + i: 0 for i in range(0, 64, 4)}
        r = self.compare(pass_lines(image(), override=bad), image(), "--show", "3")
        self.assertEqual(r.returncode, 1)
        self.assertIn("and 13 more: 0x0010000c..0x0010003c", r.stdout)

    def test_partial_dump_is_incomplete_not_failed(self):
        skip = {BASE + i for i in range(0x20, 0x40, 4)}
        out = os.path.join(self.d, "rb.bin")
        r = self.compare(pass_lines(image(), skip=skip), image(), "--out", out)
        self.assertEqual(r.returncode, 3, r.stdout)
        self.assertIn("INCOMPLETE: 8 of 16 words not read yet: 0x00100020..0x0010003c", r.stdout)
        self.assertIn("No differences in the 8 words read so far", r.stdout)
        self.assertFalse(os.path.exists(out), "no read-back file until every word is read")

    def test_gaps_listed_as_ranges(self):
        skip = {BASE + 0x8, BASE + 0x20, BASE + 0x24}
        r = self.compare(pass_lines(image(), skip=skip), image())
        self.assertEqual(r.returncode, 3)
        self.assertIn("0x00100008, 0x00100020..0x00100024", r.stdout)

    def test_partial_with_mismatch_is_failure(self):
        r = self.compare(pass_lines(image(), skip={BASE + 0x30}, override={BASE: 1}), image())
        self.assertEqual(r.returncode, 1)
        self.assertIn("VERIFY FAILED: 1 of 16 words differ", r.stdout)
        self.assertIn("Also incomplete: 1 of 16", r.stdout)

    def test_missing_pass_file(self):
        r = self.compare(None, image())
        self.assertEqual(r.returncode, 3)
        self.assertIn("not found", r.stdout)
        self.assertIn("16 of 16 words not read yet", r.stdout)

    def test_cut_short_lines_are_ignored(self):
        # Half a line from a killed session, glued to the next session's first line, plus a stray fragment.
        text = pass_lines(image(), skip={BASE + 0x10})
        text = text.replace(f"{BASE + 0x14:08x}", f"{BASE + 0x10:08x} 12{BASE + 0x14:08x}", 1) + "0010\n"
        r = self.compare(text, image())
        self.assertEqual(r.returncode, 3, r.stdout)
        self.assertIn("cut-short line(s)", r.stdout)
        self.assertIn("0x00100010..0x00100014", r.stdout)
        # The resume re-reads them (appended at the end): now complete.
        text += pass_lines(image())[len(pass_lines(image()[:0x10])):]
        r = self.compare(text, image())
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_conflicting_reads_fail(self):
        text = pass_lines(image()) + f"{BASE + 4:08x} 00000000\n"
        r = self.compare(text, image())
        self.assertEqual(r.returncode, 1)
        self.assertIn("read twice with different values", r.stdout)

    def test_size_checks_rest_is_erased(self):
        r = self.compare(pass_lines(image(), size=128), image(), "--size", "128")
        self.assertEqual(r.returncode, 0, r.stdout)
        r = self.compare(pass_lines(image(), size=128, override={BASE + 100: 0}), image(), "--size", "128")
        self.assertEqual(r.returncode, 1)
        self.assertIn("0x00100064", r.stdout)

    def test_size_smaller_than_image_rejected(self):
        r = self.compare(pass_lines(image()), image(), "--size", "32")
        self.assertEqual(r.returncode, 2)


class TestAssemble(Tmp):
    def test_two_passes_identical(self):
        self.write("isp/ldrom-pass1.txt", pass_lines(image()))
        self.write("isp/ldrom-pass2.txt", pass_lines(image()))
        r = self.run_py("isp-assemble.py", self.d, "ldrom", hex(BASE), 64)
        self.assertEqual(r.returncode, 0, r.stdout)
        with open(os.path.join(self.d, "ldrom.bin"), "rb") as f:
            self.assertEqual(f.read(), image())

    def test_missing_pass2_is_clear(self):
        self.write("isp/ldrom-pass1.txt", pass_lines(image()))
        r = self.run_py("isp-assemble.py", self.d, "ldrom", hex(BASE), 64)
        self.assertEqual(r.returncode, 1)
        self.assertNotIn("Traceback", r.stdout + r.stderr)
        self.assertIn("ldrom-pass2.txt not found", r.stdout)
        self.assertIn("isp-compare.py", r.stdout)

    def test_single_pass_flag(self):
        self.write("isp/ldrom-pass1.txt", pass_lines(image()))
        r = self.run_py("isp-assemble.py", self.d, "ldrom", hex(BASE), 64, "--passes", 1)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("single pass", r.stdout)

    def test_passes_differ_and_missing(self):
        self.write("isp/ldrom-pass1.txt", pass_lines(image(), skip={BASE + 0x3C}))
        self.write("isp/ldrom-pass2.txt", pass_lines(image(), override={BASE + 8: 0}))
        r = self.run_py("isp-assemble.py", self.d, "ldrom", hex(BASE), 64)
        self.assertEqual(r.returncode, 1)
        self.assertIn("1 missing, 1 differ", r.stdout)
        self.assertIn("missing: 0x0010003c", r.stdout)
        self.assertIn("differ: 0x00100008", r.stdout)

    def test_config(self):
        cfg = (0xFF4FFF7F).to_bytes(4, "little") + (0x0001F000).to_bytes(4, "little")
        for n in (1, 2):
            self.write(f"isp/config-pass{n}.txt", pass_lines(cfg, base=0x00300000))
        r = self.run_py("isp-assemble.py", self.d, "config", "0x00300000", 8, "--config")
        self.assertEqual(r.returncode, 0, r.stdout)
        with open(os.path.join(self.d, "config.txt")) as f:
            self.assertEqual(f.read(), "CONFIG0 0xff4fff7f\nCONFIG1 0x0001f000\n")


class TestDumpfile(unittest.TestCase):
    def test_ranges(self):
        self.assertEqual(isp_dumpfile.ranges([8, 0, 4, 16, 24, 20]), [(0, 8), (16, 24)])
        self.assertEqual(isp_dumpfile.fmt_ranges([]), "")


if __name__ == "__main__":
    unittest.main()
