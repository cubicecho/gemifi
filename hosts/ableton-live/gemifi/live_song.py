# SPDX-License-Identifier: MIT
"""The abstract song of mapping.py on top of Live's Live Object Model.

Written against the public LOM (as documented for Max for Live): Song, Song.View, Track,
Track.View, MixerDevice, Device, DeviceParameter, Scene. Only property reads/writes and
documented methods are used; no listeners (mapping.py polls). Every call is guarded so a
property missing in some Live version, or a track kind without it (the master has no
mute/solo/arm), degrades to "nothing happens" instead of an exception inside Live.
"""

from .mapping import Param, Track, Song

TEMPO_MIN, TEMPO_MAX = 20.0, 999.0      # Live's tempo range
TEMPO_STEP = 1.0                        # BPM per encoder unit


def _get(obj, name, default=None):
    try:
        return getattr(obj, name)
    except Exception:
        return default


def _set(obj, name, value):
    try:
        setattr(obj, name, value)
        return True
    except Exception:
        return False


def _call(obj, name, *args):
    try:
        fn = getattr(obj, name)
    except Exception:
        return None
    try:
        return fn(*args)
    except Exception:
        return None


class LiveParam(Param):
    """A DeviceParameter (also what MixerDevice.volume / panning / sends are)."""

    def __init__(self, p):
        self._p = p
        lo, hi = _get(p, "min", 0.0), _get(p, "max", 1.0)
        self._min, self._max = float(lo), float(hi)
        self.bipolar = self._min < 0.0 < self._max and abs(self._min + self._max) < 1e-6

    @property
    def key(self):
        return lom_key(self._p)

    def _span(self):
        return self._max - self._min

    def get_norm(self):
        v = _get(self._p, "value", self._min)
        span = self._span()
        return (float(v) - self._min) / span if span > 0 else 0.0

    def set_norm(self, norm):
        if not _get(self._p, "is_enabled", True):
            return
        norm = min(1.0, max(0.0, norm))
        v = self._min + norm * self._span()
        if _get(self._p, "is_quantized", False):
            v = float(round(v))
        _set(self._p, "value", min(self._max, max(self._min, v)))

    def nudge(self, delta):
        if _get(self._p, "is_quantized", False):
            # One step per detent, whatever the acceleration says.
            if not _get(self._p, "is_enabled", True):
                return
            v = float(_get(self._p, "value", self._min)) + (1.0 if delta > 0 else -1.0)
            _set(self._p, "value", min(self._max, max(self._min, v)))
            return
        Param.nudge(self, delta)


class TempoParam(Param):
    def __init__(self, song):
        self._song = song

    @property
    def key(self):
        return "tempo"

    def get_norm(self):
        t = float(_get(self._song, "tempo", 120.0))
        return (t - TEMPO_MIN) / (TEMPO_MAX - TEMPO_MIN)

    def set_norm(self, norm):
        norm = min(1.0, max(0.0, norm))
        _set(self._song, "tempo", TEMPO_MIN + norm * (TEMPO_MAX - TEMPO_MIN))

    def nudge(self, delta):
        t = float(_get(self._song, "tempo", 120.0)) + delta * TEMPO_STEP
        _set(self._song, "tempo", min(TEMPO_MAX, max(TEMPO_MIN, t)))


def lom_key(obj):
    """A stable identity for a LOM object.

    UNCERTAIN (needs real Live): whether Live hands out the same Python wrapper for the same
    object on every access. If it does, id() is stable. LOM objects are believed to carry a
    `_live_ptr` (the native object's address), which is stable either way, so prefer it.
    """
    if obj is None:
        return None
    ptr = _get(obj, "_live_ptr")
    return ptr if ptr is not None else id(obj)


def _same(a, b):
    if a is None or b is None:
        return False
    if a is b:
        return True
    try:
        return a == b
    except Exception:
        return False


def _wrap(p):
    return LiveParam(p) if p is not None else None


class _LazyList(object):
    """A read-only view of a LOM list (from index `start`) that wraps an item only when it is
    used. The mapping reads 8 strips or 8 device parameters out of lists that can be far longer,
    and every wrapper costs LOM reads. The length is taken once, like list() did."""

    __slots__ = ("_raw", "_wrap", "_start", "_n", "_items")

    def __init__(self, raw, wrap, start=0):
        try:
            n = len(raw)
        except Exception:
            raw = tuple(raw or ())
            n = len(raw)
        self._raw, self._wrap, self._start = raw, wrap, start
        self._n = max(0, n - start)
        self._items = {}

    def __len__(self):
        return self._n

    def __getitem__(self, i):
        if isinstance(i, slice):
            return [self[j] for j in range(*i.indices(self._n))]
        if i < 0:
            i += self._n
        if not 0 <= i < self._n:
            raise IndexError(i)
        try:
            return self._items[i]
        except KeyError:
            item = self._items[i] = self._wrap(self._raw[self._start + i])
            return item

    def __iter__(self):
        for i in range(self._n):
            yield self[i]


class LiveTrack(Track):
    def __init__(self, t):
        self.t = t

    @property
    def key(self):
        return lom_key(self.t)

    def same(self, other):
        return other is not None and _same(self.t, getattr(other, "t", None))

    def get_flag(self, name):
        return bool(_get(self.t, name, False))

    def set_flag(self, name, on):
        if name == "arm" and not self.can_arm():
            return
        _set(self.t, name, bool(on))

    def can_arm(self):
        return bool(_get(self.t, "can_be_armed", False))

    def param(self, name):
        mixer = _get(self.t, "mixer_device")
        if mixer is None:
            return None
        if name == "volume":
            return _wrap(_get(mixer, "volume"))
        if name == "pan":
            return _wrap(_get(mixer, "panning"))
        if name in ("send_a", "send_b"):
            sends = list(_get(mixer, "sends", ()) or ())
            i = 0 if name == "send_a" else 1
            return _wrap(sends[i]) if i < len(sends) else None
        return None


class LiveSong(Song):
    # mapping flag -> Song property
    _FLAG_PROPS = {"record": "record_mode", "loop": "loop", "metronome": "metronome",
                   "overdub": "arrangement_overdub", "session_record": "session_record",
                   "automation_arm": "session_automation_record"}

    def __init__(self, song):
        self.song = song

    def _track(self, t):
        return LiveTrack(t) if t is not None else None

    # ---- transport ----

    def get_flag(self, name):
        if name == "playing":
            return bool(_get(self.song, "is_playing", False))
        prop = self._FLAG_PROPS.get(name)
        return bool(_get(self.song, prop, False)) if prop else False

    def set_flag(self, name, on):
        if name == "playing":
            _call(self.song, "start_playing" if on else "stop_playing")
            return
        prop = self._FLAG_PROPS.get(name)
        if prop:
            _set(self.song, prop, bool(on))

    def tap_tempo(self):
        _call(self.song, "tap_tempo")

    def undo(self):
        if _get(self.song, "can_undo", True):
            _call(self.song, "undo")

    def redo(self):
        if _get(self.song, "can_redo", True):
            _call(self.song, "redo")

    def jump(self, beats):
        _call(self.song, "jump_by", beats)

    def stop_all_clips(self):
        _call(self.song, "stop_all_clips")

    def tempo_param(self):
        return TempoParam(self.song)

    # ---- tracks ----

    def bank_tracks(self):
        return _LazyList(_get(self.song, "tracks", ()) or (), self._track)

    def nav_tracks(self):
        visible = _get(self.song, "visible_tracks", None)
        if visible is None:
            visible = _get(self.song, "tracks", ()) or ()
        out = list(visible) + list(_get(self.song, "return_tracks", ()) or ())
        master = _get(self.song, "master_track")
        if master is not None:
            out.append(master)
        return [self._track(t) for t in out]

    def selected_track(self):
        view = _get(self.song, "view")
        return self._track(_get(view, "selected_track")) if view is not None else None

    def select_track(self, track):
        view = _get(self.song, "view")
        if view is not None and track is not None:
            _set(view, "selected_track", track.t)

    def master_param(self, name):
        master = _get(self.song, "master_track")
        mixer = _get(master, "mixer_device") if master is not None else None
        if mixer is None:
            return None
        prop = {"volume": "volume", "cue": "cue_volume", "crossfader": "crossfader"}.get(name)
        return _wrap(_get(mixer, prop)) if prop else None

    # ---- selected device ----

    def _selected_device(self):
        view = _get(self.song, "view")
        track = _get(view, "selected_track") if view is not None else None
        tview = _get(track, "view") if track is not None else None
        return _get(tview, "selected_device") if tview is not None else None

    def device_key(self):
        d = self._selected_device()
        return lom_key(d)

    def _params(self, device):
        return (_get(device, "parameters", ()) or ()) if device is not None else ()

    def device_params(self):
        # parameters[0] is the device's on/off switch ("Device On") in the LOM.
        return _LazyList(self._params(self._selected_device()), LiveParam, 1)

    def device_on_param(self):
        ps = _LazyList(self._params(self._selected_device()), LiveParam)
        return ps[0] if len(ps) else None

    def _device_list(self):
        view = _get(self.song, "view")
        track = _get(view, "selected_track") if view is not None else None
        return list(_get(track, "devices", ()) or ()) if track is not None else []

    def _device_pos(self):
        devices = self._device_list()
        d = self._selected_device()
        for i, x in enumerate(devices):
            if _same(x, d):
                return i, devices
        return None, devices

    def can_device_step(self, delta):
        i, devices = self._device_pos()
        if i is None:
            return bool(devices) and delta > 0
        return 0 <= i + delta < len(devices)

    def device_step(self, delta):
        i, devices = self._device_pos()
        if not devices:
            return
        j = 0 if i is None else i + delta
        if 0 <= j < len(devices):
            _call(_get(self.song, "view"), "select_device", devices[j])

    # ---- session ----

    def _scene_pos(self):
        scenes = list(_get(self.song, "scenes", ()) or ())
        view = _get(self.song, "view")
        cur = _get(view, "selected_scene") if view is not None else None
        for i, s in enumerate(scenes):
            if _same(s, cur):
                return i, scenes
        return None, scenes

    def can_scene_step(self, delta):
        i, scenes = self._scene_pos()
        return i is not None and 0 <= i + delta < len(scenes)

    def scene_step(self, delta):
        i, scenes = self._scene_pos()
        if i is not None and 0 <= i + delta < len(scenes):
            _set(_get(self.song, "view"), "selected_scene", scenes[i + delta])

    def fire_selected_scene(self):
        view = _get(self.song, "view")
        scene = _get(view, "selected_scene") if view is not None else None
        if scene is not None:
            _call(scene, "fire")
