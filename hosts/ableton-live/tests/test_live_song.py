# SPDX-License-Identifier: MIT
"""live_song.py against stand-ins shaped like the documented LOM (not Live itself)."""
import pytest

from gemifi.live_song import LiveSong, LiveParam, TEMPO_MIN, TEMPO_MAX
from gemifi.mapping import ENCODER_STEPS


class NS(object):
    def __init__(self, **kw):
        self.__dict__.update(kw)


class DeviceParameter(object):
    def __init__(self, value, lo=0.0, hi=1.0, quantized=False, enabled=True):
        self.value, self.min, self.max = value, lo, hi
        self.is_quantized, self.is_enabled = quantized, enabled


class ReadOnly(object):
    """The master track: no mute / solo / arm."""


def track(name):
    mixer = NS(volume=DeviceParameter(0.85), panning=DeviceParameter(0.0, -1.0, 1.0),
               sends=[DeviceParameter(0.0), DeviceParameter(0.2)])
    devices = [NS(name="dev%d" % i, parameters=[DeviceParameter(1.0)] +
                  [DeviceParameter(0.1 * k) for k in range(10)]) for i in range(2)]
    t = NS(name=name, mute=False, solo=False, arm=False, can_be_armed=True, mixer_device=mixer,
           devices=devices)
    t.view = NS(selected_device=devices[0])
    return t


class SongView(object):
    def __init__(self, song):
        self.song = song
        self.selected_track = song.tracks[0]
        self.selected_scene = song.scenes[0]

    def select_device(self, device):
        self.selected_track.view.selected_device = device


class Song(object):
    def __init__(self):
        self.tracks = [track("t%d" % i) for i in range(3)]
        self.visible_tracks = self.tracks
        self.return_tracks = [track("A")]
        self.master_track = NS(mixer_device=NS(volume=DeviceParameter(0.85), cue_volume=DeviceParameter(0.5),
                                               crossfader=DeviceParameter(0.0, -1.0, 1.0)))
        self.scenes = [NS(fired=0) for _ in range(3)]
        for sc in self.scenes:
            sc.fire = (lambda s: lambda: setattr(s, "fired", s.fired + 1))(sc)
        self.is_playing = False
        self.record_mode = False
        self.tempo = 120.0
        self.view = SongView(self)
        self.jumped = []

    def start_playing(self):
        self.is_playing = True

    def stop_playing(self):
        self.is_playing = False

    def jump_by(self, beats):
        self.jumped.append(beats)


def test_transport():
    s = Song()
    ls = LiveSong(s)
    ls.set_flag("playing", True)
    assert s.is_playing and ls.get_flag("playing")
    ls.set_flag("record", True)
    assert s.record_mode
    assert ls.get_flag("loop") is False           # missing property -> False, no exception
    ls.set_flag("loop", True)                     # plain attribute set on the stand-in
    ls.tap_tempo()                                # missing method -> nothing
    ls.jump(-4.0)
    assert s.jumped == [-4.0]


def test_param_scaling_and_quantized():
    p = DeviceParameter(0.0, -1.0, 1.0)
    lp = LiveParam(p)
    assert lp.bipolar and lp.get_norm() == 0.5
    lp.set_norm(1.0)
    assert p.value == 1.0
    q = DeviceParameter(2.0, 0.0, 5.0, quantized=True)
    lq = LiveParam(q)
    lq.nudge(37)                                  # one step per detent
    assert q.value == 3.0
    lq.set_norm(0.5)
    assert q.value in (2.0, 3.0)
    off = DeviceParameter(0.3, enabled=False)
    LiveParam(off).set_norm(1.0)
    assert off.value == 0.3


def test_tracks_and_mixer():
    s = Song()
    ls = LiveSong(s)
    tracks = ls.bank_tracks()
    assert len(tracks) == 3
    assert len(ls.nav_tracks()) == 5              # 3 + 1 return + master
    sel = ls.selected_track()
    assert sel.same(tracks[0]) and not sel.same(tracks[1])
    ls.select_track(tracks[2])
    assert s.view.selected_track is s.tracks[2]
    t = ls.bank_tracks()[0]
    t.set_flag("mute", True)
    assert s.tracks[0].mute
    t.param("volume").set_norm(0.0)
    assert s.tracks[0].mixer_device.volume.value == 0.0
    assert t.param("pan").bipolar
    assert t.param("send_b").get_norm() == pytest.approx(0.2)
    s.tracks[0].can_be_armed = False
    t.set_flag("arm", True)
    assert not s.tracks[0].arm
    assert ls.master_param("cue").get_norm() == 0.5
    assert ls.master_param("crossfader").bipolar


def test_master_track_without_flags():
    s = Song()
    s.view.selected_track = s.master_track
    ls = LiveSong(s)
    t = ls.selected_track()
    assert t.get_flag("mute") is False
    assert t.param("volume") is not None


def test_device():
    s = Song()
    ls = LiveSong(s)
    params = ls.device_params()
    assert len(params) == 10                      # parameters[0] (on/off) left out
    assert ls.device_on_param().get_norm() == 1.0
    assert ls.can_device_step(1) and not ls.can_device_step(-1)
    k = ls.device_key()
    ls.device_step(1)
    assert s.tracks[0].view.selected_device is s.tracks[0].devices[1]
    assert ls.device_key() != k


def test_scenes():
    s = Song()
    ls = LiveSong(s)
    assert ls.can_scene_step(1) and not ls.can_scene_step(-1)
    ls.scene_step(1)
    assert s.view.selected_scene is s.scenes[1]
    ls.fire_selected_scene()
    assert s.scenes[1].fired == 1


def test_tempo():
    s = Song()
    ls = LiveSong(s)
    tp = ls.tempo_param()
    tp.nudge(5)
    assert s.tempo == 125.0
    tp.nudge(-10000)
    assert s.tempo == TEMPO_MIN
    tp.set_norm(1.0)
    assert s.tempo == TEMPO_MAX


def test_continuous_nudge_uses_encoder_steps():
    p = DeviceParameter(0.0)
    LiveParam(p).nudge(ENCODER_STEPS // 2)
    assert p.value == pytest.approx(0.5)
