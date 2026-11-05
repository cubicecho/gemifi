# SPDX-License-Identifier: MIT
"""Pads and the clip grid (daw-scripts.md 5.9): pad parsing, EVENT 7, clip_T_S, scene_N, grid_*."""
import pytest

from gemifi import protocol as P
from gemifi import device as D
from gemifi.mapping import (BLINK, OFF, ON, RGB_NO_COLOUR, RGB_PLAYING, RGB_RECORDING, colour_rgb)
from gemifi.surface import GemifiSurface

from fakes import C, FakeCInstance, FakeClip, FakeDevice, FakeSong
from test_mapping import Rig

B, L, PAD = P.KIND_BUTTON, P.KIND_LED, P.KIND_PAD


def hx(s):
    return bytes.fromhex(s.replace(" ", ""))


def grid_components(layer=1, colours=3):
    """Like the Code 61: 16 base pads (no role, LEDs 0..15), then their twins in mode 0 (clip_T_S,
    T = column, S = row, LEDs 16..31); grid_left / right twins, grid_up / down and scene_1..4 buttons."""
    comps = [C(PAD, i, "", led=i, layer=0) for i in range(16)]
    comps += [C(PAD, 16 + i, "clip_%d_%d" % (i % 4 + 1, i // 4 + 1), led=16 + i, layer=layer) for i in range(16)]
    comps += [C(B, 0, "grid_left", led=32, layer=layer), C(B, 1, "grid_right", led=33, layer=layer),
              C(B, 2, "grid_up", led=34, layer=0), C(B, 3, "grid_down", led=35, layer=0)]
    comps += [C(B, 4 + n, "scene_%d" % (n + 1), led=36 + n, layer=0) for n in range(4)]
    comps += [C(L, i, "", colours=colours, owner=0, layer=0) for i in range(40)]
    return comps


def twin(t, s):
    """Pad index (and LED) of clip_T_S in grid_components()."""
    return 16 + (s - 1) * 4 + (t - 1)


class GridRig(Rig):
    def __init__(self, components=None, n_tracks=10, layers_on=(0,)):
        Rig.__init__(self, grid_components() if components is None else components, n_tracks)
        self.boxes = []
        self.m.highlight = self.boxes.append
        self.m.bind(self.comps, layers_on)

    def rgb(self):
        """FB_LED_RGB sent since the last take(), as {led: (r, g, b, flags)}."""
        return dict((P.dec_u14(p[0:2]), tuple(p[2:6])) for c, p in self.take(P.FB_LED_RGB))

    def hit(self, index, value=9000):
        self.m.handle_event(P.Event(PAD, index, value))
        self.m.handle_event(P.Event(PAD, index, 0))

    def slot(self, t, s):
        """The fake slot at track t, scene s (0-based, absolute)."""
        return self.song.tracks[t].slots[s]


@pytest.fixture
def grid():
    r = GridRig()
    r.m.refresh()
    r.first = r.take()
    return r


# ---- protocol ----

def test_pad_reply_vector_without_layer():
    # daw-scripts.md 4.4: pad 28 of 32, "pad_13_pads", role "clip_1_1", LED 52 (no layer byte)
    frame = hx("F0 7D 67 6D 01 51 51 24 00 00 07 1C 00 20 00 0B 70 61 64 5F 31 33 5F 70 61 64 73 06 50 61 64 20 "
               "31 33 08 63 6C 69 70 5F 31 5F 31 34 00 37 F7")
    c = P.parse_component_reply(P.parse_frame(frame).payload)
    assert (c.kind, c.index, c.count, c.id, c.name, c.role) == (PAD, 28, 32, "pad_13_pads", "Pad 13", "clip_1_1")
    assert c.led == 52 and c.has_led and c.layer is None and c.live(set())
    assert P.build_frame(P.GET_COMPONENT | P.REPLY, 0x51, P.build_component_reply(c)) == frame


def test_pad_reply_with_layer_and_more():
    c = P.Component(PAD, 20, 32, "pad_5_pads", "Pad 5", "clip_1_2", led=20, layer=1)
    got = P.parse_component_reply(P.build_component_reply(c))
    assert (got.led, got.layer, got.mode_index) == (20, 1, 0)
    assert not got.live(set()) and got.live({0})
    more = P.parse_component_reply(P.build_component_reply(c) + hx("05 06"))    # later fields ignored
    assert (more.led, more.layer) == (20, 1)
    none = P.parse_component_reply(P.build_component_reply(P.Component(PAD, 0, 1, "p", "", "", layer=0)))
    assert none.led == P.NONE_INDEX and not none.has_led
    with pytest.raises(P.ProtocolError):
        P.parse_component_reply(P.build_component_reply(c)[:-3])               # no led


def test_pad_event_vector():
    ev = P.parse_event(P.parse_frame(hx("F0 7D 67 6D 01 61 0C 09 00 07 1C 00 28 46 70 2E 00 00 26 F7")).payload)
    assert (ev.type, ev.index, ev.value) == (P.EVENT_PAD, 28, 9000)


def test_device_asks_for_pads():
    fake = FakeDevice(grid_components())
    dev = D.Device(fake.wire)
    fake.out = dev.receive
    dev.start()
    assert dev.ready
    pads = dev.components[PAD]
    assert len(pads) == 32 and pads[16].role == "clip_1_1" and pads[16].layer == 1
    assert sum(1 for c in dev.all_components() if c.kind == PAD) == 32
    old = FakeDevice()                          # firmware from before pads: BAD_PARAM for kind 7
    dev = D.Device(old.wire)
    old.out = dev.receive
    dev.start()
    assert dev.ready and dev.components[PAD] == []


# ---- clips ----

def test_first_refresh_turns_the_grid_off(grid):
    rgb = dict((P.dec_u14(p[0:2]), tuple(p[2:6])) for c, p in grid.first if c == P.FB_LED_RGB)
    assert sorted(rgb) == list(range(16, 32))           # the twins' LEDs; base pads have no role
    assert set(rgb.values()) == {(0, 0, 0, 0)}
    grid.m.refresh()
    assert grid.take() == []                            # change-only


def test_clip_led_colours_and_blink(grid):
    grid.slot(0, 0).clip = FakeClip(0x3366FF)
    grid.slot(1, 0).clip = FakeClip(None)
    rec = grid.slot(2, 0).clip = FakeClip(0x3366FF)
    rec.playing = rec.recording = True
    grid.m.tick()
    assert grid.rgb() == {twin(1, 1): (0x19, 0x33, 0x7F, 0), twin(2, 1): RGB_NO_COLOUR + (0,),
                          twin(3, 1): RGB_RECORDING + (0,)}
    grid.slot(0, 0).clip.triggered = True               # launch queued: blinks in its colour
    grid.m.tick()
    assert grid.rgb() == {twin(1, 1): (0x19, 0x33, 0x7F, 1)}


def test_pad_twin_launches_and_stops_a_clip(grid):
    slot = grid.slot(0, 0)
    slot.clip = FakeClip(0xFF0000)
    grid.m.tick()
    grid.take()
    grid.hit(twin(1, 1))
    assert slot.calls == ["fire"]
    assert grid.rgb() == {twin(1, 1): (127, 0, 0, 1)}  # queued: blink, at once (the press refreshes)
    slot.clip.triggered, slot.clip.playing = False, True
    grid.m.tick()
    assert grid.rgb() == {twin(1, 1): RGB_PLAYING + (0,)}
    grid.hit(twin(1, 1))                                # playing: a press stops it
    assert slot.calls == ["fire", "stop"]
    assert grid.rgb() == {twin(1, 1): RGB_PLAYING + (1,)}
    slot.clip.triggered, slot.clip.playing = False, False
    grid.m.tick()
    assert grid.rgb() == {twin(1, 1): (127, 0, 0, 0)}


def test_empty_slot_and_base_pad_do_nothing(grid):
    grid.hit(twin(2, 1))
    grid.hit(0)                                         # a base pad (no role) never sends EVENTs anyway
    assert all(s.calls == [] for t in grid.song.tracks for s in t.slots)
    assert grid.take() == []


def test_clip_role_on_a_button():
    r = GridRig([C(B, 0, "clip_2_3", led=0), C(L, 0, "", colours=1)])
    r.slot(1, 2).clip = FakeClip(0x00FF00)
    r.m.refresh()
    assert r.rgb() == {0: (0, 127, 0, 0)}               # a single-colour LED gets FB_LED_RGB too
    r.press(0)
    assert r.slot(1, 2).calls == ["fire"]


def test_unknown_grid_roles():
    r = GridRig([C(B, 0, "clip_9_1"), C(B, 1, "clip_1"), C(B, 2, "scene_9"), C(B, 3, "grid_home")])
    assert sorted(role for _, _, role in r.m.unknown_roles) == ["clip_1", "clip_9_1", "grid_home", "scene_9"]


# ---- scenes ----

def test_scene_n_launches_its_grid_row(grid):
    grid.press(4)                                       # scene_1
    assert [s.fired for s in grid.song.scenes] == [1, 0, 0, 0, 0, 0]
    grid.press(3)                                       # grid_down
    grid.press(7)                                       # scene_4: row 4 of the grid = scene 5
    assert [s.fired for s in grid.song.scenes] == [1, 0, 0, 0, 1, 0]


def test_scene_leds():
    r = GridRig()
    r.song.scenes = r.song.scenes[:3]
    r.song.scenes[1].triggered = True
    r.m.refresh()
    leds = r.leds()
    assert [leds[36 + n] for n in range(4)] == [ON, BLINK, ON, OFF]     # no 4th scene


# ---- scrolling ----

def test_grid_scrolls_and_clamps(grid):
    assert grid.m.grid_width == 4 and grid.m.grid_height == 4
    leds = dict((P.dec_u14(p[0:2]), p[2]) for c, p in grid.first if c == P.FB_LED)
    leds.update(dict((P.dec_u14(p[0:2]) + i, p[3 + i]) for c, p in grid.first if c == P.FB_LEDS
                     for i in range(p[2])))
    assert (leds[32], leds[33], leds[34], leds[35]) == (OFF, ON, OFF, ON)
    for _ in range(10):
        grid.press(1)                                   # grid_right: 10 tracks, 4 columns
    assert grid.m.grid_col == 6
    for _ in range(10):
        grid.press(3)                                   # grid_down: 6 scenes, 4 rows
    assert grid.m.grid_row == 2
    grid.take()
    grid.m.refresh()
    assert grid.take() == []
    grid.press(0)                                       # grid_left
    grid.press(2)                                       # grid_up
    assert (grid.m.grid_col, grid.m.grid_row) == (5, 1)
    for _ in range(10):
        grid.press(0)
        grid.press(2)
    assert (grid.m.grid_col, grid.m.grid_row) == (0, 0)


def test_scrolling_refreshes_only_changed_clip_leds(grid):
    grid.slot(1, 0).clip = FakeClip(0xFF0000)           # column 2, row 1 now
    grid.m.tick()
    assert grid.rgb() == {twin(2, 1): (127, 0, 0, 0)}
    grid.press(1)                                       # grid_right: that clip is in column 1 now
    assert grid.rgb() == {twin(1, 1): (127, 0, 0, 0), twin(2, 1): (0, 0, 0, 0)}
    assert grid.slot(1, 0).calls == []


def test_grid_pulls_back_when_tracks_go(grid):
    for _ in range(6):
        grid.press(1)
    assert grid.m.grid_col == 6
    grid.song.tracks = grid.song.tracks[:5]
    grid.m.tick()
    assert grid.m.grid_col == 1                         # ends at the last track
    grid.song.tracks = grid.song.tracks[:2]
    grid.m.tick()
    assert grid.m.grid_col == 0


def test_only_the_visible_grid_is_read(grid):
    for t in grid.song.tracks:
        t.slot_reads = []
    grid.press(1)                                       # col 1..4
    grid.press(3)                                       # row 1..4
    for t in grid.song.tracks:
        t.slot_reads = []
    grid.m.tick()
    reads = [(i, sorted(t.slot_reads)) for i, t in enumerate(grid.song.tracks) if t.slot_reads]
    assert reads == [(i, [1, 2, 3, 4]) for i in (1, 2, 3, 4)]         # each slot once per refresh


def test_same_clip_role_twice_reads_once():
    r = GridRig([C(B, 0, "clip_1_1", led=0), C(PAD, 0, "clip_1_1", led=1), C(L, 0, ""), C(L, 1, "")])
    r.song.tracks[0].slot_reads = []
    r.m.refresh()
    assert r.song.tracks[0].slot_reads == [0]
    assert sorted(r.rgb()) == [0, 1]                    # both LEDs


# ---- the session box ----

def test_grid_box_follows_layer_and_scroll(grid):
    assert grid.boxes == [(0, 0, 4, 4)]
    grid.press(1)
    assert grid.boxes[-1] == (1, 0, 4, 4)
    grid.m.handle_event(P.Event(P.EVENT_LAYER, 0, 0))   # the pad grid turned off: hide it
    assert grid.boxes[-1] is None
    grid.m.tick()
    assert len(grid.boxes) == 3                         # change-only
    grid.m.release()
    assert len(grid.boxes) == 3


def test_no_box_without_clip_roles():
    r = GridRig([C(B, 0, "grid_right", led=0), C(L, 0, "")])
    r.m.refresh()
    assert r.boxes == [] and r.m.grid_box() is None


# ---- colours ----

def test_colour_rgb():
    assert colour_rgb(0xFF8000) == (127, 64, 0)
    assert colour_rgb(0x010101) == (0, 0, 0)            # Live has no colours that dark
    assert colour_rgb(None) == RGB_NO_COLOUR and colour_rgb(0) == RGB_NO_COLOUR


# ---- end to end ----

def make_surface():
    # Few components: the fake device answers inside send_midi, so each one nests a call deeper.
    comps = [C(PAD, 0, "", led=0, layer=0), C(PAD, 1, "", led=1, layer=0),
             C(PAD, 2, "clip_1_1", led=2, layer=1), C(PAD, 3, "clip_2_1", led=3, layer=1)]
    comps += [C(L, i, "", colours=2, owner=0, layer=0) for i in range(4)]
    fake = FakeDevice(comps)
    fake.layers_on = {0}                                # the pad grid is on: replayed at connect
    c = FakeCInstance(fake)
    song = FakeSong()
    s = GemifiSurface(c, song=song)
    fake.out = s.receive_midi
    return s, fake, c, song


def test_end_to_end_pad_event_and_booted_resends():
    s, fake, c, song = make_surface()
    song.tracks[0].slots[0].clip = FakeClip(0xFF0000)
    s.update_display()
    assert c.highlights == [(0, 0, 2, 1, False)]
    rgb = dict((P.dec_u14(p[0:2]), tuple(p[2:6])) for cmd, p in fake.take(P.FB_LED_RGB))
    assert rgb == {2: (127, 0, 0, 0), 3: (0, 0, 0, 0)}
    fake.event(PAD, 2, 9000)
    fake.event(PAD, 2, 0)
    assert song.tracks[0].slots[0].calls == ["fire"]
    assert fake.take(P.FB_LED_RGB) == [(P.FB_LED_RGB, P.fb_led_rgb(2, 127, 0, 0, True))]
    s.update_display()
    assert fake.take(P.FB_LED_RGB) == []
    fake.boot()                                         # the device forgot its LED states
    rgb = dict((P.dec_u14(p[0:2]), tuple(p[2:6])) for cmd, p in fake.take(P.FB_LED_RGB))
    assert rgb == {2: (127, 0, 0, 1), 3: (0, 0, 0, 0)}
    s.disconnect()
    assert c.highlights[-1] == (-1, -1, -1, -1, False)
