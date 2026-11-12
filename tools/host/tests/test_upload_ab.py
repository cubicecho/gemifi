"""A/B uploads (ESP32: the app takes FW_* itself, sysex.md 4.1 "Who takes uploads", 4.2 "A/B devices")
and the serial transport, against tests/sim_device.py."""
import argparse
import contextlib
import io
import os
import random
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_midi as gm  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import SimBackend, SimDevice, SimPort, SimSerial  # noqa: E402

BOARD = 0x3F00
V_OLD, V_NEW = 0x00010000, 0x01020003       # 1.0.0 -> 1.2.3


def esp_bin(size, seed=1):
    rnd = random.Random(seed)
    return bytes([0xE9]) + bytes(rnd.randrange(256) for _ in range(size - 1))


def flash_args(path, **kw):
    a = argparse.Namespace(file=path, board=BOARD, version="1.2.3", dry_run=False, max_chunk=256, port=None,
                           boot_port=None, wait=3.0, quiet=True, retries=5, verbose=False)
    for k, v in kw.items():
        setattr(a, k, v)
    return a


class TestPlan(unittest.TestCase):
    def test_linear_plan(self):
        self.assertEqual(gp.linear_chunk_plan(1004, 256), [(0, 256), (256, 256), (512, 256), (768, 236)])
        self.assertEqual(gp.linear_chunk_plan(8, 6), [(0, 4), (4, 4)])        # rounded down to a multiple of 4
        for bad in (0, 1003):
            with self.assertRaises(ValueError):
                gp.linear_chunk_plan(bad, 256)
        with self.assertRaises(ValueError):
            gp.linear_chunk_plan(8, 3)

    def test_pad_and_crc(self):
        self.assertEqual(gp.pad4(b"\x01\x02\x03\x04\x05"), b"\x01\x02\x03\x04\x05\xff\xff\xff")
        self.assertEqual(gp.pad4(b"\x01\x02\x03\x04"), b"\x01\x02\x03\x04")
        self.assertEqual(gp.ab_crc32(b"\x05"), zlib.crc32(b"\x05\xff\xff\xff"))


class TestUploadAB(unittest.TestCase):
    def session(self, dev):
        port = SimPort(dev)
        return gemifi.Session(port, timeout=0.005, retries=5), port

    def test_order_padding_crc(self):
        for size, mc in ((1001, 256), (4096, 256), (4, 4), (777, 64), (5000, 1000)):
            img = esp_bin(size, size)
            dev = SimDevice(ab=True, board_id=BOARD, max_chunk=mc)
            s, port = self.session(dev)
            got_mc, n, padded, crc = gemifi.upload_ab(s, img, BOARD, V_NEW)
            want = gp.pad4(img)
            self.assertEqual(padded, len(want))
            self.assertEqual(padded % 4, 0)
            self.assertEqual(crc, zlib.crc32(want))
            self.assertEqual(dev.size, padded)               # FW_BEGIN size = padded size
            self.assertEqual(dev.crc, crc)
            self.assertEqual(dev.ver, V_NEW)
            # plain order from 0, every chunk a multiple of 4, <= max_chunk, no gaps
            off = 0
            for o, k in dev.ab_chunks:
                self.assertEqual(o, off)
                self.assertEqual(k % 4, 0)
                self.assertLessEqual(k, mc)
                off += k
            self.assertEqual(off, padded)
            self.assertEqual(dev.pending, (want, V_NEW))
            self.assertTrue(want.endswith(b"\xff" * (-size % 4)))
            self.assertEqual(dev.log[0], "FW_BEGIN")
            self.assertEqual(dev.log[-1], "FW_END")
            self.assertNotIn("REBOOT", dev.log)                # upload_ab leaves the reboot to the caller

    def test_lost_replies(self):
        img = esp_bin(3000)
        dev = SimDevice(ab=True, board_id=BOARD, max_chunk=128, drop_replies={2, 5, 9})
        s, _ = self.session(dev)
        gemifi.upload_ab(s, img, BOARD, V_NEW)
        self.assertEqual(dev.pending[0], gp.pad4(img))
        self.assertGreaterEqual(s.resends, 3)

    def test_device_bad_board(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        s, _ = self.session(dev)
        with self.assertRaisesRegex(gemifi.StatusError, "BAD_BOARD"):
            gemifi.upload_ab(s, esp_bin(100), BOARD + 1, V_NEW)

    def test_hello_says_ab(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        s, _ = self.session(dev)
        h = s.hello()
        self.assertTrue(gemifi.is_ab(h))
        self.assertFalse(gemifi.is_ab(self.session(SimDevice(role=gp.ROLE_APP))[0].hello()))
        self.assertFalse(gemifi.is_ab(self.session(SimDevice())[0].hello()))       # bootloader


class TestFlashCommand(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, data, name="app.bin"):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def run_flash(self, a, backend):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = gemifi.cmd_flash(a, lambda: backend)
        return rc, out.getvalue(), err.getvalue()

    def test_ab_flash_end_to_end(self):
        img = esp_bin(2001)
        dev = SimDevice(ab=True, board_id=BOARD, fw_version=V_OLD)
        rc, out, _ = self.run_flash(flash_args(self.write(img)), SimBackend(dev))
        self.assertEqual(rc, 0)
        self.assertEqual(dev.slot, gp.pad4(img))
        self.assertEqual(dev.fw_version, V_NEW)
        self.assertEqual(dev.reboots, [gp.REBOOT_APP])           # REBOOT(0) once at the end, never REBOOT(1)
        self.assertNotIn(gp.REBOOT_BOOT, dev.reboots)
        i_end = dev.log.index("FW_END")
        self.assertEqual(dev.log[i_end + 1], "REBOOT")
        self.assertEqual(dev.log[-1], "HELLO")                   # version check after the reboot
        self.assertIn("1.2.3", out)

    def test_board_mismatch_refused_locally(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        with self.assertRaisesRegex(gemifi.GemifiError, "board_id"):
            self.run_flash(flash_args(self.write(esp_bin(100)), board=BOARD + 1), SimBackend(dev))
        self.assertEqual(dev.log, ["HELLO"])                     # nothing sent after HELLO
        self.assertEqual(dev.reboots, [])

    def test_bin_needs_board_and_version(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        for kw in ({"board": None}, {"version": None}):
            with self.assertRaisesRegex(gemifi.GemifiError, "--board ID and --version"):
                self.run_flash(flash_args(self.write(esp_bin(100)), **kw), SimBackend(dev))
        with self.assertRaisesRegex(gemifi.GemifiError, "--version"):
            self.run_flash(flash_args(self.write(esp_bin(100)), version="1.2"), SimBackend(dev))
        self.assertEqual(dev.log, [])

    def test_version_mismatch_after_reboot(self):
        dev = SimDevice(ab=True, board_id=BOARD, fw_version=V_OLD)
        orig = dev.dispatch

        def no_switch(f):          # a device whose new slot doesn't boot: it stays on the old version
            if f.cmd == gp.REBOOT:
                dev.pending = None
            return orig(f)
        dev.dispatch = no_switch
        with self.assertRaisesRegex(gemifi.GemifiError, "expected 1.2.3"):
            self.run_flash(flash_args(self.write(esp_bin(100))), SimBackend(dev))

    def test_gfw_to_ab_device_refused(self):
        from test_upload import make_gfw
        dev = SimDevice(ab=True, board_id=1)
        with self.assertRaisesRegex(gemifi.GemifiError, "not a .gfw"):
            self.run_flash(flash_args(self.write(make_gfw(0x400), "a.gfw"), board=None, version=None),
                           SimBackend(dev))
        self.assertEqual(dev.log, ["HELLO"])

    def test_bin_to_bootloader_app_refused(self):
        dev = SimDevice(role=gp.ROLE_APP, board_id=BOARD)
        with self.assertRaisesRegex(gemifi.GemifiError, "needs a .gfw"):
            self.run_flash(flash_args(self.write(esp_bin(100))), SimBackend(dev))
        self.assertEqual(dev.reboots, [])

    def test_dry_run_bin(self):
        rc, out, _ = self.run_flash(flash_args(self.write(esp_bin(1001)), dry_run=True), None)
        self.assertEqual(rc, 0)
        self.assertIn("4 FW_DATA chunks", out)
        self.assertIn("0x0000 +256", out)
        self.assertIn("0x0300 +236", out)
        self.assertIn("padded to 1004", out)

    def test_not_esp_image_warns(self):
        _, _, err = self.run_flash(flash_args(self.write(b"\x00" * 64), dry_run=True), None)
        self.assertIn("warning", err)


class FakeSerialModule:
    """Stands in for pyserial: Serial() returns a SimSerial over the given device."""

    def __init__(self, dev):
        self.dev = dev
        self.opened = []
        self.ports = []

    def Serial(self):   # noqa: N802 (pyserial's name)
        mod = self

        class S(SimSerial):
            def __init__(self):
                object.__setattr__(self, "lines", [])
                super().__init__(mod.dev)

            def __setattr__(self, k, v):
                if k in ("dtr", "rts"):
                    self.lines.append((k, v))
                object.__setattr__(self, k, v)

            def open(self):
                mod.opened.append((self.port, self.baudrate))
                mod.ports.append(self)
        return S()


class TestSerial(unittest.TestCase):
    def test_stream_hello(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        port = gm.SerialPort(SimSerial(dev, piece=1))
        h = gemifi.Session(port, timeout=0.05).hello()
        self.assertEqual(h.board_id, BOARD)

    def test_stream_upload(self):
        img = esp_bin(1500)
        dev = SimDevice(ab=True, board_id=BOARD, max_chunk=256)
        port = gm.SerialPort(SimSerial(dev, piece=7))
        s = gemifi.Session(port, timeout=0.05)
        gemifi.upload_ab(s, img, BOARD, V_NEW)
        self.assertEqual(dev.pending[0], gp.pad4(img))

    def test_backend_opens_without_reset_lines(self):
        dev = SimDevice(ab=True, board_id=BOARD)
        mod = FakeSerialModule(dev)
        b = gm.SerialBackend("loop://", 115200, serial_module=mod)
        self.assertEqual(b.ports(), ["loop://"])
        self.assertEqual(gemifi.find_port(b, "app", "whatever"), "loop://")   # names don't matter on serial
        with b.open("loop://") as p:
            self.assertEqual(gemifi.Session(p, timeout=0.05).hello().role, gp.ROLE_APP)
        self.assertEqual(mod.opened, [("loop://", 115200)])
        self.assertEqual(mod.ports[0].lines, [("rts", False), ("dtr", False)])    # RTS (EN) first: no reset pulse

    def test_flash_over_serial(self):
        img = esp_bin(900)
        dev = SimDevice(ab=True, board_id=BOARD, fw_version=V_OLD)
        b = gm.SerialBackend("loop://", 115200, serial_module=FakeSerialModule(dev))
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "app.bin")
            with open(path, "wb") as f:
                f.write(img)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gemifi.cmd_flash(flash_args(path), lambda: b), 0)
        self.assertEqual(dev.fw_version, V_NEW)
        self.assertEqual(dev.reboots, [gp.REBOOT_APP])

    def test_missing_pyserial_is_clear(self):
        try:
            import serial  # noqa: F401
            self.skipTest("pyserial is installed")
        except ImportError:
            pass
        with self.assertRaisesRegex(gm.MidiError, "pyserial"):
            gm.SerialBackend("/dev/ttyUSB0")

    def test_cli_options(self):
        a = gemifi.build_parser().parse_args(["--serial", "/dev/ttyUSB0", "flash", "x.bin", "--board", "0x3F00",
                                              "--version", "1.2.3"])
        self.assertEqual((a.serial, a.baud, a.board, a.version), ("/dev/ttyUSB0", 115200, 0x3F00, "1.2.3"))


if __name__ == "__main__":
    unittest.main()
