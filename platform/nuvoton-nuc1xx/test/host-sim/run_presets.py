"""Presets end to end: the midi-controller's core (core/src, presets.c and app_proto.c included) with the
generated Keystation device config and the real NUC config_store.c on simulated data flash
(build/libctl.so, harness_ctl.c), driven with tools/host/gemifi_proto.py. A "reboot" reloads the library
(fresh RAM) and keeps the flash. Run via `make test` (or `make presets-test`)."""
import _ctypes, ctypes, os, random, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / "tools" / "host"))
import gemifi_proto as gp
import gemifi as gh
BUILD = Path(os.environ.get("SIM_BUILD", HERE / "build"))

FLASH_BASE, FLASH_SIZE, DEVID = 0x1F000, 0x1000, 0x1FE00
P_CHANNEL, P_OCTAVE, P_VEL_CURVE, P_VEL_FIXED, P_VEL_FAST = 1, 3, 4, 5, 6
P_ANALOG_CC, P_ACTION, P_NUMBER, P_GLIDE, P_RAMP, P_LOG = 8, 9, 10, 11, 12, 14
P_ACTIVE, P_PROGRAM, P_DIRTY = gp.P_PRESET_ACTIVE, gp.P_PRESET_PROGRAM, gp.P_PRESET_DIRTY
ACT_NOTE, ACT_PRESET_NEXT, ACT_PRESET_PREV = 4, 13, 14
SUSTAIN, EDIT, MOD = 0, 1, 6               # button indexes (Edit: the mode button)
PITCH_DOWN, PITCH_UP = 4, 5                # the unlock combo (Makefile adds unlock: true: the boot_combo, sysex.md 4.9)
N_SLOTS = 8

fails = 0
def check(c, m):
    global fails; print(("ok   " if c else "FAIL ") + m); fails += (not c)
    return c


class Dev:
    """The device: libctl.so + a flash array (and op counters) that outlive reboots."""

    def __init__(self):
        self.flash = (ctypes.c_uint8 * FLASH_SIZE)(*([0xFF] * FLASH_SIZE))
        self.cnt = (ctypes.c_uint32 * 4)()     # erases, programs, overprograms, bad addresses
        self.seq = 0
        self.L = None
        self.boot()

    def boot(self, safe=0):
        if self.L is not None:
            _ctypes.dlclose(self.L._handle)
        self.L = ctypes.CDLL(str(BUILD / "libctl.so"))
        assert ctypes.c_int.in_dll(self.L, "sim_booted").value == 0, "library not reloaded: RAM kept"
        self.L.sim_set_flash(self.flash, self.cnt)
        self.L.sim_boot(safe)
        self.advance(1600)                     # past the boot log
        return self.take()

    reboot = boot

    @property
    def erases(self): return self.cnt[0]
    @property
    def programs(self): return self.cnt[1]

    def take(self):
        """Everything the device sent since the last take: [(cable, bytes)]."""
        buf = (ctypes.c_uint8 * (1 << 20))()
        n = self.L.sim_take(buf, len(buf))
        d, out, i = bytes(buf[:n]), [], 0
        while i < n:
            ln = d[i] | d[i + 1] << 8
            out.append((d[i + 2], d[i + 3:i + 3 + ln]))
            i += 3 + ln
        return out

    def advance(self, ms):
        self.L.sim_advance(ms)

    def feed(self, data):
        self.L.sim_feed(bytes(data), len(data))

    def request(self, cmd, payload=b""):
        """-> (reply Frame or None, [unsolicited Frames], [channel messages]) for one request."""
        self.seq = (self.seq + 1) & 0x7F
        self.feed(gp.build_frame(cmd, self.seq, payload))
        self.advance(2)
        return self.split(self.take(), cmd, self.seq)

    @staticmethod
    def split(msgs, cmd=None, seq=None):
        reply, unsol, chan = None, [], []
        for _, m in msgs:
            if m[0] != 0xF0:
                chan.append(bytes(m))
            elif gp.is_gemifi(m):
                f = gp.parse_frame(m)
                if f.is_unsolicited:
                    unsol.append(f)
                elif cmd is not None and gp.reply_matches(f, cmd, seq):
                    reply = f
        return reply, unsol, chan

    def unlock(self):
        """Hold the unlock combo for its 2 s (sysex.md 4.9), then let go. -> the UNLOCKED states sent."""
        self.L.sim_button(PITCH_DOWN, 1); self.L.sim_button(PITCH_UP, 1); self.advance(2100)
        u = self.split(self.take())[1]
        self.L.sim_button(PITCH_DOWN, 0); self.L.sim_button(PITCH_UP, 0); self.advance(1100); self.take()
        return [gp.parse_unlocked(f.payload) for f in u if f.cmd == gp.UNLOCKED]

    def ok(self, cmd, payload=b"", what=""):
        r, u, c = self.request(cmd, payload)
        if r is not None and r.status == gp.LOCKED:     # FACTORY_RESET: the user's OK first
            self.unlock()
            r, u, c = self.request(cmd, payload)
        if r is None or r.status != gp.OK:
            raise RuntimeError(f"{what or gp.CMD_NAMES[cmd]}: " +
                               ("no reply" if r is None else gp.STATUS.get(r.status, str(r.status))))
        return r, u, c

    def status(self, cmd, payload=b""):
        r, _, _ = self.request(cmd, payload)
        return None if r is None else r.status

    def get(self, pid, index=0):
        return gp.parse_param_reply(self.ok(gp.GET_PARAM, gp.req_get_param(pid, index))[0].payload).value

    def set(self, pid, index, value):
        return self.ok(gp.SET_PARAM, gp.req_set_param(pid, index, value), f"SET_PARAM {pid}[{index}]={value}")

    def select(self, slot):
        return self.set(P_ACTIVE, 0, slot)

    def info(self, slot=0):
        return gp.parse_preset_info(self.ok(gp.PRESET_INFO, gp.req_preset_info(slot))[0].payload)

    def commit(self):
        return self.ok(gp.COMMIT)

    def press(self, i, ms=20):
        self.L.sim_button(i, 1); self.advance(ms)
        return self.split(self.take())

    def release(self, i, ms=20):
        self.L.sim_button(i, 0); self.advance(ms)
        return self.split(self.take())

    def key(self, k, down, gap_ticks=40):
        """Key k down (contact 1, gap_ticks scans, contact 2) or up."""
        if down:
            self.L.sim_key(k, 1); self.L.sim_scan(gap_ticks); self.L.sim_key(k, 3)
        else:
            self.L.sim_key(k, 0)
        self.advance(5)
        return self.split(self.take())

    def schema(self):
        data, total = bytearray(), None
        while total is None or len(data) < total:
            r, _, _ = self.ok(gp.GET_SCHEMA, gp.req_get_schema(len(data)))
            total, off, chunk = gp.parse_schema_reply(r.payload)
            assert off == len(data) and chunk
            data += chunk
        return gp.parse_schema(bytes(data))

    def journal_blank(self):
        return all(b == 0xFF for b in self.flash[:DEVID - FLASH_BASE])


def changed(unsol):
    return [gp.parse_preset_changed(f.payload) for f in unsol if f.cmd == gp.PRESET_CHANGED]


def param_changed(unsol):
    return [gp.parse_param_changed(f.payload) for f in unsol if f.cmd == gp.PARAM_CHANGED]


def notes(chan):
    return [m for m in chan if m[0] & 0xE0 == 0x80]      # Note On / Note Off


d = Dev()

# ---- 1. HELLO, schema, PRESET_INFO ---------------------------------------
print("-- HELLO, schema, PRESET_INFO")
h = gp.parse_hello(d.ok(gp.HELLO)[0].payload); print("    ", h)
check(h.role == gp.ROLE_APP and h.caps & gp.CAP_PRESETS and h.caps & gp.CAP_CONFIG, "HELLO caps: config + presets (bit 5)")
s = d.schema()
per = {p.name for p in s.params if p.per_preset}
glob = {p.name for p in s.params if not p.per_preset}
print("     per preset:", sorted(per)); print("     global:", sorted(glob))
check(per == {"midi.channel", "midi.transpose", "midi.octave", "velocity.curve", "velocity.fixed", "button.action",
              "button.number", "bend.glide", "ramp.time", "analog.cc", "analog.mode", "preset.program"}, "schema: PRESET flag on the mapping params")
check({"velocity.fast", "velocity.slow", "debug.events", "debug.log_level", "preset.active", "preset.dirty"} <= glob,
      "schema: calibration, debug and preset.active/dirty are global")
pa, pdirty = s.by_id(P_ACTIVE), s.by_id(P_DIRTY)
check(pa and pa.name == "preset.active" and (pa.min, pa.max, pa.default()) == (1, N_SLOTS, 1), "schema: preset.active 1..8, default 1")
check(pdirty and pdirty.read_only and pdirty.volatile, "schema: preset.dirty READ_ONLY | VOLATILE")
act = s.by_id(P_ACTION)
check(act.options[ACT_PRESET_NEXT:ACT_PRESET_NEXT + 3] == ["Preset next", "Preset previous", "Preset select"],
      "schema: button.action options 13..15 = preset next/previous/select")
want = ["Default", "Drums", "DAW"] + [f"Preset {n}" for n in range(4, 9)]
infos = [d.info(n) for n in range(1, N_SLOTS + 1)]
check([i.name for i in infos] == want, f"PRESET_INFO names {[i.name for i in infos]}")
check([i.yaml for i in infos] == [True] * 3 + [False] * 5, "PRESET_INFO YAML flag on slots 1..3 only")
check(all(i.slot == n + 1 and i.count == N_SLOTS and i.active == 1 and i.revision == 0 and not i.stored and
          not i.dirty and i.program == -1 for n, i in enumerate(infos)), "PRESET_INFO count 8, active 1, revision 0, nothing stored")
check(d.info(0).slot == 1, "PRESET_INFO 0 = the active slot")
check(d.journal_blank(), "fresh device: the journal is blank")

# ---- 2. select slot 2 ----------------------------------------------------
print("-- select Drums")
r, u, c = d.select(2)
check(gp.parse_param_reply(r.payload).value == 2, "SET_PARAM preset.active = 2 OK")
check([(x.slot, x.dirty) for x in changed(u)] == [(2, 0)], f"PRESET_CHANGED (2, clean): {changed(u)}")
check(not param_changed(u), "no PARAM_CHANGED for a host switch")
check(d.get(P_ACTIVE) == 2 and d.get(P_CHANNEL) == 10 and d.get(P_VEL_CURVE) == 2, "Drums: channel 10, curve hard")
check([d.get(P_ACTION, i) for i in (SUSTAIN, MOD)] == [ACT_NOTE, ACT_NOTE] and [d.get(P_NUMBER, i) for i in (SUSTAIN, MOD)] == [36, 38],
      "Drums: sustain/mod = note 36/38")
check(d.get(P_ACTION, 2) == 5 and d.get(P_ANALOG_CC) == 7, "Drums: the rest is slot 1's (oct- action, volume CC 7)")
_, _, c = d.press(SUSTAIN); check(notes(c) == [bytes([0x99, 36, 100])], f"sustain -> Note On 36 ch 10: {c}")
_, _, c = d.release(SUSTAIN); check(notes(c) == [bytes([0x89, 36, 0])], f"release -> Note Off 36 ch 10: {c}")
_, _, c = d.key(0, 1); check(len(notes(c)) == 1 and notes(c)[0][:2] == bytes([0x99, 48]), f"key 0 -> Note On 48 ch 10: {c}")
_, _, c = d.key(0, 0); check(notes(c) == [bytes([0x89, 48, 0])], f"key 0 up -> Note Off: {c}")

# ---- 3. dirty, discard ---------------------------------------------------
print("-- dirty edits are dropped on a switch")
p0 = d.programs
r, u, _ = d.set(P_GLIDE, 0, 500)
check([(x.slot, x.dirty) for x in changed(u)] == [(2, 1)], f"first edit: PRESET_CHANGED (2, dirty): {changed(u)}")
_, u, _ = d.set(P_RAMP, 0, 900); check(not changed(u), "second edit: no new PRESET_CHANGED")
check(d.get(P_DIRTY) == 1 and d.info(0).dirty and not d.info(1).dirty, "preset.dirty 1, PRESET_INFO dirty on the active slot only")
_, u, _ = d.set(P_VEL_FAST, 0, 20); check(not changed(u), "a global edit doesn't touch dirty")
d.set(P_VEL_FAST, 0, 16)
check(d.status(gp.SET_PARAM, gp.req_set_param(P_DIRTY, 0, 0)) != gp.OK and d.get(P_DIRTY) == 1, "preset.dirty is read-only")
_, u, _ = d.select(1)
check([(x.slot, x.dirty) for x in changed(u)] == [(1, 0)] and d.get(P_DIRTY) == 0, "switch to 1: clean")
check(d.get(P_GLIDE) == 80 and d.get(P_CHANNEL) == 1, "slot 1 values")
d.select(2)
check(d.get(P_GLIDE) == 80 and d.get(P_RAMP) == 300 and d.get(P_CHANNEL) == 10, "back on 2: the edits are gone")
d.set(P_GLIDE, 0, 500)
_, u, _ = d.select(2)
check([(x.slot, x.dirty) for x in changed(u)] == [(2, 0)] and d.get(P_GLIDE) == 80, "reselecting the active slot drops its edits")
check(d.programs == p0, "no flash write so far (no COMMIT, < 10 s)")

# ---- 4. COMMIT, reboot ---------------------------------------------------
print("-- COMMIT survives a reboot")
d.set(P_NUMBER, 0, 40)
_, u, _ = d.commit()
check([(x.slot, x.dirty) for x in changed(u)] == [(2, 0)] and d.get(P_DIRTY) == 0, "COMMIT: PRESET_CHANGED (2, clean)")
check(d.programs > p0 and d.cnt[2] == 0 and d.cnt[3] == 0, "COMMIT wrote flash (no word programmed twice, no bad address)")
i2 = d.info(2); check(i2.stored and i2.yaml and i2.stored_revision == 0 and not i2.dirty, f"PRESET_INFO 2: stored + YAML ({i2})")
check(not d.info(1).stored, "slot 1 still has nothing stored")
d.reboot()
check(d.get(P_ACTIVE) == 2 and d.info(0).slot == 2, "after reboot: slot 2 active")
check(d.get(P_NUMBER, 0) == 40 and d.get(P_NUMBER, MOD) == 38 and d.get(P_CHANNEL) == 10, "after reboot: the edit is there, YAML values too")
check(d.info(2).stored and d.get(P_DIRTY) == 0, "after reboot: stored, clean")
_, _, c = d.press(SUSTAIN); d.release(SUSTAIN)
check(notes(c) == [bytes([0x99, 40, 100])], f"sustain sends the stored note 40: {c}")
d.select(1); check(d.get(P_NUMBER, 0) == 64 and d.get(P_ACTION, 0) == 1, "slot 1 untouched (CC 64 momentary)")
d.reboot()
check(d.get(P_ACTIVE) == 2, "a host switch not yet 10 s old isn't stored (still 2 after reboot)")

# ---- 5. save-as, names, resets, factory reset ----------------------------
print("-- PRESET_SAVE, PRESET_NAME, PRESET_RESET, FACTORY_RESET")
d.set(P_RAMP, 0, 1000)
r, u, _ = d.ok(gp.PRESET_SAVE, gp.req_preset_save(5))
check([(x.slot, x.dirty) for x in changed(u)] == [(5, 0)], f"PRESET_SAVE 5: PRESET_CHANGED (5, clean): {changed(u)}")
check(d.get(P_ACTIVE) == 5 and d.get(P_RAMP) == 1000 and d.get(P_NUMBER, 0) == 40 and d.get(P_CHANNEL) == 10,
      "slot 5 active with the live values")
i5 = d.info(5); check(i5.stored and not i5.yaml and i5.name == "Preset 5", f"PRESET_INFO 5: stored, not YAML ({i5})")
d.select(2); check(d.get(P_RAMP) == 300 and d.get(P_NUMBER, 0) == 40, "slot 2 kept its own stored values")
_, u, _ = d.ok(gp.PRESET_NAME, gp.req_preset_name(5, "Drums copy"))
check(not changed(u) and d.info(5).name == "Drums copy", "PRESET_NAME 5 (inactive slot): stored at once, no PRESET_CHANGED")
d.set(P_GLIDE, 0, 222)
_, u, _ = d.ok(gp.PRESET_NAME, gp.req_preset_name(2, "Kit"))
check(not changed(u) and d.get(P_DIRTY) == 1 and d.info(2).name == "Kit", "PRESET_NAME on the dirty active slot keeps dirty")
d.select(2)
check(d.get(P_GLIDE) == 80 and d.info(2).name == "Kit" and d.get(P_NUMBER, 0) == 40, "rename kept the stored values, not the unsaved one")
d.commit()
d.reboot()
check([d.info(n).name for n in (2, 5)] == ["Kit", "Drums copy"], "names survive a reboot")
d.select(5); check(d.get(P_RAMP) == 1000 and d.get(P_CHANNEL) == 10, "slot 5 values survive a reboot")
d.ok(gp.PRESET_NAME, gp.req_preset_name(5, ""))
i5 = d.info(5); check(i5.name == "Preset 5" and i5.stored, "empty name: back to 'Preset 5', edits stay")
d.ok(gp.PRESET_NAME, gp.req_preset_name(6, "Empty"))
check(d.info(6).stored and d.info(6).name == "Empty", "a name alone marks a slot stored")
d.set(P_RAMP, 0, 1111)
_, u, _ = d.ok(gp.PRESET_RESET, gp.req_preset_reset(5))
check([(x.slot, x.dirty) for x in changed(u)] == [(5, 0)], f"PRESET_RESET of the active slot reloads it: {changed(u)}")
check(not d.info(5).stored and d.get(P_RAMP) == 300 and d.get(P_CHANNEL) == 1 and d.get(P_NUMBER, 0) == 64,
      "slot 5 is a copy of slot 1 again")
check(d.info(2).stored and d.info(6).stored, "other slots untouched by PRESET_RESET 5")
d.select(2)
d.set(P_VEL_FAST, 0, 30); d.commit()                   # a global
d.ok(gp.PRESET_RESET, gp.req_preset_reset(gp.PRESET_ALL))
check(all(not d.info(n).stored for n in range(1, 9)), "PRESET_RESET 0: no slot stored")
check(d.get(P_NUMBER, 0) == 36 and d.info(2).name == "Drums" and d.get(P_ACTIVE) == 2, "active slot 2 reloaded: YAML Drums")
check(d.get(P_VEL_FAST) == 30, "PRESET_RESET leaves the globals")
d.reboot()
check(d.get(P_VEL_FAST) == 30 and d.get(P_ACTIVE) == 2 and d.get(P_NUMBER, 0) == 36, "after reboot: globals kept, presets YAML")
d.set(P_NUMBER, 0, 50); d.commit()
d.ok(gp.PRESET_NAME, gp.req_preset_name(4, "Mine"))
d.set(P_LOG, 0, 4)
check(d.status(gp.FACTORY_RESET) == gp.LOCKED and not d.journal_blank(), "FACTORY_RESET locked: nothing erased")
check(d.unlock() == [1], "the unlock combo held 2 s: UNLOCKED 1")
_, u, _ = d.ok(gp.FACTORY_RESET)
check([(x.slot, x.dirty) for x in changed(u)] == [(1, 0)], f"FACTORY_RESET: PRESET_CHANGED (1, clean): {changed(u)}")
check(d.journal_blank(), "FACTORY_RESET: the journal is erased")
def all_yaml(tag):
    ok = d.get(P_ACTIVE) == 1 and d.get(P_VEL_FAST) == 16 and d.get(P_LOG) == 3
    ok &= [d.info(n).name for n in range(1, 9)] == want and not any(d.info(n).stored for n in range(1, 9))
    d.select(2); ok &= d.get(P_NUMBER, 0) == 36 and d.get(P_CHANNEL) == 10
    ok &= d.get(gp.P_PRESET_COUNT) == 3 and [d.info(n).in_use for n in range(1, 9)] == [True] * 3 + [False] * 5
    d.select(1)
    check(ok, f"{tag}: defaults, YAML presets, slot 1 active, nothing stored")
all_yaml("after FACTORY_RESET")
d.reboot(); all_yaml("after FACTORY_RESET + reboot")

print("-- presets in use, PRESET_DELETE")
check(d.status(gp.SET_PARAM, gp.req_set_param(P_ACTIVE, 0, 4)) == gp.BAD_VALUE, "selecting free slot 4 -> BAD_VALUE")
d.ok(gp.PRESET_NAME, gp.req_preset_name(4, "Lead"))
check(d.info(4).in_use and d.get(gp.P_PRESET_COUNT) == 4, "naming free slot 4 makes it a preset")
d.select(2)
_, u, _ = d.ok(gp.PRESET_DELETE, gp.req_preset_delete(2))
check([d.info(n).name for n in (1, 2, 3)] == ["Default", "DAW", "Lead"] and not d.info(4).in_use and
      d.get(gp.P_PRESET_COUNT) == 3, "PRESET_DELETE 2: DAW and Lead move up, slot 4 is free")
check(d.get(P_ACTIVE) == 2 and [x.slot for x in changed(u)] == [2], "DAW, now in slot 2, is active")
d.select(3); check(d.get(P_NUMBER, 0) == 64 and d.get(P_CHANNEL) == 1, "slot 3: Lead, a copy of Default")
d.reboot()
check([d.info(n).name for n in (1, 2, 3)] == ["Default", "DAW", "Lead"] and d.get(gp.P_PRESET_COUNT) == 3 and
      d.get(P_ACTIVE) == 2, "the moves, the count and the active slot the delete stored survive a reboot")
check(d.status(gp.PRESET_DELETE, gp.req_preset_delete(4)) == gp.BAD_PARAM, "PRESET_DELETE a free slot -> BAD_PARAM")
d.select(3)                                            # Lead (a copy of Default)
_, u, _ = d.ok(gp.PRESET_MOVE, gp.req_preset_move(3, 1))
check([d.info(n).name for n in (1, 2, 3)] == ["Lead", "Default", "DAW"], "PRESET_MOVE 3 -> 1: Lead first, the others shift down")
check(d.get(P_ACTIVE) == 1 and [x.slot for x in changed(u)] == [1] and
      [(x.id, x.value) for x in param_changed(u)] == [(P_ACTIVE, 1)], "the active Lead follows to slot 1 (PARAM_CHANGED)")
d.select(3); check(d.get(P_NUMBER, 0) == 66, "slot 3: DAW's values (sustain CC 66)")
d.select(2); check(d.get(P_NUMBER, 0) == 64 and d.get(P_CHANNEL) == 1, "slot 2: Default's values")
d.select(1)
d.ok(gp.PRESET_MOVE, gp.req_preset_move(1, 3))
check([d.info(n).name for n in (1, 2, 3)] == ["Default", "DAW", "Lead"] and d.get(P_ACTIVE) == 3,
      "PRESET_MOVE 1 -> 3: back in order, the active Lead in slot 3")
d.reboot()
check([d.info(n).name for n in (1, 2, 3)] == ["Default", "DAW", "Lead"] and d.get(P_ACTIVE) == 3 and
      d.get(gp.P_PRESET_COUNT) == 3, "the order and the active slot survive a reboot")
d.select(2); check(d.get(P_NUMBER, 0) == 66, "slot 2 is DAW again after the reboot")
check(d.status(gp.PRESET_MOVE, gp.req_preset_move(4, 1)) == gp.BAD_PARAM, "PRESET_MOVE a free slot -> BAD_PARAM")
d.ok(gp.FACTORY_RESET)
d.ok(gp.PRESET_NAME, gp.req_preset_name(8, ""))        # every slot in use for the rest of the script
check(d.get(gp.P_PRESET_COUNT) == 8, "naming slot 8 puts slots 4..8 in use too")

# ---- 6. preset buttons, held notes, Program Change -----------------------
print("-- preset_next / preset_prev buttons")
d.set(P_ACTION, MOD, ACT_PRESET_NEXT); d.commit()      # slot 1: mod = preset next
d.select(2); d.set(P_ACTION, MOD, ACT_PRESET_NEXT); d.commit()
d.select(3); d.set(P_ACTION, MOD, ACT_PRESET_PREV); d.set(P_PROGRAM, 0, 5); d.commit()
check(d.info(3).program == 5, "PRESET_INFO 3: program 5")
d.select(1)
_, u, c = d.select(3)
check(bytes([0xC0, 5]) in c, f"select 3 sends Program Change 5 on channel 1: {c}")
d.select(1)
check(d.info(3).program == 5 and d.info(1).program == -1, "PRESET_INFO program of an inactive slot")
_, _, c = d.key(4, 1); check(notes(c)[:1] and notes(c)[0][:2] == bytes([0x90, 52]), "slot 1: key 4 -> Note On 52 ch 1")
_, u, c = d.press(MOD)
check([x.slot for x in changed(u)] == [2], f"mod (preset next): slot 1 -> 2 {changed(u)}")
check([(x.id, x.value) for x in param_changed(u)] == [(P_ACTIVE, 2)], "device switch sends PARAM_CHANGED preset.active = 2")
check(notes(c) == [bytes([0x80, 52, 0])], f"held key gets Note Off on the old channel: {c}")
check(not any(m[0] & 0xF0 == 0xC0 for m in c), "slot 2 has no program: no Program Change")
_, _, c = d.key(4, 0); check(not notes(c), "releasing it afterwards sends nothing more")
_, _, c = d.release(MOD); check(not c, "mod release: silent")
_, _, c = d.press(SUSTAIN); check(notes(c) == [bytes([0x99, 36, 100])], "slot 2: sustain holds note 36 ch 10")
_, u, c = d.press(MOD)
check([x.slot for x in changed(u)] == [3] and d.get(P_ACTIVE) == 3, "mod: slot 2 -> 3")
check(bytes([0x89, 36, 0]) in c and bytes([0xC0, 5]) in c and c.index(bytes([0x89, 36, 0])) < c.index(bytes([0xC0, 5])),
      f"held note button: Note Off ch 10, then Program Change 5: {c}")
_, _, c = d.release(SUSTAIN); check(not notes(c), "sustain released after the switch: no stray message")
d.release(MOD)
_, u, _ = d.press(MOD); d.release(MOD)
check([x.slot for x in changed(u)] == [2], "slot 3 mod = preset prev: 3 -> 2")
d.select(8); d.set(P_ACTION, MOD, ACT_PRESET_NEXT)
_, u, _ = d.press(MOD); d.release(MOD)
check([x.slot for x in changed(u)] == [1], "preset next wraps 8 -> 1 (unsaved edit, dropped)")
d.select(8); check(d.get(P_ACTION, MOD) == 3, "slot 8's unsaved mod action is gone")

# ---- 7. preset.active debounce -------------------------------------------
print("-- preset.active is stored after 10 s (write_to_flash: { last_preset: true })")
d.select(1); d.commit(); d.advance(11000); d.take()
p0 = d.programs
_, u, _ = d.press(MOD); d.release(MOD)
check(d.get(P_ACTIVE) == 2, "device switch to 2")
d.advance(9500); d.take()
check(d.programs == p0, "9.5 s later: no flash write")
d.reboot(); check(d.get(P_ACTIVE) == 1, "reboot before 10 s: slot 1 again")
_, u, _ = d.press(MOD); d.release(MOD)
d.advance(9800); check(d.programs == p0, "9.8 s: still nothing written")
d.advance(300); p1 = d.programs
check(p1 > p0, "10 s after the switch: preset.active written")
d.advance(15000); check(d.programs == p1, "written once only")
d.reboot(); check(d.get(P_ACTIVE) == 2 and d.get(P_ACTION, MOD) == ACT_PRESET_NEXT, "reboot: slot 2 active (stored by the debounce)")
d.select(2); d.advance(12000); check(d.programs == p1, "re-selecting the stored slot writes nothing")
d.select(3); d.select(2); d.advance(12000); check(d.programs == p1, "flipping back within 10 s writes nothing")
# write_to_flash.idle: not while a key is held, nor until 3 s after the last input
d.select(3); d.key(0, True); d.advance(15000); d.take()
check(d.programs == p1, "a key held through the delay: nothing written")
d.key(0, False); d.advance(2500); d.take(); check(d.programs == p1, "2.5 s after its release: still nothing")
d.advance(600); p2 = d.programs; check(p2 > p1, "idle for 3 s: preset.active written")
d.reboot(); check(d.get(P_ACTIVE) == 3, "reboot: slot 3 (stored once idle)")
p1 = p2
d.select(4); d.advance(5000); d.set(P_GLIDE, 0, 7); d.advance(5100)
check(d.programs > p1 and d.get(P_DIRTY) == 1, "the debounce write leaves unsaved edits alone (still dirty)")
d.reboot(); check(d.get(P_ACTIVE) == 4 and d.get(P_GLIDE) == 80, "reboot: slot 4, the unsaved edit not stored")
check(d.cnt[2] == 0 and d.cnt[3] == 0, "no word programmed twice, no bad address")

# ---- 8. many COMMITs, compaction ------------------------------------------
print("-- many COMMITs across slots (journal compaction)")
d.ok(gp.FACTORY_RESET)
d.ok(gp.PRESET_NAME, gp.req_preset_name(8, ""))        # every slot in use again
rng = random.Random(7)
PER = [(P_CHANNEL, 0, 1, 16), (P_GLIDE, 0, 0, 2000), (P_RAMP, 0, 0, 5000), (P_VEL_FIXED, 0, 1, 127),
       (P_ANALOG_CC, 0, 0, 127), (P_PROGRAM, 0, -1, 127)] + [(P_NUMBER, i, 0, 127) for i in range(7)]
def read_slot():
    return {(pid, i): d.get(pid, i) for pid, i, _, _ in PER}
model = {}
for n in range(1, 9):
    d.select(n); model[n] = read_slot()
d.select(1)
e0, last_bad = d.erases, None
glob_fast = 16
for it in range(400):
    n = rng.randint(1, 8)
    d.select(n)
    for pid, i, lo, hi in rng.sample(PER, rng.randint(1, 6)):
        v = rng.randint(lo, hi); d.set(pid, i, v); model[n][(pid, i)] = v
    if rng.random() < 0.2:
        glob_fast = rng.randint(1, 500); d.set(P_VEL_FAST, 0, glob_fast)
    d.commit()
    if it % 100 == 99:
        d.reboot()
        bad = [k for k in range(1, 9) if (d.select(k), read_slot())[1] != model[k]]
        if bad and last_bad is None:
            last_bad = (it, bad)
compactions = d.erases - e0
print(f"     {compactions} page erases over 400 COMMITs")
check(compactions >= 5, f"the journal was compacted several times ({compactions} erases)")
check(last_bad is None, f"every slot intact at each intermediate reboot {last_bad or ''}")
d.reboot()
check(d.get(P_ACTIVE) == n, f"after reboot: the last committed slot ({n}) is active")
check(d.get(P_VEL_FAST) == glob_fast, "the global survives")
bad = [k for k in range(1, 9) if (d.select(k), read_slot())[1] != model[k]]
check(not bad, f"after reboot: every slot's stored edits intact {bad}")
check(d.cnt[2] == 0 and d.cnt[3] == 0 and all(b == 0xFF for b in d.flash[DEVID - FLASH_BASE:]),
      "no word programmed twice, no bad address, the device-ID page untouched")

# ---- 9. errors ------------------------------------------------------------
print("-- errors")
d.select(3); p0, a0 = d.programs, d.get(P_ACTIVE)
S = d.status
check(S(gp.SET_PARAM, gp.req_set_param(P_ACTIVE, 0, 9)) == gp.BAD_VALUE, "preset.active = 9 -> BAD_VALUE")
check(S(gp.SET_PARAM, gp.req_set_param(P_ACTIVE, 0, 0)) == gp.BAD_VALUE, "preset.active = 0 -> BAD_VALUE")
check(S(gp.PRESET_INFO, gp.u14(9)) == gp.BAD_PARAM, "PRESET_INFO 9 -> BAD_PARAM")
check(S(gp.PRESET_SAVE, gp.u14(0)) == gp.BAD_PARAM, "PRESET_SAVE 0 -> BAD_PARAM")
check(S(gp.PRESET_SAVE, gp.u14(9)) == gp.BAD_PARAM, "PRESET_SAVE 9 -> BAD_PARAM")
check(S(gp.PRESET_RESET, gp.u14(9)) == gp.BAD_PARAM, "PRESET_RESET 9 -> BAD_PARAM")
check(S(gp.PRESET_NAME, gp.u14(0) + gp.enc_preset_name("x")) == gp.BAD_PARAM, "PRESET_NAME 0 -> BAD_PARAM")
check(S(gp.PRESET_NAME, gp.u14(9) + gp.enc_preset_name("x")) == gp.BAD_PARAM, "PRESET_NAME 9 -> BAD_PARAM")
check(S(gp.PRESET_NAME, gp.u14(2) + bytes([17]) + b"A" * 17) == gp.BAD_VALUE, "17-character name -> BAD_VALUE")
check(S(gp.PRESET_NAME, gp.u14(2) + bytes([3]) + b"a\x7fb") == gp.BAD_VALUE, "name with 0x7F -> BAD_VALUE")
check(S(gp.PRESET_NAME, gp.u14(2) + bytes([3]) + b"a\x1fb") == gp.BAD_VALUE, "name with 0x1F -> BAD_VALUE")
check(S(gp.PRESET_NAME, gp.u14(2) + gp.enc_preset_name("x" * 16)) == gp.OK and d.info(2).name == "x" * 16,
      "16-character name OK")
p0 = d.programs
check(S(gp.PRESET_NAME, gp.u14(2) + bytes([3]) + b"ab") == gp.BAD_LEN, "name len 3, 2 bytes -> BAD_LEN")
check(S(gp.PRESET_NAME, gp.u14(2) + bytes([1]) + b"ab") == gp.BAD_LEN, "name len 1, 2 bytes -> BAD_LEN")
check(S(gp.PRESET_NAME, gp.u14(2)) == gp.BAD_LEN, "PRESET_NAME without the length byte -> BAD_LEN")
for cmd in (gp.PRESET_INFO, gp.PRESET_SAVE, gp.PRESET_RESET):
    check(S(cmd, b"") == gp.BAD_LEN and S(cmd, gp.u14(1) + b"\x00") == gp.BAD_LEN and S(cmd, b"\x01") == gp.BAD_LEN,
          f"{gp.CMD_NAMES[cmd]} with 0, 1 or 3 bytes -> BAD_LEN")
check(S(gp.COMMIT, b"\x00") == gp.BAD_LEN and S(gp.FACTORY_RESET, b"\x00") == gp.BAD_LEN, "COMMIT / FACTORY_RESET with a payload -> BAD_LEN")
check(d.programs == p0 and d.get(P_ACTIVE) == a0 and d.info(2).name == "x" * 16,
      "rejected requests changed nothing")

# ---- 10. gemifi.py backup / restore against the real firmware ---------------
print("-- gemifi.py backup -> FACTORY_RESET -> restore")
class Port:
    """gemifi.Session transport on the simulated device."""
    def __init__(self, dev): self.d, self.q = dev, []
    def send(self, frame): self.d.feed(frame)
    def recv(self, timeout):
        self.d.advance(2)
        return [bytes(m) for _, m in self.d.take() if m[0] == 0xF0]
d.select(6); d.set(P_GLIDE, 0, 66); d.ok(gp.PRESET_NAME, gp.req_preset_name(6, "Six")); d.commit()
d.select(7); d.set(P_NUMBER, 3, 99); d.commit()
d.select(6); d.set(P_RAMP, 0, 4321)                  # unsaved edit on the active slot
before = {k: (d.select(k), read_slot(), d.info(k).name)[1:] for k in range(1, 9)}
d.select(6); d.set(P_RAMP, 0, 4321)
sess = gh.Session(Port(d), timeout=0.01)
hh = sess.hello(); sch = sess.get_schema()
doc = gh.backup_doc(sess, hh, sch)
check(d.get(P_ACTIVE) == 6 and d.get(P_DIRTY) == 1 and d.get(P_RAMP) == 4321, "backup leaves the device as it was (slot 6, dirty)")
class UserPort(Port):
    """... with a user who holds the unlock combo when the host asks the device for its prompt (UNLOCK)."""
    def __init__(self, dev): super().__init__(dev); self.hold = 0
    def send(self, frame):
        if gp.parse_frame(bytes(frame)).cmd == gp.UNLOCK:
            self.d.L.sim_button(PITCH_DOWN, 1); self.d.L.sim_button(PITCH_UP, 1); self.hold = 1100
        super().send(frame)
    def recv(self, timeout):
        out = super().recv(timeout)
        if self.hold:
            self.hold -= 1
            if not self.hold:
                self.d.L.sim_button(PITCH_DOWN, 0); self.d.L.sim_button(PITCH_UP, 0)
        return out
prompts = []
sess = gh.Session(UserPort(d), timeout=0.01, on_prompt=prompts.append)
sess.factory_reset()
check(d.journal_blank() and prompts and "Hold Pitch down + Pitch up on the device for 2 s" in prompts[0],
      f"gemifi.py factory reset: the unlock prompt, then done {prompts[:1]}")
d.reboot()
sess = gh.Session(Port(d), timeout=0.01)
n_set, n_err = gh.restore_presets(sess, sess.get_schema(), doc, warn=lambda *a: print("    ", *a))
check(n_err == 0, f"restore: {n_set} values set, no errors")
d.reboot()
after = {k: (d.select(k), read_slot(), d.info(k).name)[1:] for k in range(1, 9)}
diff = [k for k in range(1, 9) if (k != 6 and after[k] != before[k])]
check(not diff, f"restored presets match after a reboot {diff}")
check(after[6][1] == "Six" and after[6][0][(P_GLIDE, 0)] == 66, "slot 6 name and stored value restored")

# ---- 11. edit mode (devices/keystation-mini32-mk3.yaml modes:) ---------------
print("-- edit mode: Edit, E3, then a white key picks a preset")
E3, E4, B3 = 4, 16, 11                      # keys: C3 = key 0
d.ok(gp.FACTORY_RESET); d.take()
_, _, c = d.key(0, 1); check(notes(c) and notes(c)[0][:2] == bytes([0x90, 48]), "C3 plays")
_, _, c = d.press(EDIT); check(notes(c) == [bytes([0x80, 48, 0])] and not any(m[0] & 0xF0 == 0xB0 for m in c),
                               f"Edit: Note Off for the held key, no CC: {c}")
_, _, c = d.release(EDIT); check(not c, "Edit release: nothing")
_, _, c = d.key(0, 0); check(not notes(c), "the held key's release: nothing")
_, u, c = d.key(B3, 1); d.key(B3, 0); check(not c and not u, "unbound key: nothing")
_, u, c = d.key(E3, 1); d.key(E3, 0); check(not notes(c) and not changed(u), "E3: into the preset pick")
_, u, c = d.key(E4, 1); d.key(E4, 0)
check([x.slot for x in changed(u)] == [2] and d.get(P_ACTIVE) == 2 and not notes(c), f"E4: preset 2, no notes {changed(u)}")
check([(x.id, x.value) for x in param_changed(u)] == [(P_ACTIVE, 2)], "PARAM_CHANGED preset.active = 2")
d.press(EDIT); d.release(EDIT)
_, _, c = d.key(E4, 1); check(notes(c) and notes(c)[0][:2] == bytes([0x99, 64]), f"Edit again: E4 plays on ch 10: {c}")
d.key(E4, 0)
d.ok(gp.FACTORY_RESET); d.take()

print("FAILS:", fails); sys.exit(1 if fails else 0)
