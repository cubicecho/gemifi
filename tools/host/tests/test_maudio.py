"""Code 61 (board 0x0002): vendor update stream (maudio_update.py), the per-board .gfw layout, and the
gemifi.py flash / restore-stock flows against a simulated keyboard (tests/sim_maudio.py)."""
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
import maudio_update as mu  # noqa: E402
import mkimage  # noqa: E402
from sim_device import SimDevice  # noqa: E402
from sim_maudio import Code61Backend, SimCode61, _Port  # noqa: E402

FIRMWARE = os.path.join(HERE, "..", "..", "..", "research", "devices", "m-audio-code61", "firmware")
STOCK = os.path.join(FIRMWARE, "FirmwareFile_v1.2.1.hex.txt")             # Code 49 v1.2.1
STOCK_CODE61 = os.path.join(FIRMWARE, "FirmwareFile_code61_v1.2.2.hex.txt")  # Code 61 v1.2.2
FAST = {"ack_timeout": 0.05, "probe_timeout": 0.05, "gap": 0}


def raw_app(n, seed=1, hdr=True):
    rnd = random.Random(seed)
    raw = bytearray(rnd.randrange(256) for _ in range(n))
    struct.pack_into("<II", raw, 0, 0x20004FF0, 0x08002545)
    if hdr:
        raw[0x130:0x150] = bytes(32)
    return bytes(raw)


def gfw(n=20000, version="0.2.0", seed=1):
    return gi.make_image(raw_app(n, seed), 2, gi.parse_version(version))


def send(dev, records, **kw):
    port = _Port(dev.handle, "sim")
    s = mu.Sender(port, **{**FAST, **kw})
    assert s.probe() in (0x45, 0x1A)
    s.start()
    s.send_records(records)
    return s, port


class TestRecords(unittest.TestCase):
    def test_checksum(self):
        rec = bytes([0x00, 0x10]) + mu.SIGNATURE + bytes([0x09, 0x01, 0x88, 0, 0, 0]) + bytes(4) + b"\0\0"
        self.assertEqual(mu.checksum(rec), (-sum(rec[2:-2])) & 0xFFFF)
        r = mu.Record([mu.Cmd(mu.OP_ERASE, 0x08018800, 0)])
        self.assertEqual(r.pack().hex(), "00106a04da3b090188000000" + "00000000" + "ebfd")
        self.assertEqual(mu.parse_record(r.pack()), r)

    def test_image_checksum(self):
        self.assertEqual(mu.image_checksum(b""), (-(0xFF * mu.APP_MAX)) & 0xFFFF)
        self.assertEqual(mu.image_checksum(b"\x01\x02"), (-(3 + 0xFF * (mu.APP_MAX - 2))) & 0xFFFF)
        with self.assertRaises(mu.UpdateError):
            mu.image_checksum(bytes(mu.APP_MAX + 1))

    def test_layout_like_stock(self):
        img = gfw(20000)                           # 20 pages: 19 full, one partly FF
        recs = mu.build_stream(img)
        self.assertEqual([r.kind() for r in recs], ["data"] * 20 + ["erase"] * 79 + ["final"])
        self.assertEqual(recs[0].addr, 0x08002400)
        self.assertEqual(recs[20].addr, 0x08002400 + 20 * 0x400)
        self.assertEqual(recs[-2].addr, 0x0801AC00)
        self.assertEqual(recs[-1].addr, 0x0801B000)
        self.assertEqual([c.op for c in recs[-1].cmds], [0x09, 0x0A, 0x0B, 0x0D, 0x0E])
        self.assertEqual(len(recs[0].pack()), 0x41E)
        self.assertEqual(len(recs[20].pack()), 0x12)
        self.assertEqual(len(recs[-1].pack()), 0x42A)
        last = recs[-1].cmds[1].data
        self.assertEqual(last[:-2], b"\xff" * 1022)
        self.assertEqual(struct.unpack("<H", last[-2:])[0], mu.image_checksum(img))
        self.assertEqual(mu.check_stream(recs), [])
        self.assertEqual(mu.app_from_records(recs), img.ljust(20 * 1024, b"\xff"))

    def test_largest_image_reaches_the_checksum_page(self):
        img = gi.make_image(raw_app(mu.APP_MAX - 2), 2, 0)          # 102,396 bytes
        recs = mu.build_stream(img)
        self.assertEqual(sum(r.kind() == "data" for r in recs), 99)
        last = recs[-1].cmds[1].data
        self.assertEqual(last[:0x3FC], img[0x18C00:])
        self.assertEqual(last[0x3FC:0x3FE], b"\xff\xff")
        dev = SimCode61()
        send(dev, recs)
        self.assertTrue(dev.finished and dev.image_checksum_ok(), dev.errors)
        with self.assertRaises(mu.UpdateError):
            mu.build_stream(bytes(mu.APP_MAX + 1))
        with self.assertRaisesRegex(gi.ImageError, "app area"):
            gi.make_image(raw_app(mu.APP_MAX + 2), 2, 0)

    def test_corrupt_records_rejected(self):
        raw = bytearray(mu.build_stream(gfw())[0].pack())
        raw[100] ^= 1
        with self.assertRaisesRegex(mu.UpdateError, "checksum"):
            mu.parse_record(bytes(raw))
        raw = bytearray(mu.build_stream(gfw())[0].pack())
        raw[2] = 0x6B
        with self.assertRaisesRegex(mu.UpdateError, "signature"):
            mu.parse_record(bytes(raw))
        with self.assertRaisesRegex(mu.UpdateError, "record 0"):
            mu.parse_stream(bytes(raw))
        with self.assertRaisesRegex(mu.UpdateError, "length"):
            mu.split_records(b"\x04\x1c\x6a\x04")

    def test_check_stream_refuses_vendor_areas(self):
        recs = mu.build_stream(gfw())
        bad = recs[:-1] + [mu.Record([mu.Cmd(mu.OP_ERASE, 0x08000000, 0)])] + recs[-1:]
        self.assertIn("outside", " ".join(mu.check_stream(bad)))
        bad = recs[:-1] + [mu.Record([mu.Cmd(mu.OP_ERASE, mu.SETTINGS, 0)])] + recs[-1:]
        self.assertIn("outside", " ".join(mu.check_stream(bad)))
        wrong = mu.build_stream(gfw())
        data = bytearray(wrong[-1].cmds[1].data)
        data[-1] ^= 1
        wrong[-1].cmds[1].data = bytes(data)
        self.assertIn("image checksum", " ".join(mu.check_stream(wrong)))

    def test_syx(self):
        recs = mu.build_stream(gfw())
        msgs = mu.syx_messages(recs)
        self.assertEqual(msgs[0], mu.START)
        self.assertTrue(all(m[0] == 0xF0 and m[-1] == 0xF7 and len(m) <= 24 for m in msgs))
        self.assertTrue(all(c in b"0123456789ABCDEF" for m in msgs[1:] for c in m[1:-1]))
        self.assertEqual(bytes.fromhex(b"".join(m[1:-1] for m in msgs[1:]).decode()), mu.pack_stream(recs))


class TestStock(unittest.TestCase):
    """Needs the vendor file, which is gitignored (research/devices/m-audio-code61/firmware/).
    Code 49 v1.2.1 here; TestStockCode61 runs the same checks on the Code 61 v1.2.2 file."""
    path, checksum = STOCK, 0x2CF0

    def setUp(self):
        if not os.path.exists(self.path):
            self.skipTest("stock update file not present")
        self.blob = mu.load_hex(self.path)

    def test_round_trip(self):
        recs = mu.parse_stream(self.blob)
        self.assertEqual(mu.pack_stream(recs), self.blob)
        app = mu.app_from_records(recs)
        self.assertEqual(len(app), 91136)
        self.assertEqual(mu.pack_stream(mu.build_stream(app)), self.blob)     # byte for byte
        self.assertEqual(mu.image_checksum(app), self.checksum)
        self.assertEqual(mu.flash_checksum(mu.apply(recs)), (self.checksum, self.checksum))
        self.assertEqual(mu.check_stream(recs), [])

    def test_sim_takes_stock_stream(self):
        recs = mu.parse_stream(self.blob)
        dev = SimCode61()
        send(dev, recs)
        self.assertEqual(dev.errors, [])
        self.assertTrue(dev.finished)
        self.assertEqual(dev.app(91136), mu.app_from_records(recs))


class TestStockCode61(TestStock):
    path, checksum = STOCK_CODE61, 0xD4C1


class TestSender(unittest.TestCase):
    def test_non_stock_image(self):
        img = gfw(30000)
        dev = SimCode61(chatter=True)                         # F0 20 F7 before every ack is ignored
        s, port = send(dev, mu.build_stream(img))
        self.assertEqual(dev.errors, [])
        self.assertTrue(dev.finished and dev.image_checksum_ok())
        self.assertEqual(dev.app(len(img)), img)
        self.assertEqual(dev.mem[:0x2400], b"\xa5" * 0x2400)            # vendor bootloader untouched
        self.assertEqual(set(dev.mem[0x1B400:0x1FC00]), {0x5C})         # settings untouched
        self.assertEqual(dev.starts, 2)                                  # probe + start
        self.assertIn(0x20, s.seen)

    def test_1a_ack_and_no_restart(self):
        dev = SimCode61(ok=0x1A)
        s, _ = send(dev, mu.build_stream(gfw()), restart=False)
        self.assertTrue(dev.finished)
        self.assertEqual(dev.starts, 1)

    def test_nak_fails_unless_retried(self):
        recs = mu.build_stream(gfw())
        with self.assertRaisesRegex(mu.UpdateError, "NAK"):
            send(SimCode61(nak_records={5}), recs)
        dev = SimCode61(nak_records={5})
        s, _ = send(dev, recs, record_retries=1)
        self.assertTrue(dev.finished and dev.image_checksum_ok())
        self.assertEqual(s.resends, 1)

    def test_timeout(self):
        with self.assertRaisesRegex(mu.UpdateError, "no reply"):
            send(SimCode61(drop_ack_records={3}), mu.build_stream(gfw()))

    def test_sim_rejects_wrong_image_checksum(self):
        recs = mu.build_stream(gfw())
        data = bytearray(recs[-1].cmds[1].data)
        data[-2] ^= 1
        recs[-1].cmds[1].data = bytes(data)
        dev = SimCode61()
        with self.assertRaisesRegex(mu.UpdateError, "NAK"):
            send(dev, recs)
        self.assertIn("image checksum wrong", dev.errors)

    def test_sim_rejects_corrupt_chunk(self):
        recs = mu.build_stream(gfw())
        dev = SimCode61()
        port = _Port(dev.handle, "sim")
        s = mu.Sender(port, **FAST)
        s.probe()
        raw = bytearray(recs[0].pack())
        raw[50] ^= 0x10
        with self.assertRaisesRegex(mu.UpdateError, "NAK"):
            s.send_record(bytes(raw))
        self.assertIn("bad record checksum", dev.errors)


class TestInputs(unittest.TestCase):
    def test_gfw_and_bin(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.gfw")
            with open(p, "wb") as f:
                f.write(gfw())
            recs, desc, vendor = mu.load_input(p)
            self.assertIn("board 0x0002", desc)
            self.assertFalse(vendor)
            nuc = bytearray(raw_app(300, hdr=False))
            struct.pack_into("<II", nuc, 0, 0x20000FF0, 0xE1)
            nuc[0xC0:0xE0] = bytes(32)
            with open(p, "wb") as f:
                f.write(gi.make_image(bytes(nuc), 1, 0))
            with self.assertRaisesRegex(mu.UpdateError, "board_id"):
                mu.load_input(p)
            b = os.path.join(d, "a.bin")
            with open(b, "wb") as f:
                f.write(raw_app(5000, hdr=False))
            self.assertIn("raw app", mu.load_input(b)[1])
            with open(b, "wb") as f:
                f.write(struct.pack("<II", 0x20004FF0, 0x000000E1) + bytes(100))     # linked at 0
            with self.assertRaisesRegex(mu.UpdateError, "0x08002400"):
                mu.load_input(b)
            h = os.path.join(d, "a.hex.txt")
            with open(h, "w") as f:
                f.write(mu.pack_stream(mu.build_stream(gfw())).hex().upper())
            self.assertTrue(mu.load_input(h)[2])

    def test_cli_build_syx_info(self):
        with tempfile.TemporaryDirectory() as d:
            p, h, s = (os.path.join(d, n) for n in ("a.gfw", "a.hex.txt", "a.syx"))
            with open(p, "wb") as f:
                f.write(gfw())
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(out):
                self.assertEqual(mu.main(["build", p, h]), 0)
                self.assertEqual(mu.main(["syx", p, s]), 0)
                self.assertEqual(mu.main(["info", h]), 0)
                self.assertEqual(mu.main(["send", p, "--dry-run"]), 0)
            with open(s, "rb") as f:
                self.assertTrue(f.read().startswith(mu.START))


class TestLayout(unittest.TestCase):
    def test_header_at_0x130(self):
        img = gfw(1000, "1.2.3")
        self.assertEqual(img[0x130:0x134], b"GMFI")
        self.assertEqual(img[0xC0:0xC4], raw_app(1000)[0xC0:0xC4])           # NUC slot left alone
        h = gi.check_image(img)                                            # found without a board hint
        self.assertEqual((h.board_id, h.fw_version), (2, 0x01020003))
        self.assertEqual(gi.find_hdr_offset(img), 0x130)
        self.assertEqual(h.crc32, gi.image_crc(img, 0x130))
        with self.assertRaisesRegex(gi.ImageError, "board_id"):
            gi.check_image(img, 1)

    def test_reset_vector_uses_base(self):
        raw = bytearray(raw_app(1000))
        struct.pack_into("<I", raw, 4, 0xE1)                                 # linked at 0, not 0x08002400
        with self.assertRaisesRegex(gi.ImageError, "outside"):
            gi.make_image(bytes(raw), 2, 0)
        struct.pack_into("<I", raw, 4, 0x08002400 + 2000 + 1)
        with self.assertRaisesRegex(gi.ImageError, "outside"):
            gi.make_image(bytes(raw), 2, 0)

    def test_nuc_defaults_unchanged(self):
        raw = bytearray((i * 7 + 3) & 0xFF for i in range(254))
        struct.pack_into("<II", raw, 0, 0x20000FF0, 0xE1)
        raw[0xC0:0xE0] = bytes(32)
        a = gi.make_image(bytes(raw), 1, 0x01020003)
        b = gi.make_image(bytes(raw), 1, 0x01020003, hdr_offset=0xC0, base=0, max_size=32768)
        self.assertEqual(a, b)
        self.assertEqual(gi.BOARDS[1][0:2], ("M-Audio Keystation Mini 32 MK3", 32768))

    def test_mkimage_flags(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "app.bin"), os.path.join(d, "app.gfw")
            with open(src, "wb") as f:
                f.write(raw_app(4001))
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(out):
                rc = mkimage.main([src, dst, "--board", "0x0002", "--version", "0.1.0", "--hdr-offset", "0x130",
                                   "--base", "0x08002400", "--max-size", "102398"])
            self.assertEqual(rc, 0, out.getvalue())
            self.assertNotIn("warning", out.getvalue())
            with open(dst, "rb") as f:
                img = f.read()
            self.assertEqual(img, gi.make_image(raw_app(4001), 2, gi.parse_version("0.1.0")))
            self.assertEqual(len(img), 4004)
            with redirect_stdout(out), redirect_stderr(out):
                self.assertEqual(mkimage.main(["--check", dst, "--board", "2"]), 0)
                rc = mkimage.main([src, dst, "--board", "2", "--version", "0.1.0", "--max-size", "4000"])
            self.assertEqual(rc, 1)


class Code61GemifiBoot:
    """A Code 61 with the gemifi bootloader: the app (HELLO caps bit 7) on 'gemifi (Code 61)'; REBOOT(1) brings
    up 'gemifi boot (Code 61)', which takes the gemifi upload (sysex.md 4.2) into the Code 61's app area;
    REBOOT(0) starts the image in it."""
    label = "sim-code61-gemifi-boot"
    APP, BOOT = "gemifi (Code 61) 1", "gemifi boot (Code 61) 1"

    def __init__(self, state="app"):
        self.state = state
        self.app = SimDevice(role=gp.ROLE_APP, board_id=2, fw_version=0x00010000, extra_caps=gp.CAP_GEMIFI_BOOT)
        self.boot = SimDevice(role=gp.ROLE_BOOT, board_id=2, flash_size=gi.CODE61_MAX & ~3)

    def ports(self):
        if self.state == "app" and gp.REBOOT_BOOT in self.app.reboots:
            self.state, self.boot.reboots = "boot", []
        elif self.state == "boot" and gp.REBOOT_APP in self.boot.reboots:
            ver = gi.parse_header(bytes(self.boot.flash[:0x200]), 0x130).fw_version
            self.app = SimDevice(role=gp.ROLE_APP, board_id=2, fw_version=ver, extra_caps=gp.CAP_GEMIFI_BOOT)
            self.state = "app"
        return [self.APP if self.state == "app" else self.BOOT]

    def open(self, name):
        dev = self.app if name == self.APP else self.boot
        return _Port(lambda m: [r] if (r := dev.handle(m)) is not None else [], name)


class TestGemifiFlow(unittest.TestCase):
    def run_cli(self, backend, *args):
        a = gemifi.build_parser().parse_args(list(args) + ["--ack-timeout", "0.2", "--probe-timeout", "0.2",
                                                           "--gap", "0", "-q"])
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            fn = gemifi.cmd_restore_stock if a.cmd == "restore-stock" else gemifi.cmd_flash
            try:
                rc = fn(a, lambda: backend)
            except gemifi.GemifiError as e:
                print(f"ERROR: {e}")
                rc = 1
        return rc, out.getvalue()

    def test_flash_from_gemifi_app(self):
        be = Code61Backend("gemifi")
        img = gfw(12000, "0.3.1", seed=4)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "app.gfw")
            with open(p, "wb") as f:
                f.write(img)
            rc, out = self.run_cli(be, "flash", p, "--wait", "10", "--after-wait", "5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(be.events, ["REBOOT(1)", "bootloader", "finished"])
        self.assertIn("power-cycle", out)
        self.assertEqual(be.boot.app(len(img)), img)
        self.assertTrue(be.boot.finished and be.boot.image_checksum_ok())
        self.assertIn("0.3.1", out)

    def test_flash_from_stock_app(self):
        be = Code61Backend("stock")
        img = gfw(9000, "0.1.0", seed=5)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "app.gfw")
            with open(p, "wb") as f:
                f.write(img)
            rc, out = self.run_cli(be, "flash", p, "--wait", "10", "--after-wait", "5")
        self.assertEqual(rc, 0, out)
        self.assertIn("F0 15 F7", out)
        self.assertEqual(be.events, ["F0 15 F7", "bootloader", "finished"])
        self.assertEqual(be.boot.app(len(img)), img)

    def test_restore_stock(self):
        be = Code61Backend("gemifi")
        app = raw_app(15000, seed=9, hdr=False)          # stands in for the vendor app
        with tempfile.TemporaryDirectory() as d:
            h = os.path.join(d, "FirmwareFile.hex.txt")
            with open(h, "w") as f:
                f.write(mu.pack_stream(mu.build_stream(app)).hex().upper())
            rc, out = self.run_cli(be, "restore-stock", h, "--wait", "10")
        self.assertEqual(rc, 0, out)
        self.assertEqual(be.boot.app(len(app)), app)
        be.ports()
        self.assertEqual(be.events, ["REBOOT(1)", "bootloader", "finished"])
        self.assertEqual(be.state, "stock")

    def write_gfw(self, d, img):
        p = os.path.join(d, "app.gfw")
        with open(p, "wb") as f:
            f.write(img)
        return p

    def test_flash_through_gemifi_bootloader(self):
        """An app with caps bit 7: REBOOT(1), then the gemifi upload, even past the NUC's 32 KB."""
        be = Code61GemifiBoot()
        img = gfw(61000, "0.2.89", seed=6)
        with tempfile.TemporaryDirectory() as d:
            rc, out = self.run_cli(be, "flash", self.write_gfw(d, img), "--wait", "2", "--no-backup")
        self.assertEqual(rc, 0, out)
        self.assertNotIn("power-cycle", out)
        self.assertEqual(bytes(be.boot.flash[:len(img)]), img)
        self.assertEqual(be.state, "app")
        self.assertIn("0.2.89", out)

    def test_flash_from_gemifi_bootloader(self):
        be = Code61GemifiBoot(state="boot")
        img = gfw(40000, "0.2.90", seed=7)
        with tempfile.TemporaryDirectory() as d:
            rc, out = self.run_cli(be, "flash", self.write_gfw(d, img), "--wait", "2")
        self.assertEqual(rc, 0, out)
        self.assertEqual(bytes(be.boot.flash[:len(img)]), img)

    def test_via_vendor_overrides(self):
        be = Code61Backend("gemifi")
        img = gfw(9000, "0.3.2", seed=8)
        with tempfile.TemporaryDirectory() as d:
            rc, out = self.run_cli(be, "flash", self.write_gfw(d, img), "--via", "vendor", "--wait", "10",
                                   "--after-wait", "5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(be.events, ["REBOOT(1)", "bootloader", "finished"])

    def test_dry_run_and_wrong_board(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "app.gfw")
            with open(p, "wb") as f:
                f.write(gfw())
            rc, out = self.run_cli(None, "flash", p, "--dry-run")
            self.assertEqual(rc, 0, out)
            self.assertIn("FW_DATA chunks", out)                 # the gemifi bootloader's plan
            rc, out = self.run_cli(None, "flash", p, "--dry-run", "--via", "vendor")
            self.assertEqual(rc, 0, out)
            self.assertIn("Code 61", out)
            be = Code61Backend("gemifi")
            be.gm.board_id = 1
            rc, out = self.run_cli(be, "flash", p, "--wait", "2")
            self.assertEqual(rc, 1)
            self.assertIn("not the Code 61", out)
            self.assertEqual(be.gm.reboots, [])


if __name__ == "__main__":
    unittest.main()
