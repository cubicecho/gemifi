"""Offline tests for the SWD shell scripts: argument handling, probe selection, verify-only, resume.

A fake openocd (tests/stubs/openocd) is put first on PATH, with fake pgrep and lsusb, so nothing
here talks to a probe or the keyboard. BP_PORT points at an ordinary temp file.

Run: cd tools/swd/nuvoton-nuc1xx && python3 -m unittest   (or pytest tests/)
"""
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

SWD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUBS = os.path.join(SWD, "tests", "stubs")
LDROM = 0x00100000


def boot_image(n):
    """A plausible LDROM image: SP in SRAM, odd reset vector inside it."""
    data = bytearray((i * 13 + 5) & 0xFF for i in range(n))
    data[0:4] = (0x20000800).to_bytes(4, "little")
    data[4:8] = (0x41).to_bytes(4, "little")
    return bytes(data)


@unittest.skipUnless(shutil.which("bash") and shutil.which("timeout"), "needs bash and coreutils")
class ScriptTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.d = self.td.name
        self.calls = os.path.join(self.d, "calls.jsonl")
        self.port = os.path.join(self.d, "ttyFAKE0")
        open(self.port, "w").close()
        self.env = {k: v for k, v in os.environ.items() if k not in ("PROBE", "DRY_RUN", "ASSUME_YES", "BP_PORT")}
        self.env.update(PATH=STUBS + os.pathsep + os.environ["PATH"], LOG_DIR=os.path.join(self.d, "logs"),
                        BP_PORT=self.port, STUB_CALLS=self.calls, STUB_BASE=hex(LDROM),
                        STUB_FAIL_FLAG=os.path.join(self.d, "failed-once"))
        self.img = self.file("boot.bin", boot_image(2048))
        self.env["STUB_IMAGE"] = self.img

    def tearDown(self):
        self.td.cleanup()

    def file(self, name, data):
        p = os.path.join(self.d, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def run_sh(self, script, *args, **env):
        e = dict(self.env, **env)
        r = subprocess.run([os.path.join(SWD, script), *args], env=e, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, timeout=120)
        r.out = r.stdout + r.stderr
        return r

    def openocd_calls(self):
        if not os.path.exists(self.calls):
            return []
        with open(self.calls) as f:
            return [json.loads(x) for x in f]

    def assert_no_writes(self):
        for argv in self.openocd_calls():
            joined = " ".join(argv)
            for bad in ("gemifi_write", "erase", "write_image", "program"):
                self.assertNotIn(bad, joined, f"verify-only ran a writing command: {argv}")

    def isp_starts(self):
        out = []
        for argv in self.openocd_calls():
            for a in argv:
                if a.startswith("set START "):
                    out.append(int(a.split(";")[0].split()[2]))
        return out

    def resume_command(self, output):
        lines = output.splitlines()
        i = next(i for i, x in enumerate(lines) if x.startswith("To resume the verify"))
        return lines[i + 1].strip()


class TestLdromVerifyOnly(ScriptTest):
    def test_full_verify_ok_and_read_only(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img)
        self.assertEqual(r.returncode, 0, r.out)
        self.assertIn("PLAN: verify LDROM (read-only)", r.out)
        self.assertIn("VERIFY OK: all 512 words", r.out)
        self.assertIn("LDROM matches boot.bin", r.out)
        self.assertNotIn("Type 'yes'", r.out)             # read-only: no prompt, and stdin isn't a tty
        self.assert_no_writes()
        self.assertEqual(self.isp_starts(), [LDROM, LDROM + 0x400])   # 256 words per session

    def test_no_backup_needed(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--verify-only")
        self.assertEqual(r.returncode, 0, r.out)

    def test_mismatch_lists_addresses(self):
        other = bytearray(boot_image(2048))
        other[0x123] ^= 0xFF
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img, STUB_IMAGE=self.file("chip.bin", bytes(other)))
        self.assertEqual(r.returncode, 1, r.out)
        self.assertIn("VERIFY FAILED: 1 of 512 words differ", r.out)
        self.assertIn("0x00100120  +0x0120", r.out)
        self.assertIn("Resuming won't change that", r.out)
        self.assert_no_writes()

    def test_interrupted_verify_resumes_only_missing_words(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img, STUB_FAIL_AT=hex(LDROM + 0x500))
        self.assertEqual(r.returncode, 2, r.out)
        self.assertIn("Sequence 5 not supported", r.out)
        self.assertIn("never erases or writes", r.out)
        self.assertIn("If the Bus Pirate is stuck, replug it", r.out)
        cmd = self.resume_command(r.out)
        self.assertIn("flash-ldrom.sh --verify-only", cmd)
        self.assertIn("--resume", cmd)
        self.assertIn("--probe buspirate", cmd)
        self.assertIn("BP_PORT=", cmd)                      # non-default port is carried over
        before = len(self.openocd_calls())

        r2 = subprocess.run(["bash", "-c", cmd], env=self.env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=120)
        out2 = r2.stdout + r2.stderr
        self.assertEqual(r2.returncode, 0, out2)
        self.assertIn("resuming the verify in", out2)
        self.assertIn("VERIFY OK: all 512 words", out2)
        self.assertEqual(self.isp_starts()[before:], [LDROM + 0x500])  # only the words not read yet
        self.assert_no_writes()

    def test_partial_line_from_killed_session_is_reread(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img,
                        STUB_FAIL_AT=hex(LDROM + 0x100), STUB_FAIL_PARTIAL="1")
        self.assertEqual(r.returncode, 2, r.out)
        r2 = subprocess.run(["bash", "-c", self.resume_command(r.out)], env=self.env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=120)
        self.assertEqual(r2.returncode, 0, r2.stdout + r2.stderr)
        self.assertIn("VERIFY OK", r2.stdout + r2.stderr)

    def test_resume_refuses_folder_of_another_file(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img, STUB_FAIL_AT=hex(LDROM + 0x10))
        vdir = shlex.split(self.resume_command(r.out))[-3]
        other = self.file("other.bin", boot_image(1024))
        r2 = self.run_sh("flash-ldrom.sh", "--verify-only", other, "--resume", vdir)
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("is a verify of something else", r2.out)

    def test_resume_refuses_foreign_folder(self):
        d = os.path.join(self.d, "random")
        os.makedirs(d)
        self.file("random/x", b"")
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img, "--resume", d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has no verify.txt", r.out)
        self.assertEqual(self.openocd_calls(), [])

    def test_resume_needs_verify_only(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--resume", self.d, "--i-have-a-backup", self.d, "--yes")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--resume only works with --verify-only", r.out)
        self.assertEqual(self.openocd_calls(), [])

    def test_too_big(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.file("big.bin", bytes(4100)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("LDROM is 4096", r.out)

    def test_verify_records_file_identity(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img)
        vdir = next(x.split(": ", 1)[1] for x in r.out.splitlines() if "read-back folder:" in x)
        with open(os.path.join(vdir, "verify.txt")) as f:
            meta = f.read()
        with open(self.img, "rb") as f:
            self.assertIn("sha256=" + hashlib.sha256(f.read()).hexdigest(), meta)
        self.assertIn("len=2048", meta)


class TestLdromWrite(ScriptTest):
    def backup(self):
        b = os.path.join(self.d, "backup")
        os.makedirs(b)
        data = boot_image(4096)
        self.file("backup/ldrom.bin", data)
        with open(os.path.join(b, "SHA256SUMS"), "w") as f:
            f.write(f"{hashlib.sha256(data).hexdigest()}  ldrom.bin\n")
        return b

    def test_write_needs_yes_without_tty(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--i-have-a-backup", self.backup())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refusing to write", r.out)
        self.assertEqual(self.openocd_calls(), [])

    def test_write_needs_backup(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--yes")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--i-have-a-backup", r.out)
        self.assertEqual(self.openocd_calls(), [])

    def test_write_then_verify(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--i-have-a-backup", self.backup(), "--yes")
        self.assertEqual(r.returncode, 0, r.out)
        self.assertIn("LDROM written and verified", r.out)
        self.assertIn("gemifi_write_isp_region", " ".join(self.openocd_calls()[0]))

    def test_write_ok_verify_interrupted_points_to_verify_only(self):
        r = self.run_sh("flash-ldrom.sh", self.img, "--i-have-a-backup", self.backup(), "--yes",
                        STUB_FAIL_AT=hex(LDROM + 0x600))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("don't rerun the write", r.out)
        cmd = self.resume_command(r.out)
        writes = len(self.openocd_calls())
        r2 = subprocess.run(["bash", "-c", cmd], env=self.env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=120)
        self.assertEqual(r2.returncode, 0, r2.stdout + r2.stderr)
        for argv in self.openocd_calls()[writes:]:
            self.assertNotIn("gemifi_write", " ".join(argv))


class TestProbeSelection(ScriptTest):
    def cfgs(self, argv):
        return [os.path.basename(argv[i + 1]) for i, a in enumerate(argv) if a == "-f"]

    def test_default_is_buspirate(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.file("s.bin", boot_image(16)))
        self.assertEqual(r.returncode, 0, r.out)
        argv = self.openocd_calls()[0]
        self.assertIn("openocd-buspirate.cfg", self.cfgs(argv))
        self.assertIn(f"set BP_PORT {{{self.port}}}", argv)

    def test_probe_flag_cmsis_dap(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.file("s.bin", boot_image(16)), "--probe", "cmsis-dap")
        self.assertEqual(r.returncode, 0, r.out)
        argv = self.openocd_calls()[0]
        self.assertIn("openocd-cmsis-dap.cfg", self.cfgs(argv))
        self.assertNotIn("openocd-buspirate.cfg", self.cfgs(argv))
        self.assertFalse(any(a.startswith("set BP_PORT") for a in argv))
        self.assertIn("Probe:  cmsis-dap", r.out)

    def test_probe_env_and_flag_overrides_env(self):
        r = self.run_sh("check.sh", "--probe=buspirate", PROBE="cmsis-dap")
        self.assertEqual(r.returncode, 0, r.out)
        self.assertIn("openocd-buspirate.cfg", self.cfgs(self.openocd_calls()[-1]))
        r = self.run_sh("check.sh", PROBE="pico")
        self.assertIn("openocd-cmsis-dap.cfg", self.cfgs(self.openocd_calls()[-1]))

    def test_resume_command_keeps_probe(self):
        r = self.run_sh("flash-ldrom.sh", "--verify-only", self.img, "--probe", "cmsis-dap",
                        STUB_FAIL_AT=hex(LDROM + 0x10))
        cmd = self.resume_command(r.out)
        self.assertIn("--probe cmsis-dap", cmd)
        self.assertNotIn("BP_PORT=", cmd)
        self.assertNotIn("Bus Pirate is stuck", r.out)

    def test_unknown_probe(self):
        for script in ("check.sh", "flash-ldrom.sh", "dump.sh"):
            r = self.run_sh(script, "--probe", "jlink")
            self.assertEqual(r.returncode, 2, script + r.out)
            self.assertIn("unknown probe 'jlink'", r.out)
        r = self.run_sh("check.sh", PROBE="stlink")
        self.assertEqual(r.returncode, 2)

    def test_probe_needs_value(self):
        r = self.run_sh("check.sh", "--probe")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--probe needs a value", r.out)

    def test_every_script_takes_probe_in_dry_run(self):
        runs = [
            ("check.sh",),
            ("read-config.sh",),
            ("dump.sh", os.path.join(self.d, "newdump")),
            ("isp-dump.sh", os.path.join(self.d, "d"), "ldrom", "0x00100000", "0x10", "1"),
            ("flash-aprom.sh", "--verify-only", self.img),
            ("flash-ldrom.sh", "--verify-only", self.img),
        ]
        for argv in runs:
            r = self.run_sh(*argv, "--dry-run", "--probe", "cmsis-dap")
            self.assertEqual(r.returncode, 0, f"{argv}: {r.out}")
            self.assertIn("openocd-cmsis-dap.cfg", r.out, argv)
            self.assertNotIn("openocd-buspirate.cfg", r.out, argv)
        self.assertEqual(self.openocd_calls(), [], "dry-run must not run openocd")


class TestAprom(ScriptTest):
    def test_verify_only_is_read_only(self):
        r = self.run_sh("flash-aprom.sh", "--verify-only", self.img)
        self.assertEqual(r.returncode, 0, r.out)
        self.assertIn("APROM matches boot.bin", r.out)
        self.assertNotIn("Type 'yes'", r.out)
        calls = self.openocd_calls()
        self.assertEqual(len(calls), 1)
        self.assertIn("gemifi_verify_aprom", " ".join(calls[0]))
        self.assert_no_writes()

    def test_verify_only_failure(self):
        r = self.run_sh("flash-aprom.sh", "--verify-only", self.img, STUB_VERIFY_FAIL="1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("APROM verify failed", r.out)
        self.assertIn("was 0x00 instead of 0x01", r.out.lower())
        self.assert_no_writes()

    def test_write_needs_yes(self):
        r = self.run_sh("flash-aprom.sh", self.img)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refusing to write", r.out)
        self.assertEqual(self.openocd_calls(), [])


if __name__ == "__main__":
    unittest.main()
