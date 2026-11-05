# SPDX-License-Identifier: MIT
"""Roles -> behaviour (hosts/README.md, "Roles").

Pure Python, no Live imports. The mapping talks to an abstract song (`Song`, `Track`, `Param`
below); live_song.py adapts the real Live Object Model to it and the tests use a fake one.

Inputs: EVENTs from the device (protocol.Event), by component index.
Outputs: FB_* notifications through `notify(cmd, payload)`.

Layers (daw-scripts.md 5.11): a twin (`layer` > 0) is live only while its mode is on.
Each component gets its own binding, so two components with one role (device_param_1 in two
modes) both drive it, and feedback goes to both. The device only sends EVENTs from live ones;
the mapping uses liveness only to page device parameters by the live device_param_N, and only
when the firmware reports layers (else pages of 8, as before), and to show the clip grid's box
only while a clip_T_S is live.

Clip grid (daw-scripts.md 5.9): pads (kind 7) bind like buttons; only pad twins send EVENTs
(type 7). clip_T_S LEDs get FB_LED_RGB. Only the grid's own slots are read, once per refresh.

State is polled, not listened to: refresh() (called every tick, ~100 ms) reads every bound
state, compares it with what was last sent, and sends only the differences. That keeps the
script free of per-track / per-parameter listener bookkeeping, and a lost notification is
fixed by the next change (sysex.md 4.6.2 says feedback is idempotent).
"""

from . import protocol as P

STRIPS = 8                    # mixer strips and device params per bank
V14 = P.U14_MAX               # 16383
ON, OFF, BLINK = 127, 0, 1    # FB_LED levels (sysex.md 4.6.2: 0 off, 1 blink, 2..127 on)
ENCODER_STEPS = 128           # encoder units for a parameter's full range
PICKUP_WINDOW = V14 * 3 // 100  # an analog within 3 % of the target picks it up
JUMP_BEATS = 4.0              # rewind / fast_forward step
REPEAT_DELAY_TICKS = 3        # held rewind / fast_forward repeats after ~300 ms, then every tick
HOST_NAMESPACE = "live."       # this script's <host>.<command> roles (none yet); other hosts' are ignored
GRID_MAX = 8                  # clip_T_S: T, S = 1..8 (daw-scripts.md 5.9)
GRID_MOVES = {"grid_left": (-1, 0), "grid_right": (1, 0), "grid_up": (0, -1), "grid_down": (0, 1)}

# Clip LEDs (FB_LED_RGB r, g, b, 0..127). A stopped clip shows its own colour.
RGB_OFF = (0, 0, 0)
RGB_PLAYING = (0, 127, 0)       # green, like Live's play triangle
RGB_RECORDING = (127, 0, 0)
RGB_NO_COLOUR = (0, 127, 127)    # a clip without a (visible) colour: teal, as in Bitwig


def colour_rgb(colour):
    """0xRRGGBB (Live's clip colour) -> (r, g, b) 0..127; RGB_NO_COLOUR if none or black."""
    if not colour:
        return RGB_NO_COLOUR
    c = int(colour)
    return ((c >> 17) & 0x7F, (c >> 9) & 0x7F, (c >> 1) & 0x7F)


def to14(norm):
    if norm <= 0.0:
        return 0
    if norm >= 1.0:
        return V14
    return int(round(norm * V14))


# ---------------------------------------------------------------------------
# The abstract song. Subclass and override; the defaults make missing features harmless.
# ---------------------------------------------------------------------------

class Param(object):
    """A continuous target. `key` identifies the underlying parameter (so the mapping notices
    when a control's target changes, e.g. after a bank switch)."""

    bipolar = False           # centre = neutral (pan): ring shows boost/cut with the centre LED

    @property
    def key(self):
        return id(self)

    def get_norm(self):
        raise NotImplementedError

    def set_norm(self, value):
        raise NotImplementedError

    def nudge(self, delta):
        """Encoder turn by `delta` units (signed)."""
        v = self.get_norm() + float(delta) / ENCODER_STEPS
        self.set_norm(min(1.0, max(0.0, v)))


class Track(object):
    @property
    def key(self):
        return id(self)

    def same(self, other):
        return other is not None and self.key == other.key

    def get_flag(self, name):
        """name: 'mute', 'solo', 'arm'."""
        return False

    def set_flag(self, name, on):
        pass

    def can_arm(self):
        return False

    def param(self, name):
        """name: 'volume', 'pan', 'send_a', 'send_b'. Returns a Param or None."""
        return None

    def clip_slot(self, scene):
        """The ClipSlot in scene row `scene` (0-based), or None."""
        return None


class ClipState(object):
    """What a clip slot's LED needs. colour: 0xRRGGBB or None. triggered: a launch or stop is queued."""

    __slots__ = ("colour", "playing", "recording", "triggered")

    def __init__(self, colour=None, playing=False, recording=False, triggered=False):
        self.colour, self.playing, self.recording, self.triggered = colour, playing, recording, triggered


class ClipSlot(object):
    def state(self):
        """A ClipState, or None for an empty slot."""
        return None

    def fire(self):
        pass

    def stop(self):
        pass


class Scene(object):
    def fire(self):
        pass

    def is_triggered(self):
        return False


class Song(object):
    TRANSPORT_FLAGS = ("playing", "record", "loop", "metronome", "overdub", "session_record",
                       "automation_arm")

    # transport
    def get_flag(self, name):
        return False

    def set_flag(self, name, on):
        pass

    def tap_tempo(self):
        pass

    def undo(self):
        pass

    def redo(self):
        pass

    def jump(self, beats):
        pass

    def stop_all_clips(self):
        pass

    def tempo_param(self):
        return None

    # tracks
    def bank_tracks(self):
        """The tracks the 8 mixer strips page through."""
        return []

    def nav_tracks(self):
        """The tracks track_prev / track_next step through (in Live: also returns and master)."""
        return self.bank_tracks()

    def selected_track(self):
        return None

    def select_track(self, track):
        pass

    def master_param(self, name):
        """name: 'volume', 'cue', 'crossfader'."""
        return None

    # selected device
    def device_key(self):
        """Identifies the selected device (None = none)."""
        return None

    def device_params(self):
        """The selected device's parameters, without its on/off switch."""
        return []

    def device_on_param(self):
        return None

    def can_device_step(self, delta):
        return False

    def device_step(self, delta):
        pass

    # session
    def can_scene_step(self, delta):
        return False

    def scene_step(self, delta):
        pass

    def fire_selected_scene(self):
        pass

    # clip grid (daw-scripts.md 5.9)
    def grid_tracks(self):
        """The tracks the grid's columns page through (in Live: the session view's visible tracks)."""
        return []

    def grid_scenes(self):
        """The scenes the grid's rows page through."""
        return []


class _Reads(object):
    """The song as the bindings see it. Inside refresh() each read below is done once and kept
    until the refresh ends: many bindings ask for the same tracks or device parameters, and in
    Live each of those is a walk over the LOM. Everything else, and every read outside
    refresh(), goes straight to the song."""

    CACHED = ("bank_tracks", "nav_tracks", "selected_track", "device_key", "device_params",
              "device_on_param", "can_device_step", "can_scene_step", "grid_tracks", "grid_scenes")

    def __init__(self, song):
        self._song = song
        self._memo = None

    def begin(self):
        self._memo = {}

    def end(self):
        self._memo = None

    def memo(self, key, fn):
        """fn(), kept under `key` until the refresh ends (not kept outside refresh())."""
        memo = self._memo
        if memo is None:
            return fn()
        try:
            return memo[key]
        except KeyError:
            value = memo[key] = fn()
            return value

    def __getattr__(self, name):
        return getattr(self._song, name)


def _cached_read(name):
    def read(self, *args):
        memo = self._memo
        if memo is None:
            return getattr(self._song, name)(*args)
        key = (name,) + args
        try:
            return memo[key]
        except KeyError:
            value = memo[key] = getattr(self._song, name)(*args)
            return value
    read.__name__ = name
    return read


for _name in _Reads.CACHED:
    setattr(_Reads, _name, _cached_read(_name))


# ---------------------------------------------------------------------------
# Bindings
# ---------------------------------------------------------------------------

class _ButtonRole(object):
    """press() / release() actions and state() -> LED level."""

    def __init__(self, press=None, release=None, state=None, momentary=False, repeat=None, rgb=None):
        self.press = press
        self.release = release
        self.state = state
        self.momentary = momentary      # LED lit while held
        self.repeat = repeat            # called every tick while held, after a delay
        self.rgb = rgb                  # () -> (r, g, b, blink): the LED gets FB_LED_RGB instead


class _ButtonBinding(object):
    def __init__(self, component, role):
        self.component = component
        self.role = role
        self.held = False
        self.held_ticks = 0

    def level(self):
        if self.role.momentary:
            return ON if self.held else OFF
        return self.role.state() if self.role.state is not None else OFF


class _LedBinding(object):
    def __init__(self, led, level_fn):
        self.led = led
        self.level = level_fn


class _ContBinding(object):
    def __init__(self, component, target):
        self.component = component
        self.target = target            # () -> Param or None
        self.picked = False
        self.last_raw = None


def _clip_role(role):
    """'clip_2_3' -> (2, 3): column T, row S, both 1..8. Else None."""
    parts = role.split("_")
    if len(parts) != 3 or parts[0] != "clip" or not parts[1].isdigit() or not parts[2].isdigit():
        return None
    t, s = int(parts[1]), int(parts[2])
    return (t, s) if 1 <= t <= GRID_MAX and 1 <= s <= GRID_MAX else None


def _is_grid_role(role):
    return (_clip_role(role) is not None or role in GRID_MOVES or
            _strip_role(role, ("scene",))[0] is not None)


def _strip_role(role, prefixes):
    """'volume_3' -> ('volume', 3) if the prefix is in `prefixes` and 1 <= N <= 8."""
    base, sep, num = role.rpartition("_")
    if not sep or base not in prefixes or not num.isdigit():
        return None, None
    n = int(num)
    if not 1 <= n <= STRIPS:
        return None, None
    return base, n


class Mapping(object):
    def __init__(self, song, notify, log=None, highlight=None):
        self.song = song
        self._reads = _Reads(song)    # what bindings read through (cached inside refresh())
        self.notify = notify
        self.log = log
        self.highlight = highlight    # (col, row, width, height) or None: show / hide the grid in the DAW
        self.bank_offset = 0
        self.device_offset = 0        # the first device parameter device_param_1 controls
        self.grid_col = 0             # the grid's scroll position: track and scene of clip_1_1
        self.grid_row = 0
        self.grid_width = 1           # the largest T and S of the bound clip_T_S / scene_N
        self.grid_height = 1
        self.layers_on = set()        # mode indexes whose layers are on (EVENT 8)
        self.reports_layers = False   # some component came with a `layer` byte (newer firmware)
        self._device_key = None
        self._buttons = {}
        self._pads = {}
        self._leds = {}
        self._rgb_leds = {}           # led -> () -> (r, g, b, blink): clip LEDs
        self._analogs = {}
        self._encoders = {}
        self._device_slots = []       # (N, component) of each device_param_N binding
        self._clip_comps = []         # components with a clip_T_S role
        self._has_grid = False        # some grid role is bound
        self._led_cache = {}
        self._rgb_cache = {}
        self._value_cache = {}
        self._ring_cache = {}
        self._box = None              # what highlight() was last given
        self.unknown_roles = []

    # ---- setup ----

    def bind(self, components, layers_on=()):
        """Build the bindings from the device's component list (roles already normalised).
        LEDs with owner: local ignore FB_LED, so they get no binding (sysex.md 4.6.1 `owner`).
        `layers_on`: the mode indexes whose layers are on (Device.layers_on)."""
        self._buttons, self._pads, self._leds, self._rgb_leds = {}, {}, {}, {}
        self._analogs, self._encoders = {}, {}
        self._device_slots = []
        self._clip_comps = []
        self.layers_on = set(layers_on)
        self.reports_layers = any(c.layer is not None for c in components)
        self.unknown_roles = []
        self.forget()
        local = set(c.index for c in components if c.kind == P.KIND_LED and c.local_only)
        grid = [c for c in components if c.role and c.kind in (P.KIND_BUTTON, P.KIND_PAD, P.KIND_LED) and
                not c.local_only and _is_grid_role(c.role)]
        self._has_grid = bool(grid)
        cols, rows = [1], [1]
        for c in grid:
            cs = _clip_role(c.role)
            if cs is not None:
                self._clip_comps.append(c)
                cols.append(cs[0])
                rows.append(cs[1])
            else:
                n = _strip_role(c.role, ("scene",))[1]
                if n is not None:
                    rows.append(n)
        self.grid_width, self.grid_height = max(cols), max(rows)
        for c in components:
            role = c.role
            if not role:
                continue
            if c.kind in (P.KIND_BUTTON, P.KIND_PAD):
                # Pads: only twins send EVENTs (daw-scripts.md 5.9), but a base pad's LED still shows its role.
                br = self._button_role(role)
                if br is None:
                    self._unknown(c)
                    continue
                b = _ButtonBinding(c, br)
                (self._buttons if c.kind == P.KIND_BUTTON else self._pads)[c.index] = b
                if c.has_led and c.led not in local:
                    # A button's or pad's own binding wins over an LED's (it knows "held").
                    self._leds.pop(c.led, None)
                    self._rgb_leds.pop(c.led, None)
                    if br.rgb is not None:
                        self._rgb_leds[c.led] = br.rgb
                    else:
                        self._leds[c.led] = _LedBinding(c.led, b.level)
            elif c.kind == P.KIND_LED:
                if c.local_only:
                    continue
                br = self._button_role(role)
                if br is None:
                    self._unknown(c)
                    continue
                if c.index in self._leds or c.index in self._rgb_leds:
                    continue
                if br.rgb is not None:
                    self._rgb_leds[c.index] = br.rgb
                else:
                    self._leds[c.index] = _LedBinding(c.index, (lambda r: (lambda: OFF if r.momentary or
                                                      r.state is None else r.state()))(br))
            elif c.kind in (P.KIND_ANALOG, P.KIND_ENCODER):
                target = self._cont_role(role)
                if target is None:
                    self._unknown(c)
                    continue
                table = self._analogs if c.kind == P.KIND_ANALOG else self._encoders
                table[c.index] = _ContBinding(c, target)
                base, n = _strip_role(role, ("device_param",))
                if base is not None:
                    self._device_slots.append((n, c))
            # keys and displays: not mapped

    def _unknown(self, c):
        # x_ roles and another host's commands (<host>.<command>, daw-scripts.md 6): ignored quietly.
        if c.role.startswith("x_") or ("." in c.role and not c.role.startswith(HOST_NAMESPACE)):
            return
        self.unknown_roles.append((c.kind, c.index, c.role))

    def forget(self):
        """Drop what was sent: the next refresh() sends everything (after ready / a release)."""
        self._led_cache, self._rgb_cache, self._value_cache, self._ring_cache = {}, {}, {}, {}
        for b in list(self._analogs.values()):
            b.picked, b.last_raw = False, None

    def release(self):
        """The device is being handed back (FB_RELEASE goes out from device.release())."""
        self.forget()
        for b in list(self._buttons.values()) + list(self._pads.values()):
            b.held = False
        self._show_box(None)

    # ---- role tables ----

    def _button_role(self, role):
        s = self._reads
        flag_roles = {"record": "record", "loop": "loop", "metronome": "metronome", "overdub": "overdub",
                      "session_record": "session_record", "automation_arm": "automation_arm"}
        if role == "play":
            return _ButtonRole(press=lambda: s.set_flag("playing", not s.get_flag("playing")),
                               state=lambda: ON if s.get_flag("playing") else OFF)
        if role == "stop":
            return _ButtonRole(press=lambda: s.set_flag("playing", False), momentary=True)
        if role == "record":
            def rec_state():
                if not s.get_flag("record"):
                    return OFF
                return ON if s.get_flag("playing") else BLINK     # armed, waiting for play
            return _ButtonRole(press=lambda: s.set_flag("record", not s.get_flag("record")), state=rec_state)
        if role in flag_roles:
            name = flag_roles[role]
            return _ButtonRole(press=lambda: s.set_flag(name, not s.get_flag(name)),
                               state=lambda: ON if s.get_flag(name) else OFF)
        if role == "tap_tempo":
            return _ButtonRole(press=s.tap_tempo, momentary=True)
        if role == "undo":
            return _ButtonRole(press=s.undo, momentary=True)
        if role == "redo":
            return _ButtonRole(press=s.redo, momentary=True)
        if role in ("rewind", "fast_forward"):
            beats = -JUMP_BEATS if role == "rewind" else JUMP_BEATS
            return _ButtonRole(press=lambda: s.jump(beats), repeat=lambda: s.jump(beats), momentary=True)

        # selected track
        if role in ("track_prev", "track_next"):
            d = -1 if role == "track_prev" else 1
            return _ButtonRole(press=lambda: self._track_step(d),
                               state=lambda: ON if self._can_track_step(d) else OFF)
        if role in ("track_mute", "track_solo", "track_arm"):
            return self._track_flag_role(role[len("track_"):], s.selected_track)

        # mixer strips
        if role in ("bank_prev", "bank_next"):
            d = -1 if role == "bank_prev" else 1
            return _ButtonRole(press=lambda: self.bank_step(d),
                               state=lambda: ON if self._can_bank_step(d) else OFF)
        if role in ("strip_prev", "strip_next"):
            d = -1 if role == "strip_prev" else 1
            return _ButtonRole(press=lambda: self.bank_step(d, 1),
                               state=lambda: ON if self._can_bank_step(d) else OFF)
        base, n = _strip_role(role, ("mute", "solo", "arm", "select"))
        if base is not None:
            strip = (lambda i: (lambda: self.strip_track(i)))(n - 1)
            if base == "select":
                def sel_press():
                    t = strip()
                    if t is not None:
                        s.select_track(t)

                def sel_state():
                    t, cur = strip(), s.selected_track()
                    return ON if t is not None and t.same(cur) else OFF
                return _ButtonRole(press=sel_press, state=sel_state)
            return self._track_flag_role(base, strip)

        # selected device
        if role in ("device_prev", "device_next"):
            d = -1 if role == "device_prev" else 1
            return _ButtonRole(press=lambda: s.device_step(d), state=lambda: ON if s.can_device_step(d) else OFF)
        if role in ("device_bank_prev", "device_bank_next"):
            d = -1 if role == "device_bank_prev" else 1
            return _ButtonRole(press=lambda: self.device_bank_step(d),
                               state=lambda: ON if self._can_device_bank_step(d) else OFF)
        if role == "device_on":
            def on_press():
                p = s.device_on_param()
                if p is not None:
                    p.set_norm(0.0 if p.get_norm() >= 0.5 else 1.0)

            def on_state():
                p = s.device_on_param()
                return ON if p is not None and p.get_norm() >= 0.5 else OFF
            return _ButtonRole(press=on_press, state=on_state)

        # session
        if role == "scene_launch":
            return _ButtonRole(press=s.fire_selected_scene, momentary=True)
        if role in ("scene_prev", "scene_next"):
            d = -1 if role == "scene_prev" else 1
            return _ButtonRole(press=lambda: s.scene_step(d), state=lambda: ON if s.can_scene_step(d) else OFF)
        if role == "stop_all_clips":
            return _ButtonRole(press=s.stop_all_clips, momentary=True)

        # clip grid (daw-scripts.md 5.9)
        cs = _clip_role(role)
        if cs is not None:
            t, row = cs[0] - 1, cs[1] - 1
            return _ButtonRole(press=lambda: self._clip_press(t, row), rgb=lambda: self._clip_rgb(t, row))
        base, n = _strip_role(role, ("scene",))
        if base is not None:
            def scene_press(i=n - 1):
                sc = self._grid_scene(i)
                if sc is not None:
                    sc.fire()

            def scene_state(i=n - 1):
                sc = self._grid_scene(i)
                if sc is None:
                    return OFF
                return BLINK if sc.is_triggered() else ON
            return _ButtonRole(press=scene_press, state=scene_state)
        if role in GRID_MOVES:
            dx, dy = GRID_MOVES[role]
            return _ButtonRole(press=lambda: self.grid_step(dx, dy),
                               state=lambda: ON if self._can_grid_step(dx, dy) else OFF)
        return None

    def _track_flag_role(self, name, track_fn):
        def press():
            t = track_fn()
            if t is None or (name == "arm" and not t.can_arm()):
                return
            t.set_flag(name, not t.get_flag(name))

        def state():
            t = track_fn()
            return ON if t is not None and t.get_flag(name) else OFF
        return _ButtonRole(press=press, state=state)

    def _cont_role(self, role):
        """role -> () -> Param or None."""
        s = self._reads
        sel = {"track_volume": "volume", "track_pan": "pan", "send_a": "send_a", "send_b": "send_b"}
        if role in sel:
            name = sel[role]

            def target():
                t = s.selected_track()
                return t.param(name) if t is not None else None
            return target
        master = {"master_volume": "volume", "cue_volume": "cue", "crossfader": "crossfader"}
        if role in master:
            return (lambda name: (lambda: s.master_param(name)))(master[role])
        if role == "tempo":
            return s.tempo_param
        base, n = _strip_role(role, ("volume", "pan"))
        if base is not None:
            def strip_target(i=n - 1, name=base):
                t = self.strip_track(i)
                return t.param(name) if t is not None else None
            return strip_target
        base, n = _strip_role(role, ("device_param",))
        if base is not None:
            def dev_target(i=n - 1):
                params = s.device_params()
                k = self.device_offset + i
                return params[k] if 0 <= k < len(params) else None
            return dev_target
        return None

    # ---- navigation ----

    def strip_track(self, i):
        tracks = self._reads.bank_tracks()
        k = self.bank_offset + i
        return tracks[k] if 0 <= k < len(tracks) else None

    def _can_bank_step(self, d):
        n = len(self._reads.bank_tracks())
        return self.bank_offset > 0 if d < 0 else self.bank_offset + STRIPS < n

    def bank_step(self, d, by=STRIPS):
        """Move the strips by `by` tracks (bank_prev/next: 8, strip_prev/next: 1)."""
        if self._can_bank_step(d):
            self.bank_offset = max(0, self.bank_offset + d * by)

    def device_page(self):
        """How far device_bank_prev / _next move. Firmware that reports layers: the live
        device_param_N in steps of 4 (4 while only device_param_1..4 are live, e.g. a 4-encoder
        layer; 8 with 5..8), or STRIPS when none is. Older firmware: always STRIPS, as before."""
        if not self.reports_layers:
            return STRIPS
        top = 0
        for n, c in self._device_slots:
            if n > top and c.live(self.layers_on):
                top = n
        return (top + 3) // 4 * 4 if top else STRIPS

    @property
    def device_bank(self):
        """The current device page, in pages of device_page()."""
        return self.device_offset // self.device_page()

    def _can_device_bank_step(self, d):
        if d < 0:
            return self.device_offset > 0
        return self.device_offset + self.device_page() < len(self._reads.device_params())

    def device_bank_step(self, d):
        if self._can_device_bank_step(d):
            self.device_offset = max(0, self.device_offset + d * self.device_page())

    def _track_index(self):
        cur = self._reads.selected_track()
        if cur is None:
            return None, []
        tracks = self._reads.nav_tracks()
        for i, t in enumerate(tracks):
            if t.same(cur):
                return i, tracks
        return None, tracks

    def _can_track_step(self, d):
        i, tracks = self._track_index()
        return i is not None and 0 <= i + d < len(tracks)

    def _track_step(self, d):
        i, tracks = self._track_index()
        if i is not None and 0 <= i + d < len(tracks):
            self.song.select_track(tracks[i + d])

    # ---- clip grid ----

    def _can_grid_step(self, dx, dy):
        """Back while the offset is > 0; on while the grid still ends before the last track / scene
        (so it stays full)."""
        if dx:
            off, size, n, d = self.grid_col, self.grid_width, len(self._reads.grid_tracks()), dx
        else:
            off, size, n, d = self.grid_row, self.grid_height, len(self._reads.grid_scenes()), dy
        return off > 0 if d < 0 else off + size < n

    def grid_step(self, dx, dy):
        if self._can_grid_step(dx, dy):
            self.grid_col += dx
            self.grid_row += dy

    def _clip(self, t, row):
        """(ClipSlot or None, ClipState or None) at the grid's column t, row `row` (0-based)."""
        k, r = self.grid_col + t, self.grid_row + row

        def read():
            tracks = self._reads.grid_tracks()
            if not 0 <= k < len(tracks):
                return None, None
            slot = tracks[k].clip_slot(r)
            return slot, (slot.state() if slot is not None else None)
        return self._reads.memo(("clip", k, r), read)

    def _clip_press(self, t, row):
        slot, st = self._clip(t, row)
        if st is None:
            return                      # an empty slot (or none there): nothing
        if st.playing or st.recording:
            slot.stop()
        else:
            slot.fire()

    def _clip_rgb(self, t, row):
        st = self._clip(t, row)[1]
        if st is None:
            return RGB_OFF + (False,)
        if st.recording:
            rgb = RGB_RECORDING
        elif st.playing:
            rgb = RGB_PLAYING
        else:
            rgb = colour_rgb(st.colour)
        return rgb + (bool(st.triggered),)

    def _grid_scene(self, i):
        scenes = self._reads.grid_scenes()
        k = self.grid_row + i
        return scenes[k] if 0 <= k < len(scenes) else None

    def grid_box(self):
        """(col, row, width, height) of the grid for the DAW to show, or None: no clip_T_S, or (on
        firmware that reports layers) none of them is live."""
        if not self._clip_comps:
            return None
        if self.reports_layers and not any(c.live(self.layers_on) for c in self._clip_comps):
            return None
        return (self.grid_col, self.grid_row, self.grid_width, self.grid_height)

    def _show_box(self, box):
        if box == self._box or self.highlight is None:
            return
        self._box = box
        self.highlight(box)

    # ---- events ----

    def handle_event(self, ev):
        if ev.type in (P.KIND_BUTTON, P.EVENT_PAD):
            # A pad twin's EVENT 7 works as a button: value > 0 the press, 0 the release.
            b = (self._buttons if ev.type == P.KIND_BUTTON else self._pads).get(ev.index)
            if b is None:
                return
            if ev.value:
                b.held, b.held_ticks = True, 0
                if b.role.press is not None:
                    b.role.press()
            else:
                b.held = False
                if b.role.release is not None:
                    b.role.release()
            self.refresh()
        elif ev.type == P.KIND_ANALOG:
            b = self._analogs.get(ev.index)
            if b is not None:
                self._analog(b, min(V14, ev.value))
        elif ev.type == P.KIND_ENCODER:
            b = self._encoders.get(ev.index)
            if b is not None:
                p = b.target()
                if p is not None and ev.delta:
                    p.nudge(ev.delta)
                    # The ring follows at once; FB_VALUE waits for the next refresh (the tick),
                    # so a fast turn isn't echoed back one FB_VALUE per EVENT.
                    self._refresh_encoder(b, value=False)
        elif ev.type == P.EVENT_LAYER:
            if ev.value:
                self.layers_on.add(ev.index)
            else:
                self.layers_on.discard(ev.index)
            self.refresh()                      # device_bank_* LEDs follow the live page size; grid box
        # keys (type 2), touch and reserved types: not mapped

    def _analog(self, b, raw):
        p = b.target()
        if p is None:
            return
        cur = to14(p.get_norm())
        if not b.picked:
            crossed = b.last_raw is not None and (b.last_raw - cur) * (raw - cur) <= 0
            if crossed or abs(raw - cur) <= PICKUP_WINDOW:
                b.picked = True
        b.last_raw = raw
        if not b.picked:
            return
        p.set_norm(float(raw) / V14)
        # What the knob set is not news to the knob: no FB_VALUE echo.
        self._value_cache[(P.KIND_ANALOG, b.component.index)] = (p.key, to14(p.get_norm()))

    # ---- clock ----

    def tick(self):
        for b in list(self._buttons.values()) + list(self._pads.values()):
            if b.held and b.role.repeat is not None:
                b.held_ticks += 1
                if b.held_ticks >= REPEAT_DELAY_TICKS:
                    b.role.repeat()
        self.refresh()

    # ---- feedback ----

    def refresh(self):
        self._reads.begin()
        try:
            self._refresh()
        finally:
            self._reads.end()

    def _refresh(self):
        s = self._reads
        dk = s.device_key()
        if dk != self._device_key:
            self._device_key = dk
            self.device_offset = 0
        n = len(s.device_params())
        if self.device_offset and self.device_offset >= n:
            page = self.device_page()
            self.device_offset = max(0, ((n - 1) // page) * page)
        n = len(s.bank_tracks())
        if self.bank_offset and self.bank_offset >= n:
            self.bank_offset = max(0, ((n - 1) // STRIPS) * STRIPS)
        if self._has_grid and (self.grid_col or self.grid_row):
            # Tracks or scenes deleted: pull the grid back so it ends at the last one.
            self.grid_col = min(self.grid_col, max(0, len(s.grid_tracks()) - self.grid_width))
            self.grid_row = min(self.grid_row, max(0, len(s.grid_scenes()) - self.grid_height))
        self._refresh_leds()
        self._refresh_rgb()
        self._show_box(self.grid_box())
        for b in self._analogs.values():
            self._refresh_analog(b)
        for b in self._encoders.values():
            self._refresh_encoder(b)

    def _refresh_leds(self):
        changed = {}
        for led, lb in self._leds.items():
            level = lb.level()
            if self._led_cache.get(led) != level:
                changed[led] = level
                self._led_cache[led] = level
        if not changed:
            return
        idx = sorted(changed)
        run = [idx[0]]
        for i in idx[1:] + [None]:
            if i is not None and i == run[-1] + 1 and len(run) < 64:
                run.append(i)
                continue
            if len(run) == 1:
                self.notify(P.FB_LED, P.fb_led(run[0], changed[run[0]]))
            else:
                self.notify(P.FB_LEDS, P.fb_leds(run[0], [changed[k] for k in run]))
            if i is not None:
                run = [i]

    def _refresh_rgb(self):
        for led, fn in self._rgb_leds.items():
            v = fn()
            if self._rgb_cache.get(led) != v:
                self._rgb_cache[led] = v
                self.notify(P.FB_LED_RGB, P.fb_led_rgb(led, v[0], v[1], v[2], v[3]))

    def _refresh_analog(self, b):
        p = b.target()
        k = (P.KIND_ANALOG, b.component.index)
        if p is None:
            self._value_cache[k] = None
            return
        now = (p.key, to14(p.get_norm()))
        if self._value_cache.get(k) != now:
            self._value_cache[k] = now
            b.picked = False                    # the target moved away from the knob
            b.last_raw = None
            if b.component.takes_value:         # takeover: none ignores FB_VALUE (sysex.md 4.6.2)
                self.notify(P.FB_VALUE, P.fb_value(P.KIND_ANALOG, b.component.index, now[1]))

    def _refresh_encoder(self, b, value=True):
        """value=False: the ring only (FB_VALUE and its cache are left for the next refresh)."""
        c = b.component
        if not value and not c.ring:
            return
        p = b.target()
        k = (P.KIND_ENCODER, c.index)
        if p is None:
            ring = (P.RING_WRAP, 0, False, True)    # nothing mapped: ring off (flags bit 1)
            if value:
                self._value_cache[k] = None
        else:
            v = to14(p.get_norm())
            now = (p.key, v)
            if value and self._value_cache.get(k) != now:
                self._value_cache[k] = now
                if c.takes_value:               # a relative encoder ignores FB_VALUE (sysex.md 4.6.2)
                    self.notify(P.FB_VALUE, P.fb_value(P.KIND_ENCODER, c.index, v))
            ring = (P.RING_BOOST_CUT, v, True, False) if p.bipolar else (P.RING_WRAP, v, False, False)
        if c.ring and self._ring_cache.get(c.index) != ring:
            self._ring_cache[c.index] = ring
            self.notify(P.FB_RING, P.fb_ring(c.index, ring[0], ring[1], ring[2], ring[3]))
