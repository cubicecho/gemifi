"""Presets (sysex.md 4.5): the codec, the simulated device (tests/sim_device.py), the CLI commands, backup and
restore across slots, and the after-upload revision check of `gemifi.py flash`."""
import contextlib
import dataclasses
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
import make_test_vectors  # noqa: E402
from sim_device import SimBackend, SimDevice, SimPort  # noqa: E402
from test_upload_ab import BOARD, V_OLD, esp_bin, flash_args  # noqa: E402

SCHEMA = make_test_vectors.example_preset_schema()
CH, FAST, ACT, NUM, ACTIVE, PROG, DIRTY = 1, 6, 9, 10, 17, 18, 19
PRESETS = [
    {"name": "Default"},
    {"name": "Drums", "vals": {(CH, 0): 10, (ACT, 0): 4, (NUM, 0): 36}, "program": 5},
    {"name": "Ableton", "vals": {(ACT, 1): 1, (NUM, 1): 65}},
]


def make_dev(**kw):
    kw.setdefault("presets", PRESETS)
    kw.setdefault("slots", 8)
    kw.setdefault("revision", 2)
    return SimDevice(role=gp.ROLE_APP, schema=kw.pop("schema", SCHEMA), **kw)


def session(dev):
    return gemifi.Session(SimPort(dev), timeout=0.005)


class TestCodec(unittest.TestCase):
    def test_numbers(self):
        self.assertEqual((gp.PRESET_INFO, gp.PRESET_SAVE, gp.PRESET_RESET, gp.PRESET_NAME, gp.PRESET_CHANGED),
                         (0x0B, 0x0C, 0x0D, 0x0E, 0x63))
        self.assertEqual((gp.CAP_PRESETS, gp.F_PRESET), (32, 32))
        self.assertEqual((gp.P_PRESET_ACTIVE, gp.P_PRESET_PROGRAM, gp.P_PRESET_DIRTY), (17, 18, 19))
        for c in (gp.PRESET_SAVE, gp.PRESET_RESET, gp.PRESET_NAME):
            self.assertIn(c, gp.LONG_TIMEOUT_CMDS)
        self.assertNotIn(gp.PRESET_INFO, gp.LONG_TIMEOUT_CMDS)

    def test_info_round_trip(self):
        i = gp.PresetInfo(2, 8, 2, gp.PI_STORED | gp.PI_DIRTY | gp.PI_YAML, 3, 2, -1, "Drums")
        payload = gp.build_preset_info_reply(i)
        self.assertEqual(len(payload), gp.PRESET_INFO_FIXED + 1 + 5)
        self.assertEqual(gp.parse_preset_info(payload), i)
        self.assertTrue(i.stored and i.dirty and i.yaml and i.is_active and i.stale)
        self.assertFalse(gp.PresetInfo(2, 8, 1, 0, 3, 0, 0, "x").stale)        # nothing stored: nothing to decide
        self.assertFalse(gp.PresetInfo(2, 8, 1, gp.PI_STORED, 3, 3, 0, "x").stale)
        f = gp.parse_frame(gp.build_frame(gp.PRESET_INFO | gp.REPLY, 9, payload))
        self.assertTrue(gp.reply_matches(f, gp.PRESET_INFO, 9))
        for bad in (payload[:-1], payload + b"\x00", payload[:gp.PRESET_INFO_FIXED]):
            with self.assertRaises(gp.ProtocolError):
                gp.parse_preset_info(bad)

    def test_names(self):
        self.assertEqual(gp.enc_preset_name("Drums"), b"\x05Drums")
        self.assertEqual(gp.enc_preset_name(""), b"\x00")
        self.assertEqual(gp.req_preset_name(3, "A b"), b"\x03\x00\x03A b")
        self.assertEqual(gp.dec_preset_name_req(gp.req_preset_name(300, "ABCDEFGHIJKLMNOP")), (300, "ABCDEFGHIJKLMNOP"))
        for bad in ("x" * 17, "tab\there", "café", "\x7f"):
            with self.assertRaises(ValueError, msg=bad):
                gp.enc_preset_name(bad)
        with self.assertRaises(gp.ProtocolError):
            gp.dec_preset_name(b"\x05abc")
        with self.assertRaises(gp.ProtocolError):
            gp.dec_preset_name_req(gp.req_preset_name(1, "ab") + b"x")

    def test_requests_and_changed(self):
        self.assertEqual(gp.req_preset_info(), b"\x00\x00")
        self.assertEqual(gp.req_preset_reset(), b"\x00\x00")
        self.assertEqual(gp.req_preset_save(130), b"\x02\x01")
        m = gp.parse_preset_changed(gp.build_preset_changed(5, True))
        self.assertEqual((m.slot, m.dirty), (5, 1))
        self.assertIn("unsaved", str(m))
        with self.assertRaises(gp.ProtocolError):
            gp.parse_preset_changed(b"\x05\x00")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            gemifi.print_unsolicited(gp.parse_frame(gp.build_frame(gp.PRESET_CHANGED, 1, gp.build_preset_changed(2, 0))))
        self.assertEqual(out.getvalue().strip(), "PRESET_CHANGED preset 2")

    def test_schema_flag(self):
        s = gp.parse_schema(gp.build_schema(SCHEMA))
        self.assertEqual(s, SCHEMA)
        self.assertTrue(s.by_name("button.action").per_preset)
        self.assertFalse(s.by_name("velocity.fast").per_preset)
        self.assertFalse(s.by_name("preset.active").per_preset)
        self.assertEqual(gemifi.flags_text(s.by_name("midi.channel")), "preset")
        self.assertEqual(s.by_name("button.action").options[13:], ["Preset next", "Preset previous", "Preset select"])


@unittest.skipUnless(os.path.exists(make_test_vectors.DEFAULT_OUT), "protocol/test-vectors.json not generated")
class TestPresetVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(make_test_vectors.DEFAULT_OUT) as f:
            cls.v = json.load(f)["presets"]

    def test_constants_and_schema(self):
        self.assertEqual((self.v["cap_presets"], self.v["flag_preset"]), (32, 32))
        raw = bytes.fromhex(self.v["schema"]["raw"])
        self.assertEqual(gp.parse_schema(raw).to_json(), self.v["schema"]["parsed"])

    def test_frames(self):
        for t in self.v["frames"]:
            f = gp.parse_frame(bytes.fromhex(t["frame"]))
            self.assertEqual((f.cmd, f.seq, f.payload.hex(" ").upper()), (t["cmd"], t["seq"], t["payload"]), t["desc"])
            fl = t["fields"]
            if f.is_reply:
                self.assertEqual(f.status, fl["status"])
                self.assertTrue(gp.reply_matches(f, f.cmd & ~gp.REPLY, f.seq), t["desc"])
                if f.cmd == gp.PRESET_INFO | gp.REPLY and f.status == gp.OK:
                    i = gp.parse_preset_info(f.payload)
                    self.assertEqual(i, gp.PresetInfo(fl["slot"], fl["count"], fl["active"], fl["flags"], fl["revision"],
                                                      fl["stored_revision"], fl["program"], fl["name"]))
            elif f.cmd in (gp.PRESET_INFO, gp.PRESET_SAVE, gp.PRESET_RESET):
                self.assertEqual(f.payload, gp.u14(fl["slot"]))
            elif f.cmd == gp.PRESET_NAME:
                self.assertEqual(f.payload, gp.u14(fl["slot"]) + bytes([len(fl["name"])]) + fl["name"].encode())
            elif f.cmd == gp.PRESET_CHANGED:
                m = gp.parse_preset_changed(f.payload)
                self.assertEqual((m.slot, m.dirty), (fl["slot"], fl["dirty"]))
            elif f.cmd == gp.SET_PARAM:
                self.assertEqual(f.payload, gp.req_set_param(fl["id"], fl["index"], fl["value"]))


class TestSim(unittest.TestCase):
    """The simulator follows research/core/presets-design.md §10.3/§10.4."""

    def test_select_dirty_save(self):
        dev = make_dev()
        s = session(dev)
        got = []
        s.on_unsolicited = got.append
        self.assertTrue(s.hello().caps & gp.CAP_PRESETS)
        self.assertEqual(s.preset_info().slot, 1)
        s.preset_select(2)
        self.assertEqual((dev.values[(CH, 0)], dev.values[(ACT, 0)], dev.values[(PROG, 0)]), (10, 4, 5))
        s.set_param(CH, 0, 11)
        self.assertEqual(s.get_param(DIRTY), 1)
        self.assertTrue(s.preset_info().dirty)
        s.set_param(FAST, 0, 30)                         # a global: no change to dirty
        s.preset_select(1)                               # unsaved edits dropped
        s.preset_select(2)
        self.assertEqual((dev.values[(CH, 0)], dev.values[(FAST, 0)]), (10, 30))
        changes = [gp.parse_preset_changed(f.payload) for f in got if f.cmd == gp.PRESET_CHANGED]
        self.assertEqual([(c.slot, c.dirty) for c in changes], [(2, 0), (2, 1), (1, 0), (2, 0)])
        s.set_param(CH, 0, 12)
        s.commit()
        i = s.preset_info(2)
        self.assertEqual((i.stored, i.dirty, i.stored_revision), (True, False, 2))
        self.assertEqual(dev.records[2]["vals"], {(CH, 0): 12})       # a diff against the slot's YAML values
        self.assertEqual(dev.stored[(ACTIVE, 0)], 2)

    def test_save_as_reset_name_errors(self):
        dev = make_dev()
        s = session(dev)
        s.set_values(ACT, 0, [6, 7])
        s.preset_save(5)
        self.assertEqual((s.preset_info().slot, dev.records[5]["vals"]), (5, {(ACT, 0): 6, (ACT, 1): 7}))
        self.assertNotIn(1, dev.records)
        s.preset_name(5, "Mine")
        i = s.preset_info(5)
        self.assertEqual((i.name, i.yaml, i.stored), ("Mine", False, True))
        s.preset_name(3, "Renamed")
        self.assertEqual((s.preset_info(3).name, dev.records[3]["vals"]), ("Renamed", {}))
        s.preset_name(3, "")
        self.assertEqual((s.preset_info(3).name, s.preset_info(3).stored), ("Ableton", False))
        s.preset_reset(5)                                 # the active one: reloads its YAML values
        self.assertEqual((dev.values[(ACT, 0)], s.preset_info(5).name), (1, "Preset 5"))
        for bad, st in ((lambda: s.preset_info(9), gp.BAD_PARAM), (lambda: s.preset_save(0), gp.BAD_PARAM),
                        (lambda: s.preset_reset(9), gp.BAD_PARAM), (lambda: s.set_param(ACTIVE, 0, 9), gp.BAD_VALUE),
                        (lambda: s.set_param(DIRTY, 0, 1), gp.BAD_PARAM)):
            with self.assertRaises(gemifi.StatusError) as e:
                bad()
            self.assertEqual(e.exception.status, st)
        f = s.request(gp.PRESET_NAME, gp.u14(1) + bytes([17]) + b"x" * 17)
        self.assertEqual(f.status, gp.BAD_VALUE)
        f = s.request(gp.PRESET_NAME, gp.u14(1) + b"\x02\x01\x02")
        self.assertEqual(f.status, gp.BAD_VALUE)
        self.assertEqual(s.request(gp.PRESET_INFO, b"\x01").status, gp.BAD_LEN)

    def test_boot_keeps_stored(self):
        dev = make_dev()
        s = session(dev)
        s.preset_select(3)
        s.set_param(CH, 0, 4)
        s.commit()
        dev.revision = 3
        dev.boot()
        i = s.preset_info()
        self.assertEqual((i.slot, dev.values[(CH, 0)], i.stale, i.stored_revision), (3, 4, True, 2))


class FakeBackend:
    label = "sim"

    def __init__(self, dev):
        self.port = SimPort(dev)

    def ports(self):
        return ["Keystation Mini 32 MK3 (gemifi) [hw:2,0]"]

    def open(self, name):
        return self.port


def run_cli(dev, *argv, stdin=None):
    a = gemifi.build_parser().parse_args(list(argv))
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
            mock.patch("sys.stdin", io.StringIO(stdin or "")):
        rc = gemifi.COMMANDS[a.cmd](a, FakeBackend(dev))
    return rc, out.getvalue()


class TestCli(unittest.TestCase):
    def test_presets_list(self):
        dev = make_dev()
        s = session(dev)
        s.preset_select(2)
        s.set_param(CH, 0, 3)
        dev.records[3] = {"vals": {(CH, 0): 2}, "name": None, "rev": 1}
        rc, out = run_cli(dev, "presets")
        self.assertEqual(rc, 0)
        self.assertIn("8 presets, active 2, firmware presets revision 2", out)
        self.assertRegex(out, r"\* +2  Drums +5 +unsaved edits")
        self.assertRegex(out, r"  3  Ableton +off +stored edits \(revision 1\)")
        self.assertRegex(out, r"  8  Preset 8 +off +empty slot")

    def test_no_presets(self):
        dev = SimDevice(role=gp.ROLE_APP, schema=make_test_vectors.example_schema())
        with self.assertRaisesRegex(gemifi.GemifiError, "no presets"):
            run_cli(dev, "presets")

    def test_select(self):
        dev = make_dev()
        rc, out = run_cli(dev, "preset", "select", "drums")
        self.assertEqual((rc, dev.active), (0, 2))
        self.assertIn("preset 2 (Drums) active", out)
        session(dev).set_param(CH, 0, 3)
        rc, out = run_cli(dev, "preset", "select", "1", stdin="no\n")
        self.assertEqual((rc, dev.active), (1, 2))                 # asked, refused
        self.assertIn("cancelled", out)
        rc, out = run_cli(dev, "preset", "select", "1", "--discard")
        self.assertEqual((rc, dev.active), (0, 1))
        with self.assertRaisesRegex(gemifi.GemifiError, "1..8"):
            run_cli(dev, "preset", "select", "9")
        with self.assertRaisesRegex(gemifi.GemifiError, "no preset named"):
            run_cli(dev, "preset", "select", "Nope")

    def test_save_reset_rename(self):
        dev = make_dev()
        session(dev).set_param(CH, 0, 7)
        rc, out = run_cli(dev, "preset", "save")
        self.assertEqual((rc, dev.records[1]["vals"]), (0, {(CH, 0): 7}))
        rc, out = run_cli(dev, "preset", "save", "6", "--name", "Seven")
        self.assertEqual((rc, dev.active, dev.records[6]["name"]), (0, 6, "Seven"))
        self.assertIn("saved into preset 6 (Seven), copied from 1", out)
        with self.assertRaisesRegex(gemifi.GemifiError, "printable"):
            run_cli(dev, "preset", "save", "--name", "x" * 17)
        rc, out = run_cli(dev, "preset", "rename", "2", "Kit")
        self.assertEqual(dev.records[2]["name"], "Kit")
        rc, out = run_cli(dev, "preset", "rename", "Kit", "")
        self.assertNotIn(2, dev.records)
        self.assertIn("'Drums' (the firmware's name)", out)
        rc, out = run_cli(dev, "preset", "reset", "6", stdin="\n")
        self.assertEqual(rc, 1)
        self.assertIn(6, dev.records)
        rc, out = run_cli(dev, "preset", "reset", "Seven", "--yes")
        self.assertNotIn(6, dev.records)
        rc, out = run_cli(dev, "preset", "reset", "all", stdin="yes\n")
        self.assertEqual((rc, dev.records), (0, {}))


def edited_dev(**kw):
    """A device with stored edits in slots 1, 2 and 6, a renamed slot 4, slot 2 active with unsaved edits."""
    dev = make_dev(**kw)
    s = session(dev)
    s.set_param(FAST, 0, 40)
    s.set_param(CH, 0, 2)
    s.commit()
    s.preset_select(6)
    s.set_values(NUM, 0, [70, 71])
    s.set_param(PROG, 0, 9)
    s.preset_save(6)
    s.preset_name(4, "Four")
    s.preset_select(2)
    s.set_param(ACT, 1, 15)
    s.preset_save(2)
    s.set_param(CH, 0, 16)                      # unsaved
    dev.outbox.clear()
    dev.log.clear()
    return dev


class TestBackupRestore(unittest.TestCase):
    def test_round_trip(self):
        src = edited_dev()
        before = (dict(src.values), {k: dict(v) for k, v in src.records.items()}, src.active, src.dirty)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "b.json")
            rc, out = run_cli(src, "backup", path)
            self.assertEqual(rc, 0, out)
            self.assertIn("8 presets", out)
            # the device ends as it started: same slot, its unsaved edit put back, dirty again
            self.assertEqual((dict(src.values), src.records, src.active, src.dirty), before)
            with open(path) as f:
                doc = json.load(f)
            self.assertEqual((doc["gemifi_backup"], doc["active"], doc["presets_revision"]), (2, 2, 2))
            self.assertEqual(doc["values"]["velocity.fast"], 40)
            self.assertNotIn("preset.active", doc["values"])
            self.assertNotIn("midi.channel", doc["values"])
            p = {e["slot"]: e for e in doc["presets"]}
            self.assertEqual(sorted(p), list(range(1, 9)))
            self.assertEqual((p[2]["values"]["midi.channel"], p[2]["unsaved"]), (16, True))
            self.assertEqual((p[6]["values"]["button.number"], p[6]["values"]["preset.program"]), ([70, 71], 9))
            self.assertEqual((p[4]["name"], p[4]["stored"], p[5]["stored"]), ("Four", True, False))

            dst = make_dev()
            s = session(dst)
            s.preset_select(7)
            s.set_param(CH, 0, 5)
            s.preset_save(7)                     # an edit the backup doesn't have: dropped by the restore
            rc, out = run_cli(dst, "restore", path)
            self.assertEqual(rc, 0, out)
            self.assertIn("stored in flash", out)
        self.assertEqual((dst.active, dst.dirty), (2, False))
        self.assertEqual(dst.values[(CH, 0)], 16)            # the unsaved edit was saved by the restore
        self.assertEqual(dst.values[(FAST, 0)], 40)
        self.assertNotIn(7, dst.records)
        self.assertEqual(dst.records[6]["vals"], src.records[6]["vals"])
        self.assertEqual(dst.records[4], {"vals": {}, "name": "Four", "rev": 2})
        self.assertEqual(dst.records[1]["vals"], {(CH, 0): 2})
        self.assertEqual(dst.records[2]["vals"], {(CH, 0): 16, (ACT, 1): 15})
        self.assertEqual(dst.stored[(ACTIVE, 0)], 2)
        for n in range(1, 9):
            self.assertEqual(dst.slot_values(n), src.slot_values(n) if n != 2 else {**src.slot_values(2), (CH, 0): 16})

    def test_restore_more_slots_than_device(self):
        src = edited_dev()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "b.json")
            run_cli(src, "backup", path)
            schema = make_test_vectors.example_preset_schema()
            schema.by_name("preset.active").max = 4
            dst = make_dev(slots=4, schema=schema)
            rc, out = run_cli(dst, "restore", path)
        self.assertIn("preset 6: this device has presets 1..4; skipped", out)
        self.assertEqual(sorted(dst.records), [1, 2, 4])

    def test_old_backup_on_preset_device_and_back(self):
        dev = make_dev()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "b.json")
            with open(path, "w") as f:
                json.dump({"gemifi_backup": 1, "board_id": 1, "values": {"midi.channel": 9}}, f)
            rc, out = run_cli(dev, "restore", path)
            self.assertIn("go into the active preset (1)", out)
            self.assertEqual((dev.values[(CH, 0)], dev.dirty), (9, True))
            run_cli(edited_dev(), "backup", path)
            plain = SimDevice(role=gp.ROLE_APP, schema=make_test_vectors.example_schema())
            rc, out = run_cli(plain, "restore", path)
        self.assertIn("doesn't; restoring the globals and preset 2 only", out)
        self.assertEqual(plain.values[(1, 0)], 16)


class TestFlashPresets(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.img = os.path.join(self.tmp.name, "app.bin")
        with open(self.img, "wb") as f:
            f.write(esp_bin(1000))

    def tearDown(self):
        self.tmp.cleanup()

    def dev(self, next_revision=3):
        dev = edited_dev(ab=True, board_id=BOARD, fw_version=V_OLD, next_revision=next_revision)
        dev.records[3] = {"vals": {(CH, 0): 4}, "name": None, "rev": 3}     # already on the new revision
        return dev

    def flash(self, dev, stdin="", **kw):
        kw.setdefault("backup", os.path.join(self.tmp.name, "pre.json"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch("sys.stdin", io.StringIO(stdin)):
            rc = gemifi.cmd_flash(flash_args(self.img, **kw), lambda: SimBackend(dev))
        return rc, out.getvalue()

    def test_backup_before_upload(self):
        dev = self.dev()
        rc, out = self.flash(dev, keep_presets=True)
        self.assertEqual(rc, 0, out)
        with open(os.path.join(self.tmp.name, "pre.json")) as f:
            doc = json.load(f)
        self.assertEqual((doc["gemifi_backup"], len(doc["presets"]), doc["fw_version"]), (2, 8, "0.1.0"))
        self.assertLess(dev.log.index("GET_PARAMS"), dev.log.index("FW_BEGIN"))

    def test_auto_backup_name_and_no_backup(self):
        dev = self.dev()
        cwd = os.getcwd()
        os.chdir(self.tmp.name)
        try:
            rc, out = self.flash(dev, backup=None, keep_presets=True)
            names = [n for n in os.listdir(".") if n.startswith("gemifi-backup-3f00-0.1.0-")]
            self.assertEqual(len(names), 1, out)
            dev = self.dev()
            rc, out = self.flash(dev, backup=None, no_backup=True, keep_presets=True)
            self.assertEqual(len([n for n in os.listdir(".") if n.startswith("gemifi-backup-")]), 1)
            self.assertLess(dev.log.index("FW_BEGIN"), 4)                     # HELLO, then straight to the upload
        finally:
            os.chdir(cwd)

    def test_keep(self):
        dev = self.dev()
        rc, out = self.flash(dev, keep_presets=True)
        self.assertEqual(rc, 0)
        self.assertIn("The presets in this firmware changed (revision 3)", out)
        self.assertIn("1 Default (revision 2), 2 Drums (revision 2), 4 Four (revision 2), 6 Preset 6 (revision 2)", out)
        self.assertNotIn("3 Ableton", out)
        self.assertIn("kept your edits", out)
        self.assertEqual(sorted(dev.records), [1, 2, 3, 4, 6])
        self.assertNotIn("PRESET_RESET", dev.log)

    def test_use_firmware_presets(self):
        dev = self.dev()
        rc, out = self.flash(dev, use_firmware_presets=True)
        self.assertEqual(rc, 0)
        self.assertEqual(dev.records, {})
        self.assertIn("undo: gemifi.py restore", out)
        self.assertEqual(dev.stored[(FAST, 0)], 40)                        # the globals stay

    def test_asks(self):
        dev = self.dev()
        rc, out = self.flash(dev, stdin="u\n")
        self.assertEqual(dev.records, {})
        self.assertIn("[K]eep / [u]se", out)
        dev = self.dev()
        rc, out = self.flash(dev, stdin="")                                # no answer: keep
        self.assertEqual(sorted(dev.records), [1, 2, 3, 4, 6])

    def test_same_revision_no_question(self):
        dev = self.dev(next_revision=None)
        del dev.records[3]
        rc, out = self.flash(dev, stdin="u\n")
        self.assertNotIn("changed", out)
        self.assertEqual(sorted(dev.records), [1, 2, 4, 6])

    def test_cli_flags(self):
        a = gemifi.build_parser().parse_args(["flash", "x.bin", "--backup", "b.json", "--use-firmware-presets"])
        self.assertEqual((a.backup, a.use_firmware_presets, a.keep_presets, a.no_backup), ("b.json", True, False, False))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            gemifi.build_parser().parse_args(["flash", "x.bin", "--keep-presets", "--use-firmware-presets"])


if __name__ == "__main__":
    unittest.main()


def count_schema():
    """The example schema with preset.count (33): a firmware with PRESET_DELETE (sysex.md 4.5)."""
    schema = make_test_vectors.example_preset_schema()
    schema.params.append(gp.make_param(gp.P_PRESET_COUNT, "preset.count", gp.T_INT, 2, 1, 8, 1, "Presets in use",
                                       flags=gp.F_READ_ONLY))
    return schema


def new_dev(**kw):
    """Slots 1..3 (Default, Drums, Ableton) in use, 4..8 free."""
    return make_dev(schema=count_schema(), **kw)


class TestPresetsInUse(unittest.TestCase):
    def test_codec(self):
        self.assertEqual((gp.PRESET_DELETE, gp.PI_IN_USE, gp.P_PRESET_COUNT), (0x12, 8, 33))
        self.assertIn(gp.PRESET_DELETE, gp.LONG_TIMEOUT_CMDS)
        self.assertEqual(gp.req_preset_delete(130), b"\x02\x01")
        old = [gp.PresetInfo(n, 3, 1, 0, 0, 0, -1, f"P{n}") for n in (1, 2, 3)]
        self.assertEqual(gp.presets_in_use(old), old)                    # no bit 3: every slot is a preset
        new = [dataclasses.replace(i, flags=gp.PI_IN_USE if i.slot < 3 else 0) for i in old]
        self.assertEqual([i.slot for i in gp.presets_in_use(new)], [1, 2])

    def test_free_slots(self):
        dev = new_dev()
        s = session(dev)
        infos = s.preset_infos()
        self.assertEqual([i.slot for i in gp.presets_in_use(infos)], [1, 2, 3])
        with self.assertRaises(gemifi.StatusError) as e:
            s.set_param(ACTIVE, 0, 4)
        self.assertEqual(e.exception.status, gp.BAD_VALUE)
        with self.assertRaises(gemifi.StatusError) as e:
            s.set_param(gp.P_PRESET_COUNT, 0, 5)
        self.assertEqual(e.exception.status, gp.BAD_PARAM)            # read-only to a host
        s.preset_name(4, "Lead")                                       # a free slot becomes a preset
        i = s.preset_info(4)
        self.assertEqual((i.in_use, i.name, dev.used(), dev.stored[(gp.P_PRESET_COUNT, 0)]), (True, "Lead", 4, 4))
        s.preset_save(6)                                               # and so do the ones before a saved one
        self.assertEqual((dev.used(), s.preset_info(5).in_use, dev.active), (6, True, 6))
        s.preset_reset(gp.PRESET_ALL)                                  # the firmware's presets: the YAML's list
        self.assertEqual((dev.used(), dev.active, dev.records), (3, 1, {}))

    def test_delete(self):
        dev = new_dev()
        s = session(dev)
        s.preset_name(4, "Lead")
        s.preset_select(2)
        s.set_param(CH, 0, 3)                                          # unsaved: dropped by the delete
        s.preset_delete(2)
        names = [s.preset_info(n).name for n in (1, 2, 3)]
        self.assertEqual((names, dev.used(), s.preset_info(4).in_use), (["Default", "Ableton", "Lead"], 3, False))
        self.assertEqual((dev.active, dev.dirty, dev.values[(ACT, 1)], dev.values[(NUM, 1)]), (2, False, 1, 65))
        self.assertEqual(dev.slot_values(3), dev.slot_values(1))       # Lead: a copy of Default, now in slot 3
        self.assertNotIn(4, dev.records)
        s.preset_select(3)
        s.preset_delete(1)                                             # the active one moves up
        self.assertEqual((dev.active, s.preset_info(0).name, dev.used()), (2, "Lead", 2))
        s.preset_delete(2)                                             # the last one: back to the one before
        self.assertEqual((dev.active, dev.used(), s.preset_info(1).name), (1, 1, "Ableton"))
        for slot, st in ((1, gp.BAD_STATE), (2, gp.BAD_PARAM), (0, gp.BAD_PARAM)):
            with self.assertRaises(gemifi.StatusError) as e:
                s.preset_delete(slot)
            self.assertEqual(e.exception.status, st, slot)
        dev.boot()
        self.assertEqual((dev.used(), s.preset_info(1).name), (1, "Ableton"))

    def test_old_firmware_has_no_delete(self):
        s = session(make_dev())
        self.assertEqual(s.request(gp.PRESET_DELETE, gp.req_preset_delete(2)).status, gp.BAD_CMD)
        with self.assertRaisesRegex(gemifi.GemifiError, "no PRESET_DELETE"):
            run_cli(make_dev(), "preset", "delete", "2", "--yes")

    def test_boot_keeps_stored_past_the_count(self):
        dev = new_dev()
        dev.records[6] = {"vals": {(CH, 0): 2}, "name": None, "rev": 2}   # a store from before preset.count
        dev.boot()
        self.assertEqual(dev.used(), 6)

    def test_cli(self):
        dev = new_dev()
        rc, out = run_cli(dev, "presets")
        self.assertIn("8 presets (3 in use), active 1", out)
        self.assertRegex(out, r"  4  Preset 4 +off +free")
        with self.assertRaisesRegex(gemifi.GemifiError, "slot 5 is free"):
            run_cli(dev, "preset", "select", "5")
        session(dev).preset_select(2)
        rc, out = run_cli(dev, "preset", "delete", "drums", stdin="no\n")
        self.assertEqual((rc, dev.used()), (1, 3))
        self.assertIn("preset 3 moves up one slot; Ableton (2) is active afterwards", out)
        rc, out = run_cli(dev, "preset", "delete", "drums", "--yes")
        self.assertEqual((rc, dev.used(), dev.active), (0, 2, 2))
        self.assertIn("preset 2 (Drums) deleted; 2 in use, preset 2 (Ableton) active", out)
        with self.assertRaisesRegex(gemifi.GemifiError, "slot 3 is free"):
            run_cli(dev, "preset", "delete", "3", "--yes")
        run_cli(dev, "preset", "delete", "2", "--yes")
        with self.assertRaisesRegex(gemifi.GemifiError, "the last preset can't be deleted"):
            run_cli(dev, "preset", "delete", "1", "--yes")

    def test_backup_and_restore(self):
        src = new_dev()
        s = session(src)
        s.preset_name(4, "Lead")
        s.preset_select(4)
        s.set_param(CH, 0, 7)
        s.commit()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "b.json")
            rc, out = run_cli(src, "backup", path)
            self.assertEqual(rc, 0, out)
            with open(path) as f:
                doc = json.load(f)
            self.assertEqual([e["slot"] for e in doc["presets"]], [1, 2, 3, 4])   # free slots aren't backed up
            dst = new_dev()                                            # 3 in use: slot 4 is made again
            rc, out = run_cli(dst, "restore", path)
            self.assertEqual(rc, 0, out)
            self.assertEqual((dst.used(), dst.active, dst.values[(CH, 0)], s_name(dst, 4)), (4, 4, 7, "Lead"))
            more = new_dev()                                           # 6 in use: 5 and 6 are deleted
            m = session(more)
            m.preset_name(5, "Five")
            m.preset_name(6, "Six")
            rc, out = run_cli(more, "restore", path)
            self.assertEqual(rc, 0, out)
            self.assertEqual((more.used(), s_name(more, 4), more.active), (4, "Lead", 4))
            self.assertNotIn(5, more.records)


def s_name(dev, slot):
    return session(dev).preset_info(slot).name
