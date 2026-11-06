"""Physical unlock (sysex.md 4.7) and diagnostics (4.8): the codec, and the host's LOCKED -> prompt -> UNLOCKED ->
send again flow against tests/sim_device.py."""
import contextlib
import io
import os
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import SimBackend, SimDevice, SimPort  # noqa: E402
import make_test_vectors  # noqa: E402


def small_schema():
    return make_test_vectors.example_schema()


COMBO = gp.UnlockInfo(0, 0, 2000, ["Pitch down", "Pitch up"])


class TestCodec(unittest.TestCase):
    def test_constants(self):
        self.assertEqual((gp.LOCKED, gp.STATUS[12], gp.UNLOCK, gp.UNLOCKED, gp.CAP_UNLOCK), (12, "LOCKED", 0x16, 0x66,
                                                                                             1024))
        self.assertEqual((gp.CMD_NAMES[0x16], gp.CMD_NAMES[0x66]), ("UNLOCK", "UNLOCKED"))
        self.assertIn((gp.CAP_UNLOCK, "unlock"), gp.CAP_NAMES)
        self.assertEqual(gp.DIAG_PARAMS[65], "diag.reset_reason")
        self.assertEqual(gp.DIAG_PARAMS[71], "diag.uptime")
        self.assertEqual(sorted(gp.DIAG_PARAMS), list(range(65, 72)))

    def test_needs_unlock(self):
        self.assertTrue(gp.needs_unlock(gp.FW_BEGIN))
        self.assertTrue(gp.needs_unlock(gp.FACTORY_RESET))
        self.assertTrue(gp.needs_unlock(gp.REBOOT, gp.req_reboot(gp.REBOOT_BOOT)))
        self.assertFalse(gp.needs_unlock(gp.REBOOT, gp.req_reboot(gp.REBOOT_APP)))
        self.assertFalse(gp.needs_unlock(gp.COMMIT))

    def test_request(self):
        self.assertEqual(gp.req_unlock(False), b"\x00")
        self.assertEqual(gp.req_unlock(True), b"\x01")

    def test_reply_round_trip(self):
        info = gp.UnlockInfo(0, 30, 2000, ["Pitch down", "Pitch up"])
        p = gp.build_unlock_reply(info)
        self.assertEqual(p, bytes([0, 0, 30, 0x50, 0x0F, 2, 10]) + b"Pitch down" + bytes([8]) + b"Pitch up")
        self.assertEqual(gp.parse_unlock_reply(p), info)
        self.assertFalse(info.unlocked)
        f = gp.parse_frame(gp.build_frame(gp.UNLOCK | gp.REPLY, 3, p))
        self.assertTrue(gp.reply_matches(f, gp.UNLOCK, 3))
        self.assertTrue(gp.reply_matches(gp.parse_frame(gp.build_frame(gp.UNLOCK | gp.REPLY, 3, b"\x02")),
                                         gp.UNLOCK, 3))      # BAD_CMD: an app without unlock

    def test_reply_errors(self):
        good = gp.build_unlock_reply(gp.UnlockInfo(1, 0, 0, ["A"]))
        for bad in (good[:-1], good + b"x", bytes([0, 2, 0, 0, 0, 0]), bytes([0, 0, 0, 0, 0, 9]) + b"\x00" * 9,
                    b"\x00\x00", bytes([gp.BAD_CMD])):
            with self.assertRaises(gp.ProtocolError, msg=bad.hex()):
                gp.parse_unlock_reply(bad)

    def test_unlocked(self):
        self.assertEqual(gp.parse_unlocked(gp.build_unlocked(1)), 1)
        self.assertEqual(gp.parse_unlocked(b"\x00"), 0)
        for bad in (b"", b"\x02", b"\x01\x00"):
            with self.assertRaises(gp.ProtocolError):
                gp.parse_unlocked(bad)

    def test_prompt(self):
        self.assertEqual(gp.unlock_prompt(["Pitch down", "Pitch up"], 2000),
                         "Hold Pitch down + Pitch up on the device for 2 s")
        self.assertEqual(gp.unlock_prompt(["Shift"], 1500), "Hold Shift on the device for 1.5 s")
        self.assertEqual(gp.unlock_prompt(["Shift", "Play"], 0), "Press Shift + Play on the device")
        self.assertEqual(gp.UnlockInfo(0, 30, 0, ["X"]).prompt, "Press X on the device")

    def test_format_diag(self):
        self.assertEqual(gp.format_diag(gp.P_DIAG_RESET_REASON, 4), "4 (watchdog)")
        self.assertEqual(gp.format_diag(gp.P_DIAG_RESET_REASON, 99), "99")
        self.assertEqual(gp.format_diag(gp.P_DIAG_SETTINGS, 0), "0 (loaded)")
        self.assertEqual(gp.format_diag(gp.P_DIAG_SCAN_MAX, 125), "125 us")
        self.assertEqual(gp.format_diag(gp.P_DIAG_UPTIME, 60), "60 s")


def locked_dev(user="unlock", **kw):
    kw.setdefault("schema", small_schema())
    return SimDevice(role=gp.ROLE_APP, unlock=COMBO, user=user, **kw)


def session(dev, wait=0.05):
    prompts = []
    s = gemifi.Session(SimPort(dev), timeout=0.005, on_prompt=prompts.append, unlock_wait=wait)
    return s, prompts


class TestFlow(unittest.TestCase):
    def test_factory_reset_waits_and_resends(self):
        dev = locked_dev()
        s, prompts = session(dev)
        s.factory_reset()
        self.assertEqual(dev.log, ["FACTORY_RESET", "UNLOCK", "FACTORY_RESET"])
        self.assertEqual(dev.locked_replies, [gp.FACTORY_RESET])
        self.assertFalse(dev.locked)
        self.assertEqual(prompts[0], "FACTORY_RESET: the device is locked. Hold Pitch down + Pitch up on the device "
                                     "for 2 s (waiting up to 0 s)")
        self.assertEqual(prompts[1], "unlocked; sending FACTORY_RESET again")
        sent = [gp.parse_frame(m) for m in s.port.sent]
        self.assertEqual(sent[1].payload, gp.req_unlock(False))     # open 0: the LOCKED reply opened the window
        self.assertEqual(dev.unlock_opens, [0])
        self.assertEqual(s.resends, 0)                               # LOCKED is not resent like BUSY
        s.factory_reset()                                            # unlocked now: no second prompt
        self.assertEqual(len(prompts), 2)

    def test_reboot_boot_and_not_app(self):
        dev = locked_dev()
        s, prompts = session(dev)
        self.assertTrue(s.reboot(gp.REBOOT_APP))
        self.assertEqual(prompts, [])
        self.assertTrue(s.reboot(gp.REBOOT_BOOT))
        self.assertEqual(dev.reboots, [gp.REBOOT_APP, gp.REBOOT_BOOT])
        self.assertEqual(dev.locked_replies, [gp.REBOOT])
        self.assertEqual(len(prompts), 2)

    def test_window_closes(self):
        dev = locked_dev(user="close")
        s, prompts = session(dev)
        with self.assertRaisesRegex(gemifi.NotUnlocked, "device not unlocked: the unlock window closed") as cm:
            s.factory_reset()
        self.assertEqual(cm.exception.status, gp.LOCKED)
        self.assertEqual(dev.log, ["FACTORY_RESET", "UNLOCK"])      # not sent again
        self.assertEqual(len(prompts), 1)

    def test_no_unlocked_times_out(self):
        dev = locked_dev(user=None)
        s, prompts = session(dev, wait=0.1)
        t0 = time.monotonic()
        with self.assertRaisesRegex(gemifi.NotUnlocked, "device not unlocked: no UNLOCKED within"):
            s.reboot(gp.REBOOT_BOOT)      # not taken for a lost REBOOT reply
        self.assertGreaterEqual(time.monotonic() - t0, 0.1)
        self.assertEqual(dev.reboots, [])

    def test_default_wait_is_window_plus_5(self):
        dev = locked_dev(user=None)
        prompts = []
        real = time.monotonic
        clock = [0.0]

        def fake_monotonic():         # real time until the prompt, then 10 s per call so the wait ends at once
            if not prompts:
                return real()
            clock[0] += 10
            return real() + clock[0]
        s = gemifi.Session(SimPort(dev), timeout=0.005, on_prompt=prompts.append)
        gemifi.time.monotonic = fake_monotonic
        try:
            with self.assertRaisesRegex(gemifi.NotUnlocked, r"within 35 s"):
                s.factory_reset()
        finally:
            gemifi.time.monotonic = real
        self.assertIn("(waiting up to 35 s)", prompts[0])

    def test_unsolicited_while_waiting(self):
        dev = locked_dev()
        seen = []
        s = gemifi.Session(SimPort(dev), timeout=0.005, on_prompt=lambda m: None, unlock_wait=0.05,
                           on_unsolicited=seen.append)
        dev.later.append(gp.build_frame(gp.LOG, 1, gp.build_log(2, 10, "hold the combo")))
        s.factory_reset()
        self.assertEqual([f.cmd for f in seen], [gp.LOG])           # UNLOCKED handled, LOG passed on

    def test_stale_relock_message_ignored(self):
        dev = locked_dev()
        s, prompts = session(dev)
        s.factory_reset()
        dev.relock()                      # 5 minutes idle: UNLOCKED 0, delivered with the next reply
        s.factory_reset()
        self.assertEqual(dev.log[-3:], ["FACTORY_RESET", "UNLOCK", "FACTORY_RESET"])
        self.assertEqual(len(prompts), 4)

    def test_device_without_unlock_unchanged(self):
        dev = SimDevice(role=gp.ROLE_APP, schema=small_schema())
        s, prompts = session(dev)
        s.factory_reset()
        self.assertTrue(s.reboot(gp.REBOOT_BOOT))
        self.assertEqual(dev.log, ["FACTORY_RESET", "REBOOT"])
        self.assertEqual(prompts, [])

    def test_locked_but_no_unlock_command(self):
        dev = SimDevice(role=gp.ROLE_APP, schema=small_schema())
        orig = dev.dispatch
        dev.dispatch = lambda f: bytes([gp.LOCKED]) if f.cmd == gp.FACTORY_RESET else orig(f)
        s, _ = session(dev)
        with self.assertRaisesRegex(gemifi.NotUnlocked, "UNLOCK failed"):
            s.factory_reset()


def cli(dev, *argv):
    a = gemifi.build_parser().parse_args(list(argv))
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = gemifi.COMMANDS[a.cmd](a, SimBackend(dev, "gemifi test"))
    return rc, out.getvalue(), err.getvalue()


class TestCli(unittest.TestCase):
    def test_unlock_command(self):
        dev = locked_dev()
        rc, out, err = cli(dev, "unlock")
        self.assertEqual(rc, 0)
        self.assertIn("Hold Pitch down + Pitch up on the device for 2 s", err)
        self.assertIn("gemifi test: unlocked", out)
        self.assertEqual(dev.log, ["HELLO", "UNLOCK"])
        self.assertEqual(dev.unlock_opens, [1])                     # open 1: the window opens without a LOCKED
        rc, out, _ = cli(dev, "unlock")
        self.assertIn("already unlocked", out)

    def test_unlock_command_without_cap(self):
        rc, out, _ = cli(SimDevice(role=gp.ROLE_APP, schema=small_schema()), "unlock")
        self.assertEqual(rc, 0)
        self.assertIn("nothing to do", out)

    def test_factory_reset_command(self):
        dev = locked_dev()
        rc, out, err = cli(dev, "factory-reset", "--yes")
        self.assertEqual(rc, 0)
        self.assertIn("the device is locked", err)
        self.assertIn("factory reset done", out)

    def test_hello_caps_and_diag(self):
        dev = locked_dev()
        rc, out, _ = cli(dev, "hello", "--diag")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"caps +0x040e config log event unlock")
        self.assertIn("diagnostics (not reported", out)            # small_schema has no diag params


class TestDiag(unittest.TestCase):
    def test_read_diag(self):
        mp = gp.make_param
        sch = gp.Schema(gp.SCHEMA_VERSION, ["Debug"], [
            mp(65, "diag.reset_reason", gp.T_ENUM, 0, 0, None, 0, "Reset reason", flags=gp.F_READ_ONLY | gp.F_VOLATILE,
               options=["Unknown", "Power-on", "Pin", "Software", "Watchdog", "Brown-out", "Crash", "Low-power"]),
            mp(68, "diag.scan_max", gp.T_INT, 0, 0, 100000, 0, "Longest scan", unit="us",
               flags=gp.F_READ_ONLY | gp.F_VOLATILE),
        ])
        dev = SimDevice(role=gp.ROLE_APP, schema=sch)
        dev.values[(65, 0)] = 4
        dev.values[(68, 0)] = 117
        s, _ = session(dev)
        self.assertEqual(gemifi.read_diag(s), [(65, "diag.reset_reason", 4), (68, "diag.scan_max", 117)])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            gemifi.print_diag(gemifi.read_diag(s))
        self.assertIn("diag.reset_reason  4 (watchdog)", out.getvalue())
        self.assertIn("diag.scan_max      117 us", out.getvalue())


class TestFlash(unittest.TestCase):
    def test_ab_flash_unlocks_first(self):
        from test_upload_ab import BOARD, V_NEW, V_OLD, esp_bin, flash_args
        img = esp_bin(1001)
        dev = SimDevice(ab=True, board_id=BOARD, fw_version=V_OLD, unlock=COMBO)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "app.bin")
            with open(path, "wb") as f:
                f.write(img)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = gemifi.cmd_flash(flash_args(path), lambda: SimBackend(dev))
        self.assertEqual(rc, 0)
        self.assertEqual(dev.locked_replies, [gp.FW_BEGIN])
        self.assertEqual(dev.log[dev.log.index("UNLOCK") + 1], "FW_BEGIN")
        self.assertEqual(dev.fw_version, V_NEW)
        self.assertIn("Hold Pitch down + Pitch up", err.getvalue())

    def test_bootloader_flash_unlocks_reboot(self):
        """A NUC-style app: REBOOT(1) is the locked request; the bootloader itself has no lock."""
        dev = SimDevice(role=gp.ROLE_APP, unlock=COMBO, schema=small_schema())
        s, prompts = session(dev)
        s.reboot(gp.REBOOT_BOOT)
        self.assertEqual(dev.log, ["REBOOT", "UNLOCK", "REBOOT"])
        self.assertEqual(dev.reboots, [gp.REBOOT_BOOT])


if __name__ == "__main__":
    unittest.main()
