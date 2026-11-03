# SPDX-License-Identifier: MIT
import pytest

from gemifi import protocol as P
from gemifi.device import normalise_role
from gemifi.mapping import Mapping, ON, OFF, BLINK, V14, to14, ENCODER_STEPS, JUMP_BEATS

from fakes import C, FakeDevice, FakeSong

B, A, E, L = P.KIND_BUTTON, P.KIND_ANALOG, P.KIND_ENCODER, P.KIND_LED


class Rig(object):
    def __init__(self, components=None, n_tracks=10):
        fake = FakeDevice(components)
        comps = []
        for lst in fake.by_kind.values():
            for c in lst:
                c.role = normalise_role(c.role)
                comps.append(c)
        self.comps = comps
        self.song = FakeSong(n_tracks)
        self.sent = []
        self.m = Mapping(self.song, lambda cmd, payload: self.sent.append((cmd, bytes(payload))))
        self.m.bind(comps)

    def take(self, cmd=None):
        got = [(c, p) for c, p in self.sent if cmd is None or c == cmd]
        self.sent = []
        return got

    def leds(self):
        """LED levels sent since the last take(), as {led: level} (FB_LED and FB_LEDS)."""
        out = {}
        for cmd, p in self.take():
            if cmd == P.FB_LED:
                out[P.dec_u14(p[0:2])] = p[2]
            elif cmd == P.FB_LEDS:
                first, n = P.dec_u14(p[0:2]), p[2]
                for i in range(n):
                    out[first + i] = p[3 + i]
        return out

    def values(self, cmds=(P.FB_VALUE,), src=None):
        out = {}
        for cmd, p in (self.take() if src is None else src):
            if cmd == P.FB_VALUE and cmd in cmds:
                out[(p[0], P.dec_u14(p[1:3]))] = P.dec_u14(p[3:5])
            elif cmd == P.FB_RING and cmd in cmds:
                out[("ring", P.dec_u14(p[0:2]))] = (p[2], P.dec_u14(p[3:5]), p[5])
        return out

    def press(self, index):
        self.m.handle_event(P.Event(B, index, 1))
        self.m.handle_event(P.Event(B, index, 0))

    def analog(self, index, value):
        self.m.handle_event(P.Event(A, index, value))

    def turn(self, index, delta):
        self.m.handle_event(P.Event(E, index, P.int_to_s14(delta)))


@pytest.fixture
def rig():
    r = Rig()
    r.m.refresh()
    r.first = r.take()          # what the first refresh sent
    return r


def test_first_refresh_sends_everything():
    r = Rig()
    r.m.refresh()
    leds = r.leds()
    # Button LEDs 0..6 and the metronome LED 7; LED 8 has no role, so it stays local.
    assert sorted(leds) == [0, 1, 2, 3, 4, 5, 6, 7]
    assert leds[3] == ON                      # bank_next possible with 10 tracks
    assert leds[6] == ON                      # select_1: track 1 is selected
    assert leds[0] == OFF
    r.m.refresh()
    assert r.take() == []                     # nothing changed, nothing sent


def test_contiguous_leds_go_as_fb_leds():
    r = Rig()
    r.m.refresh()
    cmds = [c for c, _ in r.take()]
    assert P.FB_LEDS in cmds


def test_unknown_roles_are_reported_not_bound():
    comps = [C(B, 0, "frobnicate", led=0), C(B, 1, "x_mine"), C(A, 0, "volume_9")]
    r = Rig(comps)
    assert sorted(role for _, _, role in r.m.unknown_roles) == ["frobnicate", "volume_9"]
    r.m.refresh()
    assert r.take() == []


def test_play_toggles_and_led_follows(rig):
    rig.press(0)
    assert rig.song.flags["playing"] is True
    assert rig.leds() == {0: ON}
    rig.press(0)
    assert rig.song.flags["playing"] is False
    assert rig.leds() == {0: OFF}
    # A change made in Live (mouse, keyboard) shows on the next tick.
    rig.song.flags["playing"] = True
    rig.m.tick()
    assert rig.leds() == {0: ON}


def test_stop_is_momentary(rig):
    rig.song.flags["playing"] = True
    rig.m.refresh()
    rig.take()
    rig.m.handle_event(P.Event(B, 1, 1))
    assert rig.song.flags["playing"] is False
    assert rig.leds() == {0: OFF, 1: ON}
    rig.m.handle_event(P.Event(B, 1, 0))
    assert rig.leds() == {1: OFF}


def test_record_blinks_while_waiting_for_play(rig):
    rig.press(2)
    assert rig.song.flags["record"] is True
    assert rig.leds() == {2: BLINK}
    rig.press(0)
    assert rig.leds() == {0: ON, 2: ON}


def test_led_component_role(rig):
    rig.song.flags["metronome"] = True
    rig.m.refresh()
    assert rig.leds() == {7: ON}


def test_volume_follows_analog_and_pickup_target_comes_back(rig):
    t1 = rig.song.tracks[0]
    vals = rig.values(src=rig.first)
    assert vals[(A, 0)] == to14(0.85)            # first refresh: the pick-up target
    # Far from the target: not picked up yet (no jump).
    rig.analog(0, 1000)
    assert t1.params["volume"].value == pytest.approx(0.85)
    # Crossing the target picks it up, then it follows.
    rig.analog(0, 15000)
    assert t1.params["volume"].value == pytest.approx(15000.0 / V14)
    rig.analog(0, 8192)
    assert t1.params["volume"].value == pytest.approx(8192.0 / V14)
    rig.m.refresh()
    assert (A, 0) not in rig.values()             # no echo of what the knob itself did
    # Live changes it: the new pick-up target goes to the device, and the knob must pick up again.
    t1.params["volume"].value = 0.25
    rig.m.refresh()
    assert rig.values()[(A, 0)] == to14(0.25)
    rig.analog(0, 9000)
    assert t1.params["volume"].value == pytest.approx(0.25)
    rig.analog(0, to14(0.25) + 100)                # within the window
    assert t1.params["volume"].value == pytest.approx((to14(0.25) + 100.0) / V14)


def test_analog_close_to_target_picks_up_at_once(rig):
    rig.analog(1, to14(0.85) - 50)                 # fader_2 -> volume_2
    assert rig.song.tracks[1].params["volume"].value == pytest.approx((to14(0.85) - 50.0) / V14)


def test_selected_track_volume_and_shared_target(rig):
    # track_volume (analog 2) and volume_1 (analog 0) both point at track 1's volume.
    rig.analog(2, to14(0.85))
    rig.analog(2, 4000)
    assert rig.song.tracks[0].params["volume"].value == pytest.approx(4000.0 / V14)
    rig.m.refresh()
    vals = rig.values()
    assert vals == {(A, 0): 4000}                  # the other control gets a new target
    rig.song.selected = rig.song.tracks[1]
    rig.m.refresh()
    vals = rig.values()
    assert vals[(A, 2)] == to14(0.85)


def test_device_param_encoder_ring(rig):
    params = rig.song.devices[0]
    p1 = params[0]                                 # device_param_1 = first param after "on"
    vals = rig.values((P.FB_VALUE, P.FB_RING), src=rig.first)
    assert vals[("ring", 0)] == (P.RING_WRAP, to14(p1.value), 0)
    assert vals[(E, 0)] == to14(p1.value)
    rig.turn(0, 5)
    assert p1.value == pytest.approx(0.0 + 5.0 / ENCODER_STEPS)
    vals = rig.values((P.FB_VALUE, P.FB_RING))
    assert vals[("ring", 0)] == (P.RING_WRAP, to14(5.0 / ENCODER_STEPS), 0)
    rig.turn(0, -10)                                 # clamps at 0
    assert p1.value == 0.0
    # Live moves the parameter: the ring follows.
    p1.value = 0.5
    rig.m.refresh()
    assert rig.values((P.FB_RING,))[("ring", 0)] == (P.RING_WRAP, 8192, 0)


def test_pan_ring_is_boost_cut_with_centre(rig):
    vals = rig.values((P.FB_RING,), src=rig.first)
    assert vals[("ring", 1)] == (P.RING_BOOST_CUT, 8192, 1)
    rig.turn(1, -64)
    assert rig.song.tracks[0].params["pan"].value == pytest.approx(0.0)


def test_tempo_encoder_has_no_ring(rig):
    before = rig.song.tempo.value
    vals = rig.values((P.FB_VALUE, P.FB_RING), src=rig.first)
    assert ("ring", 2) not in vals and (E, 2) in vals
    rig.turn(2, 1)
    assert rig.song.tempo.value > before


def test_device_bank_switch_and_device_change():
    comps = [C(E, 0, "device_param_1", ring=11), C(B, 0, "device_bank_next", led=0),
             C(B, 1, "device_bank_prev", led=1), C(B, 2, "device_next", led=2), C(B, 3, "device_on", led=3)]
    r = Rig(comps)
    r.m.refresh()
    assert r.leds() == {0: ON, 1: OFF, 2: ON, 3: ON}
    r.press(0)                                       # params 9..16 of the first device (11 of 12)
    assert r.m.device_bank == 1
    assert r.values((P.FB_RING,))[("ring", 0)] == (P.RING_WRAP, to14(8 / 20.0), 0)
    r.press(2)                                       # next device: bank back to 0
    r.m.refresh()
    assert r.m.device_bank == 0 and r.song.device_index == 1
    r.take()
    r.press(3)                                       # device_on toggles the on/off param
    assert r.song.device_on[1].value == 1.0


def test_bank_next_shifts_strips_and_refreshes_leds(rig):
    s = rig.song
    s.tracks[8].flags["arm"] = True                  # track 9 = strip 1 of the next bank
    rig.m.refresh()
    assert rig.leds() == {}                          # not in the current bank
    rig.press(3)                                     # bank_next (mcu bank_right)
    assert rig.m.bank_offset == 8
    leds = rig.leds()
    assert leds[4] == ON                             # arm_1 now shows track 9
    assert leds[6] == OFF                            # select_1: track 9 isn't selected
    assert leds[3] == OFF                            # no further bank (10 tracks)
    vals = rig.values()
    # Analog 2 (track_volume) has the same target as before, so only the strips refresh.
    assert vals == {}
    rig.press(4)                                     # arm_1 -> track 9
    assert s.tracks[8].flags["arm"] is False
    rig.analog(0, to14(0.85))                        # volume_1 -> track 9
    rig.analog(0, 100)
    assert s.tracks[8].params["volume"].value == pytest.approx(100.0 / V14)
    assert s.tracks[0].params["volume"].value == pytest.approx(0.85)


def test_bank_switch_sends_new_pickup_targets(rig):
    rig.song.tracks[8].params["volume"].value = 0.1
    rig.take()
    rig.press(3)
    vals = rig.values()
    assert vals[(A, 0)] == to14(0.1)                # volume_1 -> track 9
    assert vals[(A, 1)] == to14(0.85)               # volume_2 -> track 10
    # The value didn't change for volume_2 but the target did: the knob must pick up again.
    rig.analog(1, 0)
    assert rig.song.tracks[9].params["volume"].value == pytest.approx(0.85)


def test_strip_past_the_last_track_is_dark():
    comps = [C(B, 0, "arm_3", led=0), C(A, 0, "volume_3")]
    r = Rig(comps, n_tracks=2)
    r.m.refresh()
    assert r.take() == [(P.FB_LED, P.fb_led(0, OFF))]
    r.press(0)
    r.analog(0, 5)


def test_select_and_track_navigation():
    comps = [C(B, 0, "select_2", led=0), C(B, 1, "track_next", led=1), C(B, 2, "track_prev", led=2),
             C(B, 3, "track_arm", led=3), C(B, 4, "track_mute", led=4), C(B, 5, "solo_1", led=5)]
    r = Rig(comps, n_tracks=3)
    r.m.refresh()
    assert r.leds() == {0: OFF, 1: ON, 2: OFF, 3: OFF, 4: OFF, 5: OFF}
    r.press(0)
    assert r.song.selected is r.song.tracks[1]
    assert r.leds() == {0: ON, 2: ON}
    r.press(1)
    assert r.song.selected is r.song.tracks[2]
    assert r.leds() == {0: OFF, 1: OFF}
    r.press(3)
    r.press(4)
    assert r.song.tracks[2].flags["arm"] and r.song.tracks[2].flags["mute"]
    assert r.leds() == {3: ON, 4: ON}
    r.press(5)
    assert r.song.tracks[0].flags["solo"]


def test_transport_buttons():
    roles = ["loop", "metronome", "overdub", "session_record", "automation_arm", "tap_tempo", "undo", "redo",
             "stop_all_clips", "scene_launch", "scene_next", "scene_prev", "cycle", "click"]
    comps = [C(B, i, r) for i, r in enumerate(roles)]
    r = Rig(comps)
    for i in range(5):
        r.press(i)
    f = r.song.flags
    assert f["loop"] and f["metronome"] and f["overdub"] and f["session_record"] and f["automation_arm"]
    for i in range(5, 10):
        r.press(i)
    r.press(10)
    r.press(10)
    r.press(11)
    assert r.song.calls == ["tap", "undo", "redo", "stop_all", "fire0"]
    assert r.song.scene == 1
    r.press(12)                                      # cycle -> loop
    r.press(13)                                      # click -> metronome
    assert not f["loop"] and not f["metronome"]


def test_rewind_repeats_while_held(rig):
    rig.m.handle_event(P.Event(B, 7, 1))
    assert rig.song.jumps == [-JUMP_BEATS]
    for _ in range(4):
        rig.m.tick()
    assert len(rig.song.jumps) == 3                  # repeats start after the delay
    rig.m.handle_event(P.Event(B, 7, 0))
    rig.m.tick()
    assert len(rig.song.jumps) == 3


def test_master_section():
    comps = [C(A, 0, "master"), C(A, 1, "cue_volume"), C(E, 0, "crossfader", ring=11)]
    r = Rig(comps)
    r.m.refresh()
    vals = r.values((P.FB_VALUE, P.FB_RING))
    assert vals[("ring", 0)] == (P.RING_BOOST_CUT, 8192, 1)
    r.analog(0, to14(0.85))
    r.analog(0, 0)
    assert r.song.master["volume"].value == 0.0
    r.analog(1, to14(0.85))
    r.analog(1, V14)
    assert r.song.master["cue"].value == 1.0


def test_sends():
    comps = [C(E, 0, "send_a"), C(E, 1, "send_b"), C(E, 2, "track_pan")]
    r = Rig(comps)
    r.turn(0, 128)
    r.turn(1, 64)
    r.turn(2, 32)
    t = r.song.tracks[0]
    assert t.params["send_a"].value == 1.0 and t.params["send_b"].value == 0.5
    assert t.params["pan"].value == pytest.approx(0.75)


def test_keys_events_are_ignored(rig):
    rig.m.handle_event(P.Event(P.KIND_KEYS, 0, 100))
    rig.m.handle_event(P.Event(9, 0, 1))
    assert rig.take() == []


def test_release_forgets_so_everything_is_resent(rig):
    rig.m.release()
    rig.m.refresh()
    assert len(rig.leds()) == 8
