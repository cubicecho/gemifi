"""Host upload logic against a simulated bootloader (tests/sim_device.py), including lost frames."""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import SimDevice, SimPort  # noqa: E402


def make_gfw(size):
    rnd = random.Random(size)
    raw = bytearray(rnd.randrange(256) for _ in range(size))
    raw[0:8] = (0x20000FF0).to_bytes(4, "little") + (0xE1).to_bytes(4, "little")
    raw[0xC0:0xE0] = bytes(32)
    return gi.make_image(bytes(raw), 1, 0x00010203)


class TestUpload(unittest.TestCase):
    def run_upload(self, img, dev):
        port = SimPort(dev)
        s = gemifi.Session(port, timeout=0.005, retries=5)
        hdr = gi.check_image(img)
        mc, n = gemifi.upload(s, img, hdr)
        return s, port, mc, n

    def assert_flashed(self, dev, img):
        self.assertEqual(bytes(dev.flash[:len(img)]), img)
        self.assertTrue(all(c <= 1 for c in dev.program_count), "a word was programmed twice")

    def test_sizes(self):
        for size in (0x100, 0x200, 0x204, 0x1000, 0x7F00, 0x8000):
            for mc in (4, 64, 256, 512):
                img = make_gfw(size)
                dev = SimDevice(max_chunk=mc)
                self.run_upload(img, dev)
                self.assert_flashed(dev, img)

    def test_lost_replies_resend_same_seq(self):
        img = make_gfw(0x1800)
        dev = SimDevice(drop_replies={2, 3, 7, 20})
        s, port, _, _ = self.run_upload(img, dev)
        self.assert_flashed(dev, img)
        self.assertGreaterEqual(s.resends, 4)
        # every resend is byte-identical to the request it repeats (same seq)
        frames = [gp.parse_frame(m) for m in port.sent]
        for a, b in zip(port.sent, port.sent[1:]):
            fa, fb = gp.parse_frame(a), gp.parse_frame(b)
            if fa.seq == fb.seq:
                self.assertEqual(a, b)
        self.assertEqual(len({f.seq for f in frames}) <= 128, True)

    def test_lost_requests(self):
        img = make_gfw(0x1000)
        dev = SimDevice(drop_requests={1, 5, 6})
        self.run_upload(img, dev)
        self.assert_flashed(dev, img)

    def test_busy_resends_same_seq(self):
        img = make_gfw(0x800)
        dev = SimDevice(busy_on={3, 4})
        s, port, _, _ = self.run_upload(img, dev)
        self.assert_flashed(dev, img)
        self.assertEqual(port.sent[2], port.sent[3])      # BUSY -> identical frame again
        self.assertEqual(port.sent[3], port.sent[4])

    def test_bad_max_chunk_aborts(self):
        dev = SimDevice(max_chunk=200)
        with self.assertRaisesRegex(gemifi.GemifiError, "divides 512"):
            self.run_upload(make_gfw(0x400), dev)
        self.assertEqual(dev.log[-1], "FW_ABORT")

    def test_timeouts(self):
        s = gemifi.Session(SimPort(SimDevice()))
        self.assertEqual((s.timeout, s.long_timeout, s.retries), (0.2, 2.0, 5))

    def test_reboot_timeout_is_success(self):
        dev = SimDevice(drop_replies=set(range(1, 100)))
        s = gemifi.Session(SimPort(dev), timeout=0.002)
        self.assertFalse(s.reboot(gp.REBOOT_BOOT))
        self.assertEqual(dev.n_req, gp.MAX_TRIES)
        self.assertEqual(dev.reboot_to, gp.REBOOT_BOOT)
        s = gemifi.Session(SimPort(SimDevice()), timeout=0.002)
        self.assertTrue(s.reboot(gp.REBOOT_APP))

    def test_bad_image_reported_and_aborted(self):
        img = bytearray(make_gfw(0x400))
        hdr = gi.check_image(bytes(img))
        img[0x300] ^= 0xFF      # corrupt after the header CRC was computed
        dev = SimDevice()
        s = gemifi.Session(SimPort(dev), timeout=0.005)
        with self.assertRaisesRegex(gemifi.GemifiError, "BAD_IMAGE"):
            gemifi.upload(s, bytes(img), hdr)
        self.assertEqual(dev.log[-1], "FW_ABORT")

    def test_wrong_board(self):
        img = make_gfw(0x400)
        dev = SimDevice(board_id=2)
        with self.assertRaisesRegex(gemifi.GemifiError, "BAD_BOARD"):
            self.run_upload(img, dev)

    def test_app_rejects_fw(self):
        img = make_gfw(0x400)
        with self.assertRaisesRegex(gemifi.GemifiError, "BAD_STATE"):
            self.run_upload(img, SimDevice(role=gp.ROLE_APP))

    def test_no_reply_gives_up(self):
        dev = SimDevice(drop_requests=set(range(1, 100)))
        s = gemifi.Session(SimPort(dev), timeout=0.002)
        with self.assertRaisesRegex(gemifi.GemifiError, "no reply"):
            s.hello()
        self.assertEqual(dev.n_req, 6)                    # 5 resends = 6 sends (sysex.md 1)

    def test_unsolicited_passed_through(self):
        dev = SimDevice()
        got = []
        port = SimPort(dev, unsolicited=[gp.build_frame(gp.LOG, 9, gp.build_log(2, 5, "hi"))])
        s = gemifi.Session(port, timeout=0.005, on_unsolicited=got.append)
        h = s.hello()
        self.assertEqual(h.role, gp.ROLE_BOOT)
        self.assertEqual(got[0].cmd, gp.LOG)


class TestPortNames(unittest.TestCase):
    def test_names(self):
        self.assertTrue(gemifi.is_boot_name("gemifi boot (Keystation Mini 32 MK3) [hw:2,0]"))
        self.assertTrue(gemifi.is_app_name("Keystation Mini 32 MK3 (gemifi) [hw:2,0]"))
        self.assertFalse(gemifi.is_app_name("gemifi boot (Keystation Mini 32 MK3)"))
        self.assertFalse(gemifi.is_app_name("Keystation Mini 32 MK3 MIDI 1"))


if __name__ == "__main__":
    unittest.main()
