"""gemifi device YAML: loading (with line numbers), validation and C code generation.

The format and the generated-code contract are defined in core/README.md ("Device YAML" and
"Generated code"); the C structs are core/include/gm_device.h. This module is used by
gemifi_build.py (CLI + dashboard) and the tests.

    dev, diags = load_device(path)          # dev is None if there are errors
    files = generate(dev, source="devices/x.yaml")   # {"gm_device_config.h": text, ...}
"""
import difflib
import itertools
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The repo root, for components/ (appended: its core/, platform/ and site/ folders must not shadow
# anything).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if REPO_ROOT not in sys.path:
    sys.path.append(REPO_ROOT)
import components  # noqa: E402
from components.api import (CURVES, INVERT, MIGRATE_HINT, NO_PIN, OPEN_DRAIN, PRESETS_OFF,  # noqa: E402,F401
                            PULLDOWN, PULLUP, RENAMED_ID, WHITE, Hub, LDict, LList, Pin, c_str, errors, f1_on,
                            type_name, with_article)
import gemifi_board as gbd  # noqa: E402
from components.led_strip import rgb_curve  # noqa: E402,F401
from components.hub import c_comment  # noqa: E402
import components.i2c as i2c_buses  # noqa: E402
from components.keys import (T as _T, Bool as _Bool, Enum as _Enum, Hex as _Hex, Id as _Id,  # noqa: E402
                             IdRef as _IdRef, Int as _Int, Key as _Key, Keys as _Keys, List as _List, Map as _Map,
                             Null as _Null, OneOf as _OneOf, Period as _Period, Pin as _Pin, Raw as _Raw, Ref as _Ref,
                             Str as _Str, ById as _ById, CONTROL_DAW as _CONTROL_DAW)
from components.cd74hc4067 import Mux  # noqa: E402
from components.tm1628 import TM1628  # noqa: E402
from components.xy_pad import XYPad  # noqa: E402
from components import analog, buttons, encoders, key_matrix, leds, modes, pads  # noqa: E402
from components.buttons import MMC_COMMANDS  # noqa: E402,F401
from components.modes import (LAYER_ENCODER_KEYS, LAYER_KEYS, LAYER_PAD_KEYS, MODE_KEYS,  # noqa: E402,F401
                              STEP_KEYS)
from components.leds import (LED_DEFAULTS_KEYS, LED_KEYS, LED_LOOK_NAMES, LED_OWNERS,  # noqa: E402,F401
                             LED_SOURCES, TIMEOUT, TIMEOUT_NEVER, validate_timeout)
from components.buttons import (ACTION_IDS, ACTIONS, BUTTON_KEYS, NUMBER_KEYS,  # noqa: E402,F401
                                button_action)
from components.analog import ANALOG_KEYS, ANALOG_MODES, ANALOG_NUMBER_KEYS, TAKEOVER, TOUCH  # noqa: E402,F401
from components.encoders import (ACCEL_CURVES, ACCEL_KEYS, ACCEL_PRESETS, ENC_MODES, ENCODER_KEYS,  # noqa: E402,F401
                                 ENCODER_NUMBER_KEYS, ENCODER_SETTINGS_KEYS)
from components.key_matrix import MATRIX_KEYS, VELOCITY_KEYS  # noqa: E402,F401
from components.pads import PAD_DEFAULTS, PAD_KEYS, PAD_SETTINGS_KEYS  # noqa: E402,F401

try:
    import yaml
except ImportError:  # pragma: no cover - reported by gemifi_build.py
    yaml = None

YAML_HINT = ("PyYAML is required: `pip install pyyaml` (or `sudo apt install python3-yaml`, "
             "or `python3 -m venv tools/builder/.venv && tools/builder/.venv/bin/pip install pyyaml`)")

FORMAT_VERSION = 2
# The builder's version: min_version: is checked against it, and /api/info sends it.
# tools/release/set-version.mjs sets it at each release.
VERSION = "0.63.0"


# ---- MIDI messages (core/README.md "MIDI messages") ----
# The message kinds a firmware may compile in (gm_settings.h GM_MSG_*): each is on when the YAML uses it
# or lists it in midi_messages.enable. sysex is on when midi_messages.sysex has strings.
MSG_FEATURES = ["cc14", "nrpn", "program", "panic", "mmc"]
MSG_ALIASES = {"rpn": "nrpn", "bank_select": "program"}
SYSEX_MAX = 64              # bytes of one sent SysEx string (the stack buffer in controller.c)
SYSEX_TOKENS = {"v": 0x80, "msb": 0x81, "lsb": 0x82, "ch": 0x83}   # gm_device.h GM_SX_*
SX_SUM_START, SX_SUM = 0x84, 0x85
CTL_TOP = 0xFFFF            # gm_device.h GM_CTL_TOP


# ---- params and their scope (research/core/presets-design.md §1, §10.3) ----
# The ONE place the builder knows core/src/settings.c's param table: the id (gm_settings.h P_*), the
# scope (PRESET = PF_PRESET: each preset has its own value; GLOBAL: one value for the device;
# VOLATILE: never stored) and how many values a device has (0 = the param doesn't exist there).
# The storage budget (§10.5) counts the stored values from it. Keep it in step with settings.c.
PRESET, GLOBAL, VOLATILE = "preset", "global", "volatile"


def _vel(dev):
    return 1 if dev.matrix is not None and dev.matrix.kind == "velocity" else 0


def _own(lst):
    """The YAML's own components of a list: the layer twins (after them) have the layer.* params."""
    return sum(1 for x in lst if not getattr(x, "layer", 0))


def _twins(lst):
    return len(lst) - _own(lst)


def _layer_modes(dev):
    """layer.enabled: one per mode, on a device with layer twins or a cycle (components/modes/README.md "Cycles")."""
    has = dev.n_cycles or _twins(dev.buttons) or _twins(dev.encoders) or dev.pad_twins
    return len(dev.modes) if has else 0


PARAMS = [  # (name, id, scope, count(dev))
    ("midi.channel", 1, PRESET, lambda d: 1),
    ("midi.transpose", 2, PRESET, lambda d: 1),
    ("midi.octave", 3, PRESET, lambda d: 1),
    ("velocity.curve", 4, PRESET, _vel),
    ("velocity.fixed", 5, PRESET, lambda d: 1),
    ("velocity.fast", 6, GLOBAL, _vel),
    ("velocity.slow", 7, GLOBAL, _vel),
    ("analog.cc", 8, PRESET, lambda d: len(d.analogs)),
    ("button.action", 9, PRESET, lambda d: _own(d.buttons)),
    ("button.number", 10, PRESET, lambda d: _own(d.buttons)),
    ("bend.glide", 11, PRESET, lambda d: 1 if d.buttons else 0),
    ("ramp.time", 12, PRESET, lambda d: 1 if d.buttons else 0),
    ("debug.events", 13, GLOBAL, lambda d: 1),
    ("debug.log_level", 14, GLOBAL, lambda d: 1),
    ("encoder.cc", 15, PRESET, lambda d: _own(d.encoders)),
    ("encoder.mode", 16, PRESET, lambda d: _own(d.encoders)),
    ("preset.active", 17, GLOBAL, lambda d: 1 if d.has_presets else 0),
    ("preset.program", 18, PRESET, lambda d: 1 if d.has_presets else 0),
    ("preset.dirty", 19, VOLATILE, lambda d: 1 if d.has_presets else 0),
    ("pad.note", 20, PRESET, lambda d: len(d.pads)),
    *((n, i, s, (lambda d: 1 if d.pads else 0)) for n, i, s in (
        ("pad.channel", 21, PRESET), ("pad.curve", 22, PRESET), ("pad.aftertouch", 23, PRESET),
        ("pad.threshold", 24, GLOBAL), ("pad.release", 25, GLOBAL), ("pad.full", 26, GLOBAL),
        ("pad.window", 27, GLOBAL), ("pad.retrigger", 28, GLOBAL), ("pad.octave", 40, PRESET))),
    ("analog.mode", 29, PRESET, lambda d: len(d.analogs)),
    ("midi.thru", 30, GLOBAL, lambda d: 1 if d.din else 0),
    ("midi.din_out", 31, GLOBAL, lambda d: 1 if d.din else 0),
    ("midi.usb_to_din", 32, GLOBAL, lambda d: 1 if d.din and d.din.mode == DIN_MODES.index("usb_merge") else 0),
    ("preset.count", 33, GLOBAL, lambda d: 1 if d.has_presets else 0),
    # layers (components/modes/README.md "Button layers", "Cycles"): global, so they count once in the budget
    ("layer.enabled", 34, GLOBAL, _layer_modes),
    ("layer.button.action", 35, GLOBAL, lambda d: _twins(d.buttons)),
    ("layer.button.number", 36, GLOBAL, lambda d: _twins(d.buttons)),
    ("layer.encoder.cc", 37, GLOBAL, lambda d: _twins(d.encoders)),
    ("layer.encoder.mode", 38, GLOBAL, lambda d: _twins(d.encoders)),
    # diagnostics (protocol/sysex.md 4.10): READ_ONLY, never stored; 72-74 kept for more
    *((n, i, VOLATILE, (lambda d: 1)) for n, i in (
        ("diag.reset_reason", 65), ("diag.boot_loop", 66), ("diag.settings", 67), ("diag.scan_max", 68),
        ("diag.loop_max", 69), ("diag.scan_period", 70), ("diag.uptime", 71))),
    ("config.hash", 39, VOLATILE, lambda d: 1),     # read-only: the resolved YAML's hash (gemifi_yaml.py)
    # MIDI messages (core/README.md "MIDI messages"): ids 41-59
    ("preset.bank", 41, PRESET, lambda d: 1 if d.has_presets and "program" in getattr(d, "msg", ()) else 0),
    # button gestures (components/buttons/README.md "Gestures"): ids 75-79, global, each only with its gesture
    ("button.tap_time", 75, GLOBAL, lambda d: 1 if buttons.gesture_mask(d, "double_tap") else 0),
    ("button.hold_time", 76, GLOBAL, lambda d: 1 if buttons.gesture_mask(d, "hold") else 0),
    ("button.combo_time", 77, GLOBAL, lambda d: 1 if getattr(d, "combos", []) else 0),
    *((n, i, GLOBAL, (lambda d: 1 if buttons.repeat_mask(d) else 0)) for n, i in (
        ("button.repeat_delay", 78), ("button.repeat_rate", 79))),
]
# STM32F1 DMA1 (RM0008 table 78): every DMA request is wired to one channel. allocate_dma() hands
# them out: the YAML's DMA users (hardware scans, maudio_pads, din_midi dma:) may not share one.
# Timer requests are TIMn_UP (update) and TIMn_CHk (compare k); TIM3_CH2 and TIM4_CH4 have none.
STM32F1_DMA = {"ADC1": 1, "USART1_TX": 4, "USART1_RX": 5, "USART2_TX": 7, "SPI1_TX": 3, "SPI2_TX": 5,
               "TIM1_UP": 5, "TIM1_CH1": 2, "TIM1_CH2": 3, "TIM1_CH3": 6, "TIM1_CH4": 4,
               "TIM2_UP": 2, "TIM2_CH1": 5, "TIM2_CH2": 7, "TIM2_CH3": 1, "TIM2_CH4": 7,
               "TIM3_UP": 3, "TIM3_CH1": 6, "TIM3_CH3": 2, "TIM3_CH4": 3,
               "TIM4_UP": 7, "TIM4_CH1": 1, "TIM4_CH2": 4, "TIM4_CH3": 5}
STM32F1_DMA_CHANNELS = 7
# hardware_scan timers, in the order `timer: auto` tries them. TIM2..4 are also the core scan
# tick's (an interrupt, no DMA): it takes the first of them the scans leave. The analog scan can't
# have TIM1: its ADC triggers (CC1..3) need the compare outputs on, and PA9/PA10 are USART1's.
HW_SCAN_TIMERS = {"matrix": [1, 3, 4, 2], "analog": [4, 3, 2]}
TICK_TIMERS = [2, 3, 4]
# The matrix scan's DMA steps (scan_dma.c), and the requests each tries, in order: request 0 is the
# update event (period start), k is compare k. Only the select may be the update; when it isn't,
# every step moves one tick later.
HW_SCAN_ROLES = {"sel": [0, 1, 2, 3, 4], "in": [1, 3, 4, 2], "rd": [2, 1, 3, 4], "pre": [3, 4, 1, 2]}
HW_SCAN_ROLE_NAMES = {"sel": "select", "in": "columns in", "rd": "read", "pre": "precharge",
                      "mux": "mux step"}
# The analog scan: one compare steps the muxes (DMA); the ADC1 trigger is another (adc_dma.c).
HW_ADC_MUX = {4: [2, 3], 3: [1, 3, 4], 2: [1, 4]}
HW_ADC_TRIGGER = {4: "TIM4_CC4", 3: "TIM3_TRGO (OC2REF)", 2: "TIM2_CC2"}
# din_midi.mode (gm_din.h GM_DIN_ONLY / _PORT / _MERGE)
DIN_MODES = ["din_only", "separate_port", "usb_merge"]
# Params whose stored values can need 16 bits (settings.c min/max outside -128..127); every other
# value fits a byte. The packed stm32f1 record (platform/stm32f1/src/config_store.c) sizes its groups
# from this, so keep it in step with settings.c (test_builder checks it).
WIDE_PARAMS = {"velocity.fast", "velocity.slow", "bend.glide", "ramp.time",
               "pad.threshold", "pad.release", "pad.full", "pad.retrigger", "preset.bank",
               "button.tap_time", "button.hold_time", "button.combo_time", "button.repeat_delay",
               "button.repeat_rate"}
# ... and these on a device with NRPN / RPN (GM_MSG_NRPN: their numbers go up to 16383)
NRPN_WIDE_PARAMS = {"analog.cc", "encoder.cc", "layer.encoder.cc"}
PARAM_ID = {n: i for n, i, _s, _c in PARAMS}


def stored_counts(dev):
    """(per-preset values, global values) the device stores at most: the record sizes of §10.5."""
    out = {PRESET: 0, GLOBAL: 0, VOLATILE: 0}
    for _n, _i, scope, count in PARAMS:
        out[scope] += count(dev)
    return out[PRESET], out[GLOBAL]


# encoder.mode (gm_settings.h ENC_*), analog.mode (AN_MODE_*). From cc14 on they need a message kind
# (MODE_FEATURE), and the number key that gives encoder.cc / analog.cc follows the mode (MODE_NUMBER_KEY).
MODE_FEATURE = {"cc14": "cc14", "nrpn": "nrpn", "nrpn14": "nrpn", "rpn": "nrpn", "rpn14": "nrpn", "sysex": "sysex"}
MODE_NUMBER_KEY = {"nrpn": "nrpn", "nrpn14": "nrpn", "rpn": "rpn", "rpn14": "rpn", "sysex": "sysex"}
# The per-preset globals a preset may set (§10.2), as data (components/keys.py): YAML section -> its keys,
# each starting a per-preset param. Defaults and ranges as in core/src/settings.c; a time is whole ms. A
# key without a default (pad_settings') starts at the device's own value (the top-level pad_settings:).
PRESET_GLOBALS = {
    "midi": _Keys(
        _Key("channel", _Int(1, 16), default=1, param="midi.channel", doc="The MIDI channel the device plays on."),
        _Key("transpose", _Int(-24, 24), default=0, param="midi.transpose",
             doc="Semitones the keys' notes are moved by (the transpose buttons step it)."),
        _Key("octave", _Int(-4, 4), default=0, param="midi.octave",
             doc="Octaves the keys' notes are moved by (the octave buttons step it)."),
    ),
    "velocity": _Keys(
        _Key("curve", _Enum(CURVES), default="normal", param="velocity.curve",
             doc="How a key's speed becomes velocity (`fixed`: `velocity.fixed`). A velocity key matrix only."),
        _Key("fixed", _Int(1, 127), default=100, param="velocity.fixed",
             doc="The velocity of curve `fixed`, and of the `note` buttons."),
    ),
    "bend": _Keys(_Key("glide", _Period(0, 2_000_000, step_us=1000, example="80ms"), default="80ms",
                       param="bend.glide", doc="How long the pitch-bend buttons take to glide to full bend and back.")),
    "ramp": _Keys(_Key("time", _Period(0, 5_000_000, step_us=1000, example="80ms"), default="300ms",
                       param="ramp.time", doc="How long a `cc_ramp` button takes to glide from 0 to 127 and back.")),
    "pad_settings": _Keys(
        *(pads.PAD_SETTINGS[k].but(default=None, shown_default="`pad_settings:`'s").elsewhere(
            "components/pads/README.md") for k in
          ("channel", "curve", "aftertouch")),
        _Key("octave", _Int(-4, 4), default=0, param="pad.octave",
             doc="Octaves the pads' notes are moved by (the pad octave buttons step it)."),
    ),
}


def preset_base(dev, key):
    """Slot 1's value of a per-preset global (an override equal to it is dropped), in its param's units."""
    if key.default is None:                     # pad_settings': the device's own
        return dev.pad_set[key.name]
    if isinstance(key.type, _Enum):
        return key.type.values.index(key.default)
    if isinstance(key.type, _Period):
        return int(key.default.removesuffix("ms"))
    return key.default
# Keys that look per-preset but aren't: they get this message instead of "unknown key".
PRESET_NOT_HERE = {
    "midi": {"transport": "the MIDI transport is the device's (the same in every preset): set it in the "
                          "top-level midi:"},
    "velocity": {k: "global (the keybed's calibration, the same in every preset): set it in "
                    "key_matrix.velocity" for k in ("fast", "slow")},
    "pad_settings": {k: "global (the pads' calibration, the same in every preset): set it in the "
                        "top-level pad_settings:" for k in ("threshold", "release", "full", "window", "retrigger")},
}
_HW = "the same in every preset: set it on the component itself"
def _preset_keys(spec):
    """What a preset may set on a component: the keys that start a param (components/keys.py Key.param),
    and the rest, refused with where to set them. -> (allowed, refused)."""
    return [k.name for k in spec if k.param], [k.name for k in spec if not k.param]


PRESET_SPECS = {"buttons": buttons.BUTTON, "analog": analog.ANALOG, "encoders": encoders.ENCODER, "pads": pads.PAD}
PRESET_COMPONENTS = {   # section -> (component kind, plural, allowed keys, refused keys)
    "buttons": ("button", "buttons", *_preset_keys(buttons.BUTTON)),
    "analog": ("analog input", "analog inputs", *_preset_keys(analog.ANALOG)),
    "encoders": ("encoder", "encoders", *_preset_keys(encoders.ENCODER)),
    "pads": ("pad", "pads", *_preset_keys(pads.PAD)),
}
MAX_PRESETS = 64
PRESET_NAME_LEN = 16           # config_store.h CS_NAME_LEN
PRESET_REVISION_MAX = 0x3FFF
_SLOT_1 = " Not in slot 1, which is the rest of the file."


def _override(k, sec):
    """A component's key as a preset sets it: left out, the component's own value."""
    return k.elsewhere(f"components/{sec}/README.md").but(required=False, default=None, shown_default=None)


# One presets.list item (core/README.md "Presets"), checked by validate_presets(): its name, its Program
# Change, and what differs from slot 1: the per-preset globals and, by component id, the component keys that
# start a per-preset param (each the component's own Key).
PRESET_ITEM = _Keys(
    _Key("name", _Str(), shown_default="`Default` (slot 1), `Preset N`",
         doc=f"Its name: up to {PRESET_NAME_LEN} printable ASCII characters, unique."),
    _Key("program", _Int(0, 127), param="preset.program", doc="The Program Change sent when it is selected."),
    _Key("bank", buttons.BANK, param="preset.bank", see='core/README.md "MIDI messages"',
         doc="With `program:`: the Bank Select sent before it, 0..16383 or `{ msb, lsb }`."),
    *(_Key(sec, _Map(keys), doc=f"What it changes of the per-preset `{sec}` settings.{_SLOT_1}")
      for sec, keys in PRESET_GLOBALS.items()),
    *(_Key(sec, _ById(kind, _Map(_Keys(*(_override(k, sec) for k in PRESET_SPECS[sec] if k.name in allowed)))),
           doc=f"By {kind} id, what differs from the {kind}'s own keys.{_SLOT_1}")
      for sec, (kind, _pl, allowed, _hw) in PRESET_COMPONENTS.items()),
)
PRESET_KEYS = PRESET_ITEM.names
PRESETS_SPEC = _Keys(
    _Key("slots", _Int(2, MAX_PRESETS), shown_default="the number listed",
         doc="The preset slots, at least the number listed: the ones not listed start as copies of slot 1."),
    _Key("revision", _Int(0, PRESET_REVISION_MAX), default=0,
         doc="Bump it when the YAML's presets change: the updater then offers to load them."),
    _Key("list", _List(_Map(PRESET_ITEM)), doc="The presets, slot 1 first: its values are the rest of the file."),
)
PRESETS_KEYS = PRESETS_SPEC.names
# The Core area (core/yaml/presets.md)
PRESETS_AREA = _Keys(_Key("presets", _Map(PRESETS_SPEC), see='core/README.md "Presets"',
                          doc="Presets on: the preset slots, 2 or more, and the presets in them. Left out, presets "
                              "are off (one slot)."))

# ---- storage budget (§10.5) ----
# Worst-case record, in 32-bit words: header (magic, seq, slot|flags|revision, count) + a name + 2 per
# value ({id | index << 16, value}) + crc (config_store.h "Record v2"). A "packed" store (stm32f1,
# record v3) holds each param's values in one group instead: see packed_record_bytes().
REC_HEADER_WORDS, REC_NAME_WORDS, REC_CRC_WORDS = 4, 4, 1
STORE_BUDGETS = {
    # journal: `pages` pages of `page` bytes, `spare` of them kept free (one erase spare, one for a
    # slot's old and new copy while it is rewritten); a record never spans a page
    "nuvoton-nuc1xx": {"kind": "journal", "page": 512, "pages": 7, "spare": 2, "what": "data-flash journal"},
    # log: `pages` pages of `page` bytes, one kept spare for compaction, which always works while every
    # slot's newest record together (L) and the largest one (M) give L + (pages - 2) * M <= (pages - 1) *
    # page (platform/stm32f1/src/config_store.c). Records are packed (v3). Code 61: 3 pages
    "stm32f1": {"kind": "log", "page": 1024, "pages": 3, "format": "packed", "what": "config pages"},
    # nvs: one blob per slot (each <= record bytes), all of them <= total bytes
    **{p: {"kind": "nvs", "record": 1984, "total": 12 * 1024, "what": "NVS"}
       for p in ("esp32", "esp32s2", "esp32s3")},
}
BUDGET_WARN = 0.85             # warn above this share of the budget (more erase cycles)

# ---- host feedback / DAW (core/README.md "DAW feedback") ----
DEFAULT_LED_TIMEOUT_MS = 2000

# Mackie Control names -> Note On numbers (buttons and their LEDs), from the community documentation
# of the MCU / Logic Control protocol (core/README.md "Mackie Control").
MCU_NOTES = {}
for _i in range(8):
    MCU_NOTES[f"rec_{_i + 1}"] = 0x00 + _i
    MCU_NOTES[f"solo_{_i + 1}"] = 0x08 + _i
    MCU_NOTES[f"mute_{_i + 1}"] = 0x10 + _i
    MCU_NOTES[f"select_{_i + 1}"] = 0x18 + _i
    MCU_NOTES[f"vpot_push_{_i + 1}"] = 0x20 + _i
    MCU_NOTES[f"f{_i + 1}"] = 0x36 + _i
    MCU_NOTES[f"fader_touch_{_i + 1}"] = 0x68 + _i
for _n, _name in enumerate(["track", "send", "pan", "plugin", "eq", "instrument", "bank_left", "bank_right",
                            "channel_left", "channel_right", "flip", "global", "name_value", "smpte_beats"]):
    MCU_NOTES[_name] = 0x28 + _n
for _n, _name in enumerate(["midi_tracks", "inputs", "audio_tracks", "audio_instruments", "aux", "busses",
                            "outputs", "user", "shift", "option", "control", "alt", "read", "write", "trim",
                            "touch", "latch", "group", "save", "undo", "cancel", "enter", "marker", "nudge",
                            "cycle", "drop", "replace", "click", "solo", "rewind", "fast_forward", "stop",
                            "play", "record", "up", "down", "left", "right", "zoom", "scrub", "user_a",
                            "user_b"]):
    MCU_NOTES[_name] = 0x3E + _n
MCU_NOTES.update({"master_touch": 0x70, "smpte_led": 0x71, "beats_led": 0x72, "rude_solo_led": 0x73,
                  "relay": 0x76})
MCU_FADERS = {**{f"fader_{i + 1}": i for i in range(8)}, "master": 8}     # pitch bend channel
MCU_VPOTS = {f"vpot_{i + 1}": i for i in range(8)}                        # CC 0x10 + n / ring 0x30 + n

# daw: roles the gemifi DAW scripts understand (hosts/README.md "Roles"); x_<anything> is free.
DAW_ROLES = {
    "play", "stop", "record", "loop", "metronome", "overdub", "session_record", "automation_arm", "tap_tempo",
    "undo", "redo", "rewind", "fast_forward",
    "track_prev", "track_next", "track_mute", "track_solo", "track_arm",
    "track_volume", "track_pan", "send_a", "send_b",
    *(f"{r}_{i}" for r in ("mute", "solo", "arm", "select", "volume", "pan", "device_param") for i in range(1, 9)),
    "bank_prev", "bank_next", "master_volume", "master_mute", "cue_volume", "crossfader", "tempo",
    "device_prev", "device_next", "device_bank_prev", "device_bank_next", "device_on",
    "device_preset", "device_preset_bank",
    "scene_launch", "scene_prev", "scene_next", "stop_all_clips",
    # navigation, clip grid, encoder assign, modifiers, banking, sends, master, markers, displays,
    # meters (reserved): issue #21 (protocol/daw-scripts.md "Roles")
    "nav_up", "nav_down", "nav_left", "nav_right", "nav_select", "nav_back", "nav_home", "nav_alt", "nav_scroll",
    *(f"clip_{t}_{sc}" for t in range(1, 9) for sc in range(1, 9)),
    *(f"{r}_{i}" for r in ("scene", "knob", "send_a", "send_b", "meter") for i in range(1, 9)),
    "grid_up", "grid_down", "grid_left", "grid_right",
    "assign_pan", "assign_send", "assign_device", "assign_eq", "assign_track", "assign_instrument", "flip",
    "view_all", "view_midi", "view_audio", "view_inputs", "view_buses",
    "shift", "option", "control", "alt", "strip_prev", "strip_next", "master_solo", "master_select",
    "marker_prev", "marker_next", "marker_add", "nudge_back", "nudge_forward", "zoom_in", "zoom_out", "scrub",
    "automation_read", "automation_write", "automation_touch", "automation_latch", "automation_off", "save",
    "time_format", "time", "track_name", "param", "status", "master_meter",
}
DAW_ROLE_RE = re.compile(r"x_[a-z0-9_]{1,29}")
HOST_ROLE_RE = re.compile(r"[a-z][a-z0-9]*\.[a-z0-9_]+")     # <host>.<command>, e.g. zynthian.toggle_audio_play
# mcu: names whose role isn't the name itself (the DAW scripts map the others, hosts/README.md)
MCU_ROLES = {"channel_left": "strip_prev", "channel_right": "strip_next"}
COMP_STR_MAX = 31               # sysex.md lstring

# Hubs (I/O expanders), declared ESPHome style: one top-level list per chip, named like the
# ESPHome component where there is one. A pin on a hub names the hub by its chip key:
# { sn74hc595: leds, number: 3 }. ESPHome's pin key for the MCP23017 is mcp23xxx. Interchangeable
# chips for one job are platforms of a domain, a base component's list: led_strip: [{ id: strip,
# platform: ws2812, ... }], pins { led_strip: strip, number: 0 }.
#
# Each hub type is a HubType: its YAML keys, what its pins can be, its validation and the C of its
# gm_hub_t entry. The tables below (HUB_KEYS, OUT_HUBS, ...) are made from HUB_DEFS. A new
# type is a component of its own, components/<key>/ (components/hub/README.md "Adding a hub type").
# The hub types, from the components, in GM_HUB_* order.
HUB_DEFS = {t.key: t for t in sorted(components.hub_types(), key=lambda t: (t.num, t.key))}
# The display platforms (display: [{ platform: ... }]), from the components, in GM_SCREEN_* order.
DISPLAY_PLATFORMS = {t.platform: t for t in sorted(components.display_platforms(), key=lambda t: t.num)}
HUB_TYPES = list(HUB_DEFS)
# A domain's platforms, by domain: {"led_strip": {"ws2812": WS2812, ...}}.
HUB_DOMAINS = {d: {k: t for k, t in HUB_DEFS.items() if t.domain == d}
               for d in dict.fromkeys(t.domain for t in HUB_DEFS.values() if t.domain)}
# The top-level hub lists (a chip's key, or a domain) and the pin keys ({ <pin key>: <hub id> }).
HUB_LISTS = list(dict.fromkeys(t.domain or k for k, t in HUB_DEFS.items()))
HUB_PIN_KEY = {k: t.domain or t.pin_key or k for k, t in HUB_DEFS.items()}
PIN_HUB_KEYS = list(dict.fromkeys(HUB_PIN_KEY.values()))
# older list and pin keys of a type, refused with the new form
HUB_MOVED = {old: t.moved_text() for t in HUB_DEFS.values() for old in t.old_keys}
HUB_KEYS = {k: list(t.keys) for k, t in HUB_DEFS.items()}
I2C_HUBS = {k for k, t in HUB_DEFS.items() if t.i2c}
MUX_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, Mux)}
XY_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, XYPad)}
TM_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, TM1628)}
RAM_HUBS = {k for k, t in HUB_DEFS.items() if t.ram}
OUT_HUBS = {k for k, t in HUB_DEFS.items() if "out" in t.roles}
IN_HUBS = {k for k, t in HUB_DEFS.items() if "in" in t.roles}
ANALOG_HUBS = {k for k, t in HUB_DEFS.items() if "analog" in t.roles}
# A decoder's outputs are one-of-N selects: only matrix rows (key_matrix, switch_matrix).
ROW_HUBS = {k for k, t in HUB_DEFS.items() if "row" in t.roles}

TOP_KEYS = ["gemifi", "name", "friendly_name", "description", "project", "board_id", "platform", "chip", "board",
            "usb", "midi", "i2c", "scan_interval", "hardware_scan", "boot_combo", "factory_reset_combo", "unlock",
            "status_led", "feedback", "mcu", *HUB_LISTS,
            "buttons", "led_defaults", "leds", "analog", "encoders", "encoder_settings", "key_matrix", "pads",
            "pad_settings", "button_settings", "combos", "presets", "modes", "display", "displays", "din_midi", "write_to_flash", "midi_messages", "layout"]
PIN_KEYS = ["number", "inverted", "mode", "allow_other_uses", *PIN_HUB_KEYS]
# The keys of each section (the v.mapping() calls below). gemifi_schema.py builds the JSON schema from
# these lists, so a new key goes here. (The Core areas' keys are data: CORE_AREAS, at the end.)
PROJECT_KEYS = ["version"]
DECODER_ROWS_KEYS = ["decoder", "first", "count"]
NODE_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?")

# ESPHome pin modes (esphome.io "Pin Schema"): a string shorthand or a mapping of flags.
MODE_FLAGS = ["input", "output", "pullup", "pulldown", "open_drain"]
MODE_STRINGS = {"INPUT": {"input"}, "OUTPUT": {"output"}, "OUTPUT_OPEN_DRAIN": {"output", "open_drain"},
                "ANALOG": {"analog"}, "INPUT_PULLUP": {"input", "pullup"},
                "INPUT_PULLDOWN": {"input", "pulldown"},
                "INPUT_OUTPUT_OPEN_DRAIN": {"input", "output", "open_drain"}}

# Format 1 keys: an old file gets one error per old key, at its line, saying what replaced it.
RENAMED_TOP = {
    "hubs": "replaced: declare each hub under its chip's key, ESPHome style, e.g. "
            "`sn74hc595: [{ id: leds, data_pin: ..., clock_pin: ..., latch_pin: ... }]` "
            "(keys: " + ", ".join(HUB_LISTS) + ")",
    "scan_us": "renamed: use `scan_interval:` with a unit, e.g. `scan_interval: 125us`",
    "version": "moved: use `project: { version: 1.0.0 }` (as ESPHome's `project:`)",
    "ssd1306": "moved: an OLED is a display now, ESPHome style: `display: [{ id: oled, platform: ssd1306, "
               "address: 0x3C }]`, and a text display names it with `display: oled` (instead of `ssd1306: oled`)"
               + MIGRATE_HINT,
    **HUB_MOVED,
}
RENAMED_PIN = {
    "pin": "renamed: use `number:` (e.g. `{ number: PA1, inverted: true }`)",
    "channel": "renamed: use `number:` (e.g. `{ cd74hc4051: faders, number: 0 }`)",
    "hub": "replaced: name the hub by its chip, e.g. `{ sn74hc595: leds, number: 3 }` "
           "(mcp23017 hubs: `mcp23xxx:`)",
    "pullup": "replaced: use `mode: INPUT_PULLUP` (or `mode: { input: true, pullup: true }`)",
    "pulldown": "replaced: use `mode: INPUT_PULLDOWN` (or `mode: { input: true, pulldown: true }`)",
    "mcp23017": "the pin key for an MCP23017 hub is `mcp23xxx:` (as in ESPHome)",
    **HUB_MOVED,
}
RENAMED_HUB = {
    "type": "replaced: the chip is the list this hub is in (e.g. `sn74hc595:`), not a `type:` key",
    "shared": "replaced: set `allow_other_uses: true` on each shared pin, e.g. "
              "`clock_pin: { number: GPIO2, allow_other_uses: true }`",
    "data": "renamed: use `data_pin:`", "clock": "renamed: use `clock_pin:`",
    "latch": "renamed: use `latch_pin:` (sn74hc595) or `load_pin:` (sn74hc165)",
    "enable": "renamed: use `oe_pin:` (sn74hc595) or `enable_pin:` (muxes)",
    "chain": "renamed: use `sr_count:`", "select": "replaced: use `pin_s0:`, `pin_s1:`, ...",
    "common": "renamed: use `common_pin:`", "settle_us": "renamed: use `delay:` with a unit, e.g. `delay: 5us`",
}

# Ids (ESPHome rules): C identifiers, not C/C++ keywords, unique across the device. gemifi also
# wants them lower_snake_case.
ID_RE = re.compile(r"[a-z][a-z0-9_]*")
C_IDENT_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
CPP_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t
char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await
co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false
float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or
or_eq private protected public register reinterpret_cast requires restrict return short signed sizeof
static static_assert static_cast struct switch template this thread_local throw true try typedef typeid
typename union unsigned using virtual void volatile wchar_t while xor xor_eq""".split())
# Words YAML reads as booleans or null when unquoted: PyYAML (YAML 1.1) turns yes/no/on/off/true/false
# (any case) into booleans and null/~ into nothing; the YAML 1.1 spec (and some other tools) also
# read y/n as booleans. An id must read the same everywhere, so these are refused even when quoted.
YAML_WORDS = {"y", "n", "yes", "no", "on", "off", "true", "false", "null"}

MIDI_TRANSPORT_MACRO = {"usb": "GM_MIDI_USB", "serial": "GM_MIDI_SERIAL", "ble": "GM_MIDI_BLE"}
DEFAULT_USB = (0x1209, 0x0002)   # pid.codes test PID (protocol/sysex.md section 7)

# Time periods (ESPHome style): a number and a unit, e.g. 125us, 1.6ms, 2s.
TIME_RE = re.compile(r"\s*(\d+(?:\.\d*)?|\.\d+)\s*(ns|us|µs|ms|s|min)\s*")
TIME_UNIT_US = {"ns": 0.001, "us": 1, "µs": 1, "ms": 1000, "s": 1000000, "min": 60000000}


# ---------------------------------------------------------------------------------------------
# diagnostics

class Diag:
    """One problem. `src` is the file it is in when that isn't the device file itself (a package or
    an !include file): the line then counts in that file. It comes with the line (Line.file)."""

    def __init__(self, level, path, line, msg, file=None, src=None):
        self.level, self.path, self.line, self.msg, self.file = level, path, line, msg, file
        self.src = src or getattr(line, "file", None)

    def __str__(self):
        loc = self.file or "<device>"
        if self.line:
            loc += f":{self.line}"
        where = f"{self.path}: " if self.path else ""
        return f"{loc}: {self.level}: {where}{self.msg}"

    def as_dict(self):
        d = {"level": self.level, "path": self.path, "line": self.line, "msg": self.msg}
        if self.src:
            d["file"] = self.src
        return d


# ---------------------------------------------------------------------------------------------
# YAML with line numbers

class YamlLoadError(Exception):
    def __init__(self, line, msg):
        super().__init__(msg)
        self.line, self.msg = line, msg


class Line(int):
    """A 1-based line number that knows its file: set for the files a device file pulls in
    (packages, !include), None for the device file itself. Diag takes the file from it."""
    file = None

    def __new__(cls, n, file=None):
        x = super().__new__(cls, n)
        x.file = file
        return x


class Tagged:
    """A value written with one of gemifi's YAML tags (gemifi_yaml.py resolves them):
    `!include file` / `!include { file, vars }` (tag "include"), `!extend id`, `!remove id`."""

    def __init__(self, tag, value, line):
        self.tag, self.value, self.line = tag, value, line

    def __repr__(self):
        return f"!{self.tag} {self.value!r}"


TAGS = ("include", "extend", "remove")


def load_yaml(text, file=None):
    """Parse one YAML document into LDict / LList / plain scalars (and Tagged for gemifi's tags).
    file: the path to give the lines (Line.file), for a file other than the device file.
    Raises YamlLoadError."""
    if yaml is None:
        raise YamlLoadError(0, YAML_HINT)
    try:
        node = yaml.compose(text, Loader=yaml.SafeLoader)
    except yaml.YAMLError as e:
        mark = getattr(e, "problem_mark", None) or getattr(e, "context_mark", None)
        line = mark.line + 1 if mark else 0
        msg = " ".join(str(x) for x in (getattr(e, "context", None), getattr(e, "problem", None)) if x)
        raise YamlLoadError(Line(line, file), f"YAML syntax: {msg or e}")
    if node is None:
        raise YamlLoadError(Line(1, file), "the file is empty")
    ctor = yaml.SafeLoader("")
    active = set()

    def ln(n):
        return Line(n.start_mark.line + 1, file) if file else n.start_mark.line + 1

    def tagged(n):
        tag = n.tag[1:]
        if tag not in TAGS:
            raise YamlLoadError(ln(n), f"unknown tag {n.tag} (gemifi knows {', '.join('!' + t for t in TAGS)})")
        if isinstance(n, yaml.SequenceNode) or (tag != "include" and not isinstance(n, yaml.ScalarNode)):
            want = "a file name or { file: ..., vars: ... }" if tag == "include" else "an id"
            raise YamlLoadError(ln(n), f"{n.tag} takes {want}")
        val = n.value if isinstance(n, yaml.ScalarNode) else None
        if isinstance(n, yaml.MappingNode):
            n.tag = "tag:yaml.org,2002:map"
            val = conv(n)
        return Tagged(tag, val, ln(n))

    def conv(n):
        if id(n) in active:
            raise YamlLoadError(ln(n), "recursive YAML alias")
        if n.tag.startswith("!") and not n.tag.startswith("!!"):
            return tagged(n)
        if isinstance(n, yaml.MappingNode):
            active.add(id(n))
            ctor.flatten_mapping(n)
            d = LDict()
            d.line = ln(n)
            for kn, vn in n.value:
                k = conv(kn)
                kl = ln(kn)
                if isinstance(k, (dict, list, Tagged)):
                    raise YamlLoadError(kl, "a mapping key must be a plain value")
                if k in d:
                    raise YamlLoadError(kl, f"duplicate key '{k}' (first on line {d.key_lines[k]})")
                d[k] = conv(vn)
                d.key_lines[k] = kl
                if isinstance(vn, yaml.ScalarNode):
                    d.raw[k] = vn.value
            active.discard(id(n))
            return d
        if isinstance(n, yaml.SequenceNode):
            active.add(id(n))
            lst = LList()
            lst.line = ln(n)
            for item in n.value:
                lst.append(conv(item))
                lst.item_lines.append(ln(item))
                lst.item_raw.append(item.value if isinstance(item, yaml.ScalarNode) else None)
            active.discard(id(n))
            return lst
        try:
            return ctor.construct_object(n, deep=True)
        except yaml.YAMLError as e:
            raise YamlLoadError(ln(n), f"bad value: {e}")

    return conv(node)


# ---------------------------------------------------------------------------------------------
# platforms

def _ranges(nums, fmt):
    """[0,1,2,5] -> 'X0-X2, X5' using fmt(n)."""
    out, s = [], sorted(nums)
    i = 0
    while i < len(s):
        j = i
        while j + 1 < len(s) and s[j + 1] == s[j] + 1:
            j += 1
        out.append(fmt(s[i]) if i == j else f"{fmt(s[i])}-{s[j]}")
        i = j + 1
    return ", ".join(out)


class BoardPin:
    """A pin the board itself uses (Platform.board_pins). why: what for, as 'the ...' (the message is
    '<pin> is <why>'). ok: the uses that match the board's, so they get no message: 'in', 'out',
    'analog' ('in_pulldown' etc. with a pull-down). level: 'error' or 'warning' for any other use."""

    def __init__(self, level, why, ok=()):
        self.level, self.why, self.ok = level, why, frozenset(ok)


def _board_pins(pins, level, why, ok=()):
    return {n: BoardPin(level, why, ok) for n in pins}


class Platform:
    name = ""
    # The board the platform's firmware is built for (platform/<name>/boards/<board>.h) and the pins it
    # uses itself: pin number -> BoardPin. One table per platform until board profiles (#116) hold it.
    board = None
    board_pins = {}
    native_usb = False
    transports = ("usb",)
    default_transport = "usb"
    is_esp = False
    # Features the platform's HAL implements (a hub that needs one is refused where it's missing):
    i2c = False             # hal_i2c_write / hal_i2c_read (mcp23017, pcf8574)
    i2c_buses = 0           # its I2C controllers (i2c: buses)

    def parse_pin(self, v):
        """-> pin number, or raise ValueError(friendly message)."""
        raise NotImplementedError

    def pin_name(self, num):
        return str(num)

    def check(self, num, role, flags, transport):
        """-> list of (level, message) for using pin num as role ('in' | 'out' | 'analog')."""
        return []

    def check_board(self, num, role, flags):
        """-> list of (level, message) if the board uses pin num itself, other than as role."""
        bp = self.board_pins.get(num)
        use = role + ("_pulldown" if flags & PULLDOWN else "")
        if bp is None or use in bp.ok:
            return []
        return [(bp.level, f"{self.pin_name(num)} is {bp.why} (board {self.board}: "
                           f"platform/{self.name}/boards/{self.board}.h)")]

    def serial_tx(self, num):
        """-> None if num can be a maudio_pads tx_pin, else why not."""
        return f"maudio_pads isn't supported on {self.name} yet"

    def serial_rx(self, tx, rx):
        """-> None if rx can be the maudio_pads rx_pin that goes with tx_pin tx, else why not."""
        return f"maudio_pads rx_pin isn't supported on {self.name} yet"

    def din_uart(self, tx, rx):
        """-> None if tx / rx (None: no MIDI In) can be the DIN MIDI UART, else why not."""
        return f"din_midi isn't supported on {self.name} yet"

    def ledstrip(self, num, others):
        """-> None if MCU pin num can drive a ws2812 strip (others: the other strips' pins), else why not."""
        return (f"ws2812 isn't supported on {self.name} yet (the platform has no LED strip driver, "
                "hal_ledstrip_*)")

    def adc(self, num):
        """-> (ok, warning or None)"""
        return False, None

    def adc_text(self):
        """Its ADC pins, for messages."""
        return "no ADC pins"


class NucPlatform(Platform):
    name = "nuvoton-nuc1xx"
    native_usb = True
    PORTS = "ABCD"
    VALID = "PA0-PA15, PB0-PB15, PC0-PC15, PD0-PD15"

    def parse_pin(self, v):
        m = re.fullmatch(r"P([A-Za-z])\.?(\d{1,2})", str(v).strip()) if not isinstance(v, bool) else None
        if not m or m.group(1).upper() not in self.PORTS or int(m.group(2)) > 15:
            hint = " (NUC1xx pins are named by port, e.g. PA1)" if isinstance(v, int) else ""
            raise ValueError(f"'{v}' is not a pin on {self.name}{hint} (valid: {self.VALID})")
        return self.PORTS.index(m.group(1).upper()) * 16 + int(m.group(2))

    def pin_name(self, num):
        return f"P{self.PORTS[num // 16]}{num % 16}"

    def check(self, num, role, flags, transport):
        out = []
        if flags & PULLDOWN:
            out.append(("error", "NUC1xx GPIOs have no internal pull-down: fit an external one and use "
                                 "mode: INPUT (INPUT_PULLUP uses the quasi-bidirectional mode's weak pull-up)"))
        return out

    def adc_text(self):
        return "ADC pins: PA0-PA7"

    def adc(self, num):
        return num < 8, None   # PA0-PA7 = ADC0-ADC7

    # Keystation Mini 32 MK3: the bootloader reads Pitch down + Pitch up (PA5, PA6, active-low, external
    # pull-ups) at reset (BOARD_BOOT_COMBO); the app drives no pins of its own. Buttons there (the
    # Keystation's own) and outputs are fine; an analog input (a pot) can hold the pin low at power-up.
    board = "keystation-mini32-mk3"
    board_pins = _board_pins((5, 6), "warning", "read at reset by the bootloader (BOARD_BOOT_COMBO: PA5 and "
                             "PA6 both low = update mode), so a pot turned down at power-up can start the "
                             "bootloader", ok=("in", "out"))


class Stm32F1Platform(Platform):
    """STM32F103-class parts (and GD32F103 clones): ports A-G, pin = port * 16 + bit."""
    name = "stm32f1"
    native_usb = True
    PORTS = "ABCDEFG"
    VALID = "PA0-PA15 ... PG0-PG15 (as fitted on the package)"
    # ADC1 channels 0-15: PA0-PA7 = 0-7, PB0-PB1 = 8-9, PC0-PC5 = 10-15
    ADC = set(range(0, 8)) | {16, 17} | set(range(32, 38))

    def parse_pin(self, v):
        m = re.fullmatch(r"P([A-Za-z])\.?(\d{1,2})", str(v).strip()) if not isinstance(v, bool) else None
        if not m or m.group(1).upper() not in self.PORTS or int(m.group(2)) > 15:
            hint = " (STM32 pins are named by port, e.g. PB8)" if isinstance(v, int) else ""
            raise ValueError(f"'{v}' is not a pin on {self.name}{hint} (valid: {self.VALID})")
        return self.PORTS.index(m.group(1).upper()) * 16 + int(m.group(2))

    def pin_name(self, num):
        return f"P{self.PORTS[num // 16]}{num % 16}"

    def check(self, num, role, flags, transport):
        out, nm = [], self.pin_name(num)
        if num in (11, 12):
            out.append(("error", f"{nm} is a USB data line (D-/D+)"))
        if num in (13, 14):
            out.append(("warning", f"{nm} is an SWD pin (SWDIO/SWCLK): using it switches the debug port "
                                   "off, so SWD stops working"))
        # PA15, PB3, PB4 are JTAG-only: the platform switches JTAG off and keeps SWD
        # (sys_clock_init, BOARD_SWJ_NOJTAG in the board header), so no warning.
        if num == 18:
            out.append(("warning", "PB2 is BOOT1 on STM32F1: a pull at reset can change the boot mode"))
        if num in (45, 46, 47) and role == "out":
            out.append(("warning", f"{nm} is in the backup domain: it can only sink about 3 mA and "
                                   "switch slowly (2 MHz); don't drive LEDs from it directly"))
        return out

    def adc_text(self):
        return "ADC pins: PA0-PA7, PB0-PB1, PC0-PC5"

    def adc(self, num):
        return num in self.ADC, None

    def serial_tx(self, num):
        """-> None if num can be a maudio_pads tx_pin, else why not."""
        return None if num == 2 else "only PA2 (USART2 TX) is supported as a maudio_pads tx_pin on stm32f1"

    def serial_rx(self, tx, rx):
        return None if tx == 2 and rx == 3 else "the maudio_pads rx_pin on stm32f1 is PA3 (USART2 RX, with tx_pin PA2)"

    def din_uart(self, tx, rx):
        if tx != 9 or rx not in (None, 10):
            return "din_midi on stm32f1 is USART1: tx_pin PA9, rx_pin PA10"
        return None

    # ws2812 on stm32f1: the bits are an SPI's MOSI (platform/stm32f1/src/hal.c), so a strip goes on PA7
    # (SPI1) or PB15 (SPI2), one strip each.
    LEDSTRIP_SPI = {7: 1, 31: 2}

    def ledstrip(self, num, others):
        if num not in self.LEDSTRIP_SPI:
            return "a ws2812 strip on stm32f1 is on an SPI's MOSI: PA7 (SPI1) or PB15 (SPI2)"
        if num in others:
            return f"{self.pin_name(num)} has a ws2812 strip already: one strip per SPI (PA7, PB15)"
        return None

    # M-Audio Code 61: PA8 switches the USB D+ pull-up (usbd.c), PC13 / PC15 are the soft power button
    # and latch (startup.c, main.c): errors. The recovery combo (recovery.c), before any gemifi code
    # runs, drives PB3-PB7 low and reads PB8 / PB9 (pulled up; both low = the bootloader). A plain
    # input or an output there is fine (the Code 61's matrix row selects and columns are wired so);
    # an input with a pull-down (a button to VCC) is what conflicts: a warning.
    board = "m-audio-code61"
    board_pins = {
        8: BoardPin("error", "the board's USB D+ pull-up switch (BOARD_USB_PULLUP_PIN): the platform drives it"),
        45: BoardPin("error", "the board's soft power button (BOARD_POWER_BTN_PIN): the firmware reads it "
                              "(held 2 s = switch off)"),
        47: BoardPin("error", "the board's power latch (BOARD_POWER_LATCH_PIN): the firmware drives it high "
                              "to stay on"),
        **_board_pins(range(19, 24), "warning", "driven low at reset by the recovery combo "
                      "(BOARD_RECOVERY_DRIVE_MASK, PB3-PB7), so a button to VCC on it shorts the pin while "
                      "held at power-up (wire buttons to GND: INPUT_PULLUP)", ok=("in", "out")),
        **_board_pins((24, 25), "warning", "read at reset by the recovery combo (BOARD_RECOVERY_MASK: PB8 "
                      "and PB9 both low = the bootloader), so a button to VCC held low by a pull-down "
                      "starts the bootloader at power-up (wire buttons to GND: INPUT_PULLUP)", ok=("in", "out")),
    }


class EspPlatform(Platform):
    is_esp = True
    i2c = True              # i2c_master (platform/esp/gemifi_esp/hal_esp.c)
    i2c_buses = 2           # the ESP32, S2 and S3 each have two I2C controllers

    def __init__(self, name, valid, flash, strapping, input_only, adc1, adc2, usb_pins=(),
                 uart0=(), psram=(), transports=("serial",), default_transport="serial"):
        self.name, self.valid = name, set(valid)
        self.flash, self.strapping, self.input_only = set(flash), set(strapping), set(input_only)
        self.adc1, self.adc2, self.usb_pins = set(adc1), set(adc2), set(usb_pins)
        self.uart0, self.psram = set(uart0), set(psram)
        self.transports, self.default_transport = transports, default_transport
        self.native_usb = "usb" in transports
        self.rmt_tx = {"esp32": 8, "esp32s2": 4, "esp32s3": 4}.get(name, 0)   # RMT TX channels (ws2812)

    def valid_text(self):
        return _ranges(self.valid, lambda n: f"GPIO{n}")

    def parse_pin(self, v):
        if isinstance(v, bool):
            raise ValueError(f"'{v}' is not a pin")
        s = str(v).strip()
        m = re.fullmatch(r"(?:GPIO|IO)?(\d{1,2})", s, re.I)
        n = int(m.group(1)) if m else None
        if n is None or n not in self.valid:
            raise ValueError(f"'{v}' is not a pin on {self.name} (valid: {self.valid_text()})")
        return n

    def pin_name(self, num):
        return f"GPIO{num}"

    def check(self, num, role, flags, transport):
        out, nm = [], self.pin_name(num)
        if num in self.input_only and role == "out":
            out.append(("error", f"{nm} is input-only on {self.name}"))
        if num in self.flash:
            out.append(("warning", f"{nm} is wired to the SPI flash on most {self.name} modules; "
                                   "using it will likely crash the chip"))
        if num in self.psram:
            out.append(("warning", f"{nm} is used by octal PSRAM/flash on some {self.name} modules"))
        if num in self.strapping:
            out.append(("warning", f"{nm} is a strapping pin on {self.name}: an external pull or a "
                                   "pressed button at reset can change the boot mode"))
        if num in self.usb_pins and transport == "usb":
            out.append(("error", f"{nm} is a native USB data line (D-/D+) and midi.transport is usb"))
        if num in self.uart0:
            if transport == "serial":
                out.append(("error", f"{nm} is UART0, which carries MIDI when midi.transport is serial"))
            else:
                out.append(("warning", f"{nm} is UART0 (boot log / serial console)"))
        if flags & (PULLUP | PULLDOWN):
            if self.name == "esp32" and num in self.input_only:
                out.append(("error", f"{nm} has no internal pull-up/down on esp32: fit an external one "
                                     "and use mode: INPUT"))
        return out

    def ledstrip(self, num, others):
        if len(others) + 1 > self.rmt_tx:
            return f"{len(others) + 1} ws2812 strips: {self.name} has {self.rmt_tx} RMT TX channels, one a strip"
        return None

    def adc_text(self):
        return f"ADC pins: {_ranges(self.adc1 | self.adc2, lambda n: f'GPIO{n}')}"

    def adc(self, num):
        if num in self.adc1:
            return True, None
        if num in self.adc2:
            return True, (f"{self.pin_name(num)} is on ADC2, which can't be read while Wi-Fi is on "
                          f"(prefer an ADC1 pin: {_ranges(self.adc1, lambda n: f'GPIO{n}')})")
        return False, None


PLATFORMS = {p.name: p for p in [
    NucPlatform(),
    Stm32F1Platform(),
    EspPlatform("esp32", valid=list(range(0, 20)) + [21, 22, 23, 25, 26, 27] + list(range(32, 40)),
                flash=range(6, 12), strapping=[0, 2, 5, 12, 15], input_only=range(34, 40),
                adc1=range(32, 40), adc2=[0, 2, 4, 12, 13, 14, 15, 25, 26, 27], uart0=[1, 3],
                transports=("serial", "ble"), default_transport="serial"),
    EspPlatform("esp32s2", valid=list(range(0, 22)) + list(range(26, 47)), flash=range(26, 33),
                strapping=[0, 45, 46], input_only=[46], adc1=range(1, 11), adc2=range(11, 21),
                usb_pins=[19, 20], uart0=[43, 44], transports=("usb", "serial"), default_transport="usb"),
    EspPlatform("esp32s3", valid=list(range(0, 22)) + list(range(26, 49)), flash=range(26, 33),
                psram=range(33, 38), strapping=[0, 3, 45, 46], input_only=[], adc1=range(1, 11),
                adc2=range(11, 21), usb_pins=[19, 20], uart0=[43, 44],
                transports=("usb", "serial", "ble"), default_transport="usb"),
]}


# ---------------------------------------------------------------------------------------------
# the model

# Pin, NO_PIN, Hub (a record: a hub, a button, an LED ...): components/api.py (imported above).


class Device:
    def __init__(self):
        self.msg = set()             # MIDI message kinds compiled in (MSG_FEATURES, "sysex"): msg_features()
        self.msg_used = set()        # ... those the YAML uses
        self.pad_action_uses = []    # (path, line, action): pad_octave_* (checked once pads: is read)
        self.msg_enable = set()      # ... and midi_messages.enable
        self.sysex = []              # midi_messages.sysex: Hub(id, data (bytes, GM_SX_* codes), flags)
        self.sysex_ids = {}
        self.null_rpn = False
        self.mmc_id = 0x7F
        self.hubs, self.buttons, self.leds, self.analogs, self.encoders = [], [], [], [], []
        self.screens = []           # display: (components/display)
        self.matrix = None
        self.version = None
        self.boot_combo = []
        # safety (core/README.md "Factory-reset combo", "Physical unlock", "Status LED")
        self.reset_combo, self.reset_hold_ms = [], RESET_HOLD_DEFAULT_MS
        self.unlock, self.unlock_hold_ms = [], UNLOCK_HOLD_DEFAULT_MS   # [] = unlock off
        self.status_led = None   # LED index
        self.i2c_buses = []         # i2c: (components/i2c)
        self.feedback = True
        self.led_owner, self.led_timeout = 0, DEFAULT_LED_TIMEOUT_MS // 10
        self.host_fb = True          # feedback.script: GET_COMPONENT and FB_* (sysex.md 4.6)
        self.mcu = None              # Hub(extender=bool, cable=0|1) when mcu: is given
        self.daw_port = False
        self.din = None              # Hub(tx, rx, mode, port_name, thru, out, usb) for din_midi:
        # presets (research/core/presets-design.md): slot n = presets[n - 1], Hub(name, program, vals)
        self.n_presets = 1
        self.slots_invalid = False   # preset_slots: presets: gives an invalid slot count
        self.presets_given = False   # the YAML has presets:
        self.presets_revision = 0
        self.presets = []
        self.modes = []              # Hub per mode (components/modes), in YAML order; a cycle's steps in a row
        self.n_cycles = 0            # modes: cycle: groups
        self.pads = []               # Hub per pad (pads.validate), in YAML order
        self.pad_twins = []          # Hub per pad layer twin (mode_layer): GET_COMPONENT pads after dev.pads
        self.btn_set = {}            # button_settings: the gesture timing params' defaults (buttons.button_settings)
        self.combos = []             # Hub per combo (buttons.validate_combos), in YAML order
        self.btn_lists = []          # action lists (buttons.action_list): a tuple of items each, each list once
        self.pad_set = dict(PAD_DEFAULTS)   # pad_settings: the pad.* param defaults
        self.displays = []           # Hub per display (validate_displays), in YAML order
        self.layout = None           # (width, height, [Hub item]) from layout: (validate_layout)
        self.budget = None           # storage_budget(self), after validation
        self.hw_scan = None          # (row_ns, settle_ns, precharge) from hardware_scan.matrix
        self.hw_adc = False          # hardware_scan.analog
        self.hw_timer = {}           # scan -> (timer n or None = auto, path, line)
        self.dma_map = []            # allocate_dma: [(channel, request, user)]
        self.scan_timers = {}        # allocate_dma: scan -> (timer n, {role: request}); 'tick' -> n
        self.config_hash = None      # load_device: the resolved YAML's hash (param config.hash)
        self.sources = []            # load_device: the files a package or !include pulled in
        self.board = None            # the board profile (gemifi_board.load), bare-metal platforms only
        self.board_given = False     # ... named by board: (else the platform's default)

    @property
    def has_presets(self):
        """Presets are on with 2 or more slots. One slot (no presets:) is presets off: it only holds the
        settings (core/README.md "Presets")."""
        return self.n_presets > 1


class Use:
    """One use of a pin, for the duplicate check. line_kind: the hub control line it is ('clock',
    'latch', ...), or None for a component pin; share: allow_other_uses was set."""

    def __init__(self, path, line, line_kind=None, share=False, owner=None):
        self.path, self.line, self.line_kind, self.share, self.owner = path, line, line_kind, share, owner


# ---------------------------------------------------------------------------------------------
# validation

ROLE_TEXT = {"in": "an input", "out": "an output", "analog": "an analog input", "common": "a mux common pin"}


class _V:
    def __init__(self, file):
        self.file, self.diags = file, []
        self.uses = {}
        self.plat = None
        self.board = None        # the board profile: its reserved pins join the platform check
        self.transport = None
        self.hub_by_id = {}
        self.screen_by_id = {}
        self.presets = None      # the presets: mapping, once preset_slots has checked it
        self.mode_toggles = {}   # button index -> mode id, for the preset check
        self.mode_with_toggles = {}   # toggle: { button, with }: button index -> mode id (keeps its action)
        self.mode_with_buttons = {}   # ... and its with: button index -> mode id
        self.mode_with_of = {}   # ... and its with: button index
        self.mode_modifiers = {}  # a layer-only hold mode's button (a modifier, keeps its action) -> mode id
        self.ids = {}            # id -> (kind, path, line, index, derived-from-name or None)

    def err(self, path, line, msg):
        self.diags.append(Diag("error", path, line, msg, self.file))

    def warn(self, path, line, msg):
        self.diags.append(Diag("warning", path, line, msg, self.file))

    # -- generic field helpers --

    def mapping(self, v, path, line, keys, renamed=None):
        """Check v is a mapping with only `keys`. Keys in `renamed` ({old: message}) are format 1
        keys: reported with that message."""
        if not isinstance(v, dict):
            self.err(path, line, f"must be a mapping (got {type_name(v)})")
            return None
        for k in v:
            if renamed and k in renamed:
                self.err(f"{path}.{k}" if path else str(k), v.line_of(k), renamed[k])
            elif k not in keys:
                close = difflib.get_close_matches(str(k), keys, 1)
                hint = f" (did you mean '{close[0]}'?)" if close else f" (known: {', '.join(keys)})"
                self.err(f"{path}.{k}" if path else str(k), v.line_of(k), f"unknown key{hint}")
        return v

    def seq(self, v, path, line):
        if v is None:
            return LList()
        if not isinstance(v, list):
            self.err(path, line, f"must be a list (got {type_name(v)})")
            return LList()
        return v

    def integer(self, d, key, path, lo, hi, default=None, required=False):
        p = f"{path}.{key}" if path else key
        if key not in d or d[key] is None:
            if required:
                self.err(p, d.line, "is required")
            return default
        v = d[key]
        if isinstance(v, bool) or not isinstance(v, int):
            self.err(p, d.line_of(key), f"must be an integer {lo}..{hi} (got {type_name(v)} {d.raw_of(key)!r})")
            return default
        if not lo <= v <= hi:
            fmt = (lambda x: f"0x{x:X}") if hi > 255 and key in ("board_id", "vid", "pid", "address") else str
            self.err(p, d.line_of(key), f"{fmt(v)} is out of range ({fmt(lo)}..{fmt(hi)})")
            return default
        return v

    def boolean(self, d, key, path, default=False):
        if key not in d:
            return default
        v = d[key]
        if not isinstance(v, bool):
            self.err(f"{path}.{key}", d.line_of(key), f"must be true or false (got {v!r})")
            return default
        return v

    def string(self, d, key, path, required=False, default=None):
        p = f"{path}.{key}" if path else key
        if key not in d or d[key] is None:
            if required:
                self.err(p, d.line, "is required")
            return default
        v = d[key]
        if isinstance(v, (int, float, bool)):
            v = d.raw_of(key)        # `name: 1.0` / `name: Off` mean the text as written
        if not isinstance(v, str) or not v.strip():
            self.err(p, d.line_of(key), f"must be a non-empty string (got {v!r})")
            return default
        if any(ord(c) > 126 or ord(c) < 32 for c in v):
            self.warn(p, d.line_of(key), "contains non-ASCII or control characters; they may not "
                                         "show correctly in the web UI")
        return v

    def period(self, d, key, path, lo_us, hi_us, default=None, step_us=1, example="125us"):
        """An ESPHome time period (`125us`, `1.6ms`, `2s`) -> whole multiples of step_us, or default."""
        p = f"{path}.{key}" if path else key
        if key not in d or d[key] is None:
            return default
        x, raw, line = d[key], d.raw_of(key), d.line_of(key)
        m = TIME_RE.fullmatch(x) if isinstance(x, str) else None
        if not m:
            if isinstance(x, (int, float)) and not isinstance(x, bool):
                self.err(p, line, f"needs a unit: `{key}: {raw}` could be any unit; write e.g. "
                                  f"`{key}: {raw}us` or `{raw}ms` (units: us, ms, s)")
            else:
                self.err(p, line, f"must be a time period like {example} (got {raw!r}; units: us, ms, s)")
            return default
        us = float(m.group(1)) * TIME_UNIT_US[m.group(2)]
        n = round(us / step_us)
        step = fmt_us(step_us)
        if abs(us / step_us - n) > 1e-6:
            self.warn(p, line, f"{raw} rounded to {fmt_us(n * step_us)} (steps of {step})")
        if not lo_us <= n * step_us <= hi_us:
            self.err(p, line, f"{raw} is out of range ({fmt_us(lo_us)}..{fmt_us(hi_us)})")
            return default
        return n

    # -- ids and names --

    def ident(self, d, path, kind, index, allow_name=True):
        """The component's id (ESPHome rules). With allow_name, an id may be left out when name: is
        given: it is then derived from the name ("Oct down" -> oct_down). Returns the id or None."""
        p = f"{path}.id"
        if "id" not in d:
            if "label" in d:            # format 1; reported as renamed already
                return None
            name = d.get("name") if allow_name else None
            if name is None:
                need = "id: or name:" if allow_name else "id:"
                self.err(path, d.line, f"needs {need} (e.g. `id: {example_id(kind)}`; the id is what "
                                       "the rest of the file uses to refer to it)")
                return None
            name = d.raw_of("name") if not isinstance(name, str) else name
            v = derive_id(name)
            why = id_problem(v, v)
            if why:
                self.err(f"{path}.name", d.line_of("name"), f"the id derived from name '{name}' would be "
                                                            f"'{v}', which can't be an id: set id:")
                return None
            return self.register(v, path, d.line_of("name"), kind, index, derived=name)
        v, raw, line = d["id"], d.raw_of("id"), d.line_of("id")
        why = id_problem(v, raw)
        if why:
            self.err(p, line, f"{why} (e.g. `id: {suggest_id(raw, kind)}`)")
            return None
        return self.register(v, path, line, kind, index)

    def register(self, v, path, line, kind, index, derived=None):
        if v in self.ids:
            k0, p0, l0, _, d0 = self.ids[v]
            how = lambda pth, dn: f"{pth} (derived from its name '{dn}')" if dn else pth  # noqa: E731
            self.err(f"{path}.{'name' if derived else 'id'}", line,
                     f"id '{v}' of {how(path, derived)} is already the id of {how(p0, d0)} (line {l0}, "
                     f"{with_article(k0)}); ids are unique across the device: set a different id:")
            return None
        self.ids[v] = (kind, path, line, index, derived)
        return v

    def display_name(self, d, path, ident):
        """name: (free text, optional), else derived from the id: oct_down -> "Oct down"."""
        name = self.string(d, "name", path)
        return name if name is not None else humanize(ident or "")

    def unique_name(self, names, kind, name, path, d):
        """Display names label the rows of the web config table and the CLI's `param:name` keys:
        two of the same kind with one name can't be told apart. names: (kind, name) -> path, kept by
        the caller across its list."""
        if not name:
            return
        first = names.setdefault((kind, name.lower()), path)
        if first != path:
            self.warn(f"{path}.name" if "name" in d else path, d.line_of("name") if "name" in d else d.line,
                      f"'{name}' is also the name of {first}: the config page and CLI can't tell them apart "
                      "(set a different name:)")

    def enum_value(self, d, key, path, options):
        """d[key]'s index in options, or None (an error, with the closest option)."""
        x = d[key]
        if x in options:
            return options.index(x)
        close = difflib.get_close_matches(str(d.raw_of(key)), options, 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        self.err(path, d.line_of(key), f"unknown {key} '{d.raw_of(key)}' ({hint}valid: {', '.join(options)})")
        return None

    @staticmethod
    def hub_keys(kind):
        """The hub types (YAML keys) of a kind, for a pin's hubs=: a HubType class (Mux ...), or a role
        ('in', 'out', 'analog', 'row')."""
        if isinstance(kind, str):
            return {k for k, t in HUB_DEFS.items() if kind in t.roles}
        return {k for k, t in HUB_DEFS.items() if isinstance(t, kind)}

    @staticmethod
    def names_hub(x):
        """A pin given as a mapping that names a hub ({ sn74hc165: x, number: 3 })."""
        return isinstance(x, dict) and any(k in x for k in PIN_HUB_KEYS)

    def ref(self, container, key, path, line, kind, plural):
        """A reference to another component by id (`led: edit_led`, boot_combo items, hub pins).
        Returns the index of that `kind` component, or None after reporting why not."""
        val = container[key]
        raw = container.raw_of(key)
        target = self.ids.get(val) if isinstance(val, str) else None
        if target is None:
            known = [i for i, t in self.ids.items() if t[0] == kind]
            close = difflib.get_close_matches(raw, known, 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            extra = ""
            if val is not None and not isinstance(val, str):
                extra = f" (YAML reads `{raw}` as the {type_name(val)} {val!r}, not a name)"
            self.err(path, line, f"no {kind} with id '{raw}'{extra} ({plural}: {', '.join(known) or 'none'}{hint})")
            return None
        if target[0] != kind:
            self.err(path, line, f"'{raw}' is the id of {target[1]} ({with_article(target[0])}), not of "
                                 f"{with_article(kind)}")
            return None
        return target[3]

    # -- pins --

    def mode(self, v, path, role, open_drain=False):
        """ESPHome pin mode (string or mapping) -> set of flags, checked against the role.
        open_drain: the pin may be an open-drain output. Returns None after an error."""
        if "mode" not in v or v["mode"] is None:
            return set()
        m, line = v["mode"], v.line_of("mode")
        if isinstance(m, str):
            flags = MODE_STRINGS.get(m.upper())
            if flags is None:
                close = difflib.get_close_matches(m.upper(), list(MODE_STRINGS), 1)
                hint = f"; did you mean {close[0]}?" if close else ""
                self.err(f"{path}.mode", line, f"unknown mode '{m}' ({', '.join(MODE_STRINGS)}{hint})")
                return None
            flags = set(flags)
        elif isinstance(m, dict):
            mm = self.mapping(m, f"{path}.mode", line, MODE_FLAGS)
            flags = {k for k in MODE_FLAGS if self.boolean(mm, k, f"{path}.mode")}
        else:
            self.err(f"{path}.mode", line, f"must be a mode name like INPUT_PULLUP or a mapping "
                                           f"{{ input: true, pullup: true }} (got {type_name(m)})")
            return None
        bad = None
        if "open_drain" in flags and not (open_drain and role == "out"):
            bad = "open_drain is only supported on a tm1628 or tm1637 dio_pin"
        elif "pullup" in flags and "pulldown" in flags:
            bad = "pullup and pulldown are both set"
        elif role == "out" and flags - {"output", "open_drain"}:
            bad = f"this pin is {ROLE_TEXT[role]}: use mode: OUTPUT" + \
                  (" or OUTPUT_OPEN_DRAIN" if open_drain else "") + " (or leave mode out)"
        elif role in ("in", "common") and "output" in flags:
            bad = f"this pin is {ROLE_TEXT[role]}: mode can't include output"
        elif role == "in" and "analog" in flags:
            bad = "this pin is a digital input: mode can't be ANALOG"
        elif role == "analog" and flags - {"analog", "input"}:
            bad = "an analog input takes mode ANALOG or INPUT (no pull resistors)"
        if bad:
            self.err(f"{path}.mode", line, bad)
            return None
        return flags

    def pin(self, v, path, line, role, allow_hub=True, line_kind=None, owner=None, hubs=None, why=None,
            open_drain=False):
        """Resolve a pin (ESPHome pin schema: `PA1` or { number, inverted, mode, allow_other_uses },
        on a hub { <chip>: <hub id>, number }). role: 'in' | 'out' | 'analog' | 'common'.
        line_kind: the hub control line this is (for allow_other_uses). Returns Pin or None."""
        if v is None:
            self.err(path, line, "is required")
            return None
        flags, hub_key, share = 0, None, False
        raw = v
        if isinstance(v, dict):
            v = self.mapping(v, path, line, PIN_KEYS, renamed=RENAMED_PIN)
            if v is None:
                return None
            if any(k in v for k in RENAMED_PIN):
                return None        # format 1 pin: already reported
            if any(k not in PIN_KEYS for k in v):
                return None        # an unknown key (a hub type gemifi lacks, say): already reported,
                #                    and its number: isn't an MCU pin
            if self.boolean(v, "inverted", path):
                flags |= INVERT
            mode = self.mode(v, path, role, open_drain)
            if mode is None:
                return None
            flags |= (PULLUP if "pullup" in mode else 0) | (PULLDOWN if "pulldown" in mode else 0)
            flags |= OPEN_DRAIN if "open_drain" in mode else 0
            share = self.boolean(v, "allow_other_uses", path)
            if share and line_kind is None:
                self.err(f"{path}.allow_other_uses", v.line_of("allow_other_uses"),
                         "gemifi only shares hub control lines (e.g. one clock_pin for two shift-register "
                         "hubs): a component pin can't have another use")
                return None
            keys = [k for k in PIN_HUB_KEYS if k in v]
            if len(keys) > 1:
                self.err(path, line, f"names more than one hub ({', '.join(keys)})")
                return None
            hub_key = keys[0] if keys else None
            raw = v.get("number")
            if raw is None:
                self.err(path, line, "needs number: (the pin" + (f", or the channel on hub '{v[hub_key]}')"
                                                                 if hub_key else ")"))
                return None
            if hub_key is None:
                line = v.line_of("number")
        if hub_key is None:
            if isinstance(raw, (dict, list)):
                self.err(path, line, f"must be a pin name (got {type_name(raw)})")
                return None
            try:
                num = self.plat.parse_pin(raw)
            except ValueError as e:
                self.err(path, line, str(e))
                return None
            name = self.plat.pin_name(num)
            if role == "analog":
                ok, w = self.plat.adc(num)
                if not ok:
                    self.err(path, line, f"{name} has no ADC on {self.plat.name} "
                                         f"({self.plat.adc_text()}; or use a mux hub channel)")
                elif w:
                    self.warn(path, line, w)
            r = "in" if role == "common" else role
            msgs = self.plat.check(num, r, flags, self.transport)
            # The pins the board itself uses, from one source: the platform's table (Platform.board_pins,
            # finer: which uses match the board's) for the board it describes, else the board profile's
            # `reserved` pins (issue #116).
            if not self.board or self.board["id"] == self.plat.board:
                msgs += self.plat.check_board(num, r, flags)
            else:
                msgs += gbd.pin_check(self.board, num, r)
            for lvl, msg in msgs:
                (self.err if lvl == "error" else self.warn)(path, line, msg)
            self.use(("mcu", num), name, Use(path, line, line_kind, share, owner))
            return Pin(0, flags, num, name)
        if not allow_hub:
            self.err(path, line, "must be an MCU pin (hub control pins can't be on another hub)")
            return None
        hub_id = v[hub_key]
        hub = self.hub_by_id.get(hub_id) if isinstance(hub_id, str) else None
        if hub is None:
            idx = self.ref(v, hub_key, path, line, "hub", "hubs")
            if idx is None:
                return None
            hub = self.hub_by_id.get(hub_id)
            if hub is None:
                return None
        if HUB_PIN_KEY[hub.type] != hub_key:
            self.err(path, line, f"hub '{hub.id}' is a {hub.type}: write `{HUB_PIN_KEY[hub.type]}: {hub.id}`, "
                                 f"not `{hub_key}: {hub.id}`")
            return None
        hub.used = True
        num = self.hub_pin_num(hub, raw, path, line)
        if num is None:
            return None
        allowed = hubs or {"out": OUT_HUBS, "in": IN_HUBS, "analog": ANALOG_HUBS}[role]
        if hub.type not in allowed:
            self.err(path, line, f"hub '{hub.id}' is a {hub.type}, which can't be {ROLE_TEXT[role]} "
                                 f"(use {' / '.join(sorted(allowed))}{'; ' + why if why else ''})")
            return None
        if role == "analog" and hub.common is not None and not hub.common_adc:
            self.err(path, line, f"hub '{hub.id}' common_pin {hub.common.desc} has no ADC, so its "
                                 f"channels can't be analog ({self.plat.adc_text()})")
        if flags & PULLDOWN:
            self.err(path, line, f"a {hub.type} pin has no pull-down")
        if flags & PULLUP and not hub.kind.pullup:
            self.err(path, line, f"a {hub.type} pin has no pull-up (only mcp23017 pins do)")
        desc = f"{hub.id}.{num}"
        self.use(("hub", hub.index, num), desc, Use(path, line))
        return Pin(hub.index + 1, flags, num, desc)

    def hub_pin_num(self, hub, raw, path, line):
        n = None
        if isinstance(raw, int) and not isinstance(raw, bool):
            n = raw
        elif isinstance(raw, str) and hub.type in XY_HUBS and raw.strip().lower() in ("x", "y"):
            n = "xy".index(raw.strip().lower())
        elif isinstance(raw, str):
            s = raw.strip().upper()
            n = hub.kind.pin_name(s)
            if n is None and s.isdigit():
                n = int(s)
        if n is None:
            self.err(path, line, f"'{raw}' is not a pin of hub '{hub.id}' (use {hub.kind.pin_hint(hub)})")
            return None
        if not 0 <= n < hub.n_pins:
            self.err(path, line, f"{hub.kind.what} {n} is out of range for hub '{hub.id}' "
                                 f"({hub.type}{hub.kind.range_text(hub)}: 0..{hub.n_pins - 1})")
            return None
        return n

    def use(self, key, name, u):
        self.uses.setdefault(key, []).append((name, u))

    def matrix_rows(self, d, path, why, hubs=None):
        """A matrix's rows (matrix_rows), for a component."""
        return matrix_rows(self, d, path, why, hubs)

    def matrix_columns(self, d, path, owner):
        """A matrix's columns (matrix_columns), for a component."""
        return matrix_columns(self, d, path, owner)

    # What a control's YAML shares with the rest of the device (the DAW roles, Mackie Control, the
    # MIDI messages), for the control components (components/<control>): the builder's functions.
    def daw_role(self, dev, d, p):
        """d's daw: role (daw_role)."""
        return daw_role(self, dev, d, p)

    @staticmethod
    def id_problem(x, raw):
        """Why x (written raw) can't be an id, or None (id_problem)."""
        return id_problem(x, raw)

    def mcu_name(self, dev, d, p, kind):
        """d's mcu: control of a kind, 'fader' or 'vpot' (mcu_name) -> its number, or None."""
        return mcu_name(self, dev, d, p, kind)

    @staticmethod
    def mcu_table(kind):
        """The Mackie Control names of a kind -> their numbers: 'fader', 'vpot', or 'button' (the notes)."""
        return {"fader": MCU_FADERS, "vpot": MCU_VPOTS, "button": MCU_NOTES}[kind]

    def mcu_needed(self, dev, path, line):
        """The device has mcu: (else an error at path) (mcu_needed)."""
        return mcu_needed(self, dev, path, line)

    def sysex_ref(self, dev, d, key, path):
        """d[key], the id of a midi_messages.sysex string -> its index (sysex_ref)."""
        return sysex_ref(self, dev, d, key, path)

    def bank_value(self, d, p):
        """d's bank: -> the Bank Select value (bank_value)."""
        return bank_value(self, d, p)

    def control_mode(self, dev, d, p, modes):
        """d's mode: -> its index in modes, or None (control_mode)."""
        return control_mode(self, dev, d, p, modes)

    def mode_number(self, dev, d, p, mname, required, keys):
        """analog.cc / encoder.cc from the key the mode takes (mode_number)."""
        return mode_number(self, dev, d, p, mname, required, keys)

    def control_out(self, d, p, top):
        """channel:, min:, max: -> (min, max, channel) or None (control_out)."""
        return control_out(self, d, p, top)

    def check_uses(self):
        """A pin used twice is an error, unless every use is a hub control line of the same kind
        (clock with clock, ...) on different hubs, and each sets allow_other_uses: true."""
        for key, lst in self.uses.items():
            if len(lst) < 2:
                continue
            name, first = lst[0]
            us = [u for _, u in lst]
            same_line = all(u.line_kind for u in us) and len({u.line_kind for u in us}) == 1 and \
                len({u.owner for u in us}) == len(us)
            if same_line and all(u.share for u in us):
                continue
            for u in us[1:]:
                hint = ""
                if same_line:
                    hint = " (to share a hub control line between hubs, set allow_other_uses: true on each use)"
                elif all(u.share for u in us):
                    hint = (" (allow_other_uses only shares the same control line between hubs, e.g. "
                            "clock_pin with clock_pin)")
                self.err(u.path, u.line, f"{name} is already used by {first.path} (line {first.line}){hint}")


# hardware_scan: (core/README.md "Hardware scan"): platforms whose timers and DMA can scan the
# matrix rows by themselves. Gaps the STM32F1 needs between its DMA steps within a row: columns
# to input after the select, and (precharge) the read before the next select.
HW_SCAN_PLATFORMS = {"stm32f1"}
HW_SCAN_GAP_NS = 250

_HW_SCAN_SEE = 'core/README.md "Hardware scan"'
_HW_TIMERS_SEE = 'core/README.md "DMA channels (stm32f1)"'
# row_time and settle in us, to 1 ns (validate_hw_scan checks them against each other and the gaps)
HW_SCAN_MATRIX_KEYS = _Keys(
    _Key("row_time", _Period(0.5, 50, step_us=0.001, example="3.5us"), required=True, see=_HW_SCAN_SEE,
         doc="The time per row: a frame is every row once (an odd row count gets its last row twice)."),
    _Key("settle", _Period(0.25, 49.5, step_us=0.001, example="3us"), required=True, see=_HW_SCAN_SEE,
         doc="From the row's select to the columns' read; at least 0.5us with `precharge`."),
    _Key("precharge", _Bool(), default=False, see=_HW_SCAN_SEE,
         doc="The columns are driven to their idle level before each row, so the read waits less: needs "
             "diodes or tolerant drivers."),
    _Key("timer", _Raw({}, "`auto`, `tim1`..`tim4`"), default="auto", see=_HW_TIMERS_SEE,
         doc="The timer that paces the scan; `auto` takes the first that leaves the UARTs' DMA channels free."),
)
HW_SCAN_ANALOG_KEYS = _Keys(
    _Key("timer", _Raw({}, "`auto`, `tim2`..`tim4`"), default="auto", see=_HW_TIMERS_SEE,
         doc="The timer that steps the muxes and starts ADC1 (not TIM1)."),
)
HW_SCAN_KEYS = _Keys(
    _Key("matrix", _Map(HW_SCAN_MATRIX_KEYS), see=_HW_SCAN_SEE,
         doc="The key matrix and every switch matrix, scanned row by row by a timer and DMA."),
    _Key("analog", _OneOf(_Bool(), _Map(HW_SCAN_ANALOG_KEYS)), default=False, see=_HW_SCAN_SEE,
         doc="The analog inputs and pads, read in sweeps of mux steps and ADC conversions by a timer and DMA; "
             "`true`, or a mapping to pick the timer."),
)
# The "scanning" area (core/yaml/scanning.md)
SCAN_AREA = _Keys(
    _Key("scan_interval", _Period(1, 0xFFFF), default="125us", see='core/README.md "Software scan"',
         doc="The scan tick's period: the key matrix (without `hardware_scan`), encoders on MCU pins and "
             "shift-register hubs are read every tick."),
    _Key("hardware_scan", _Map(HW_SCAN_KEYS), see=_HW_SCAN_SEE,
         doc="stm32f1: the matrices and the analog inputs scanned by timers and DMA instead of the CPU."),
)


def validate_hw_scan(v, dev, data, plat):
    if "hardware_scan" not in data:
        return
    line = data.line_of("hardware_scan")
    hs = v.mapping(data["hardware_scan"], "hardware_scan", line, HW_SCAN_KEYS.names)
    if hs is None:
        return
    if plat.name not in HW_SCAN_PLATFORMS:
        v.err("hardware_scan", line, f"not available on {plat.name} (only on: "
                                     f"{', '.join(sorted(HW_SCAN_PLATFORMS))})")
        return
    if "analog" in hs:
        a = hs["analog"]
        if isinstance(a, dict):         # `analog: { timer: tim3 }`
            if v.mapping(a, "hardware_scan.analog", hs.line_of("analog"), HW_SCAN_ANALOG_KEYS.names) is not None:
                dev.hw_adc = True
                hw_timer(v, dev, a, "analog")
        else:
            dev.hw_adc = v.boolean(hs, "analog", "hardware_scan")
        if dev.hw_adc and "analog" not in data and "pads" not in data:
            v.err("hardware_scan.analog", hs.line_of("analog"), "needs analog: inputs or pads: to read")
            dev.hw_adc = False
    if "matrix" not in hs:
        return
    path, mline = "hardware_scan.matrix", hs.line_of("matrix")
    mx = v.mapping(hs["matrix"], path, mline, HW_SCAN_MATRIX_KEYS.names)
    if mx is None:
        return
    hw_timer(v, dev, mx, "matrix")
    if "key_matrix" not in data and "switch_matrix" not in data:
        v.err(path, mline, "needs a key_matrix: or switch_matrix: to scan")
        return
    for k in ("row_time", "settle"):
        if k not in mx:
            v.err(f"{path}.{k}", mx.line, f"is required (e.g. `{k}: "
                                          f"{'3.5us' if k == 'row_time' else '3us'}`)")
    row = HW_SCAN_MATRIX_KEYS.get(v, mx, "row_time", path)
    settle = HW_SCAN_MATRIX_KEYS.get(v, mx, "settle", path)
    pre = HW_SCAN_MATRIX_KEYS.get(v, mx, "precharge", path)
    if row is None or settle is None:
        return
    lo = HW_SCAN_GAP_NS * (2 if pre else 1)
    if pre and settle < 2 * HW_SCAN_GAP_NS:
        v.err(f"{path}.settle", mx.line_of("settle"), f"{settle / 1000:g}us is too short with "
              f"precharge: (at least {2 * HW_SCAN_GAP_NS / 1000:g}us: the columns go back to inputs "
              f"{HW_SCAN_GAP_NS / 1000:g}us after the select)")
        return
    if settle + lo > row:
        v.err(f"{path}.row_time", mx.line_of("row_time"),
              f"{row / 1000:g}us leaves no room after the {settle / 1000:g}us settle: at least "
              f"{(settle + lo) / 1000:g}us{' with precharge' if pre else ''}")
        return
    dev.hw_scan = (row, settle, pre)


def hw_timer(v, dev, d, scan):
    """hardware_scan.<scan>.timer: auto (the default) or tim1..tim4 -> dev.hw_timer[scan]."""
    path = f"hardware_scan.{scan}.timer"
    if "timer" not in d:
        dev.hw_timer[scan] = (None, path, d.line)
        return
    x, line = d["timer"], d.line_of("timer")
    ok = ["auto"] + [f"tim{n}" for n in sorted(HW_SCAN_TIMERS[scan])]
    name = x.lower() if isinstance(x, str) else None
    if name == "tim1" and scan == "analog":
        v.err(path, line, "the analog scan can't use TIM1: its ADC triggers (CC1..CC3) drive the "
                          f"compare outputs, and PA9/PA10 are USART1's (valid: {', '.join(ok)})")
    elif name not in ok:
        close = difflib.get_close_matches(str(x), ok, 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(path, line, f"unknown timer '{d.raw_of('timer')}' ({hint}valid: {', '.join(ok)})")
    dev.hw_timer[scan] = (int(name[3:]) if name in ok and name != "auto" else None, path, line)


def fmt_us(us):
    if us >= 1000 and us % 100 == 0:
        return f"{us / 1000:g}ms"
    return f"{us:g}us"


# ---------------------------------------------------------------------------------------------
# ids and display names

def id_problem(v, raw):
    """Why v (loaded from the source text raw) can't be an id, or None if it can."""
    if isinstance(v, bool):
        return (f"`{raw}` is read by YAML as the boolean {str(v).lower()} (YAML 1.1, which PyYAML follows, "
                "reads yes/no/on/off/true/false as booleans), so it can't be an id")
    if v is None:
        if not raw:
            return "is empty"
        return f"`{raw}` is read by YAML as null (nothing), so it can't be an id"
    if isinstance(v, (int, float)):
        return f"`{raw}` is read by YAML as the number {v!r}; an id must start with a letter"
    if not isinstance(v, str):
        return f"must be a name (got {type_name(v)})"
    if v.lower() in YAML_WORDS:
        return (f"'{v}' can't be an id: written without quotes, YAML reads it as a "
                f"{'null' if v.lower() == 'null' else 'boolean'} (yes/no/on/off/y/n/true/false/null)")
    if not ID_RE.fullmatch(v):
        if C_IDENT_RE.fullmatch(v):
            return f"'{v}' must be lower_snake_case (a-z, 0-9 and _, starting with a letter)"
        return f"'{v}' isn't a valid id: a-z, 0-9 and _ only, starting with a letter"
    if v in CPP_KEYWORDS:
        return f"'{v}' is a C/C++ keyword, so it can't be an id"
    return None


def derive_id(name):
    """ESPHome's id from a name: lowercase, every run of other characters -> '_'."""
    return re.sub(r"[^a-z0-9]+", "_", str(name).lower()).strip("_")


def suggest_id(text, kind="item"):
    """A valid lower_snake_case id made from any text: "Oct - LED" -> "oct_minus_led"."""
    s = re.sub(r"(?<![^\s])\+(?![^\s])", " plus ", str(text))
    s = re.sub(r"(?<![^\s])-(?![^\s])", " minus ", s)
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s)
    s = derive_id(s)
    base = example_id(kind)
    if not s:
        return base
    if not s[0].isalpha():
        s = f"{base}_{s}"
    if s in YAML_WORDS or s in CPP_KEYWORDS:
        s = f"{s}_{base}"
    return s


def example_id(kind):
    return {"button": "button_1", "LED": "led_1", "analog input": "volume", "encoder": "encoder_1",
            "hub": "faders", "pad": "pad_1", "i2c bus": "bus_a"}.get(kind, "item")


def humanize(ident):
    """The default display name of an id: oct_down -> "Oct down", fader_1 -> "Fader 1"."""
    s = " ".join(ident.replace("_", " ").split())
    return s[:1].upper() + s[1:]


# ---------------------------------------------------------------------------------------------
# The device's own top-level keys as data (components/keys.py), by area: each area is a page of the docs'
# "Core" section (core/yaml/<area>.md, its key table where `<!-- keys -->` is), and the JSON schema types
# those keys from it (gemifi_schema.py). The components' keys are their own (KEY_SECTIONS, spec).

DEVICE_KEYS = _Keys(
    _Key("gemifi", _Raw({"enum": [FORMAT_VERSION]}, f"`{FORMAT_VERSION}`"), required=True,
         doc=f"The format version of the file: {FORMAT_VERSION}."),
    _Key("name", _Str(NODE_NAME_RE.pattern, "node name"), required=True,
         doc="The device's node name: lowercase letters, digits and -, at most 31 (`keystation-mini32`)."),
    _Key("friendly_name", _Str(), shown_default="`name`",
         doc="Its display name: the USB product string, HELLO and the web page."),
    _Key("description", _Str(), doc="A line about it, for people reading the file."),
    _Key("project", _Map(_Keys(
        _Key("version", _Raw(_T("string", pattern=r"^\d+\.\d+\.\d+$"), "major.minor.patch"),
             shown_default="the build's", doc="The firmware version it reports (HELLO), major.minor.patch."))),
         doc="The firmware project: its version."),
    _Key("board_id", _Hex(0, 0x3FFF), required=True, see='core/README.md "Board profiles"',
         doc="The id a firmware image is checked against before it is flashed: a board profile's fixed one, or "
             "your own for a DIY board."),
    _Key("platform", _Enum(PLATFORMS), required=True, doc="The MCU family it builds for (the docs' Platforms)."),
    _Key("chip", _Str(), required=True, doc="The MCU's part number (`STM32F103RC`, `NUC123LD4AN0`, `esp32`)."),
    _Key("board", _Raw({"enum": [b for p in gbd.PLATFORMS for b in gbd.boards(p)]}, "board profile name"),
         shown_default="the platform's: " + ", ".join(f"`{b}` ({p})" for p, b in gbd.DEFAULT_BOARD.items()),
         see='core/README.md "Board profiles"',
         doc="The board profile (platform/<platform>/boards/<board>.json): its layout and board pins."),
)


# MIDI and USB (the "midi" area, with din_midi: and midi_messages:, below)
USB_KEYS = _Keys(
    _Key("vid", _Hex(0, 0xFFFF), required=True, doc="The USB vendor id (`0x1209`: pid.codes)."),
    _Key("pid", _Hex(0, 0xFFFF), required=True, doc="The USB product id."),
    _Key("daw_port", _Bool(), shown_default="`true` with `mcu:`",
         see='core/README.md "Second USB port and Mackie Control"',
         doc="A second USB-MIDI port, `<friendly_name> DAW`, for Mackie Control: needs `mcu:` and USB MIDI."),
)
MIDI_KEYS = _Keys(
    _Key("transport", _Enum(MIDI_TRANSPORT_MACRO),
         shown_default="`usb`; " + ", ".join(f"`{p.default_transport}` on {n}" for n, p in PLATFORMS.items()
                                             if p.default_transport != "usb"),
         doc="How the device talks MIDI: `usb` (native USB, ESP32-S2/S3), `serial` (a UART) or `ble` (ESP32, "
             "ESP32-S3). Only the ESPs have a choice."),
)


def validate(data, file=None):
    """data: the loaded YAML (LDict). Returns (Device or None, [Diag])."""
    v = _V(file)
    dev = Device()
    if not isinstance(data, dict):
        v.err("", getattr(data, "line", 1), f"the top level must be a mapping (gemifi: {FORMAT_VERSION}, "
                                            "name: ..., ...)")
        return None, v.diags
    v.mapping(data, "", data.line, TOP_KEYS, renamed=RENAMED_TOP)

    fmt = v.integer(data, "gemifi", "", 0, 1000, required=True)
    if fmt == 1:
        v.err("gemifi", data.line_of("gemifi"), f"this is a format 1 file; this builder reads format "
                                                 f"{FORMAT_VERSION}: fix the errors below (each says what the "
                                                 "old key became; core/README.md \"Device YAML\" has the format) "
                                                 f"and set gemifi: {FORMAT_VERSION}")
    elif fmt is not None and fmt != FORMAT_VERSION:
        v.err("gemifi", data.line_of("gemifi"), f"format version {fmt} is not supported (this builder "
                                                 f"reads version {FORMAT_VERSION})")
    validate_names(v, dev, data)
    dev.description = v.string(data, "description", "")
    dev.board_id = DEVICE_KEYS.get(v, data, "board_id", "") or 0
    dev.version = validate_version(v, data)
    pname = data.get("platform")
    if pname is None:
        v.err("platform", data.line, f"is required ({' | '.join(PLATFORMS)})")
        return None, v.diags
    if pname not in PLATFORMS:
        close = difflib.get_close_matches(str(pname), list(PLATFORMS), 1)
        hint = f"; did you mean '{close[0]}'?" if close else ""
        v.err("platform", data.line_of("platform"), f"unknown platform '{pname}' "
                                                    f"(known: {', '.join(PLATFORMS)}{hint})")
        return None, v.diags
    v.plat = plat = PLATFORMS[pname]
    dev.platform = plat
    dev.chip = v.string(data, "chip", "", required=True)
    if dev.chip and plat.name == "nuvoton-nuc1xx" and not re.fullmatch(r"NUC1\d\d\w+", dev.chip, re.I):
        v.warn("chip", data.line_of("chip"), f"'{dev.chip}' doesn't look like a NUC1xx part number")
    if dev.chip and plat.name == "stm32f1" and not re.fullmatch(r"(STM32|GD32|APM32|CKS32)F1\d\d\w*",
                                                                 dev.chip, re.I):
        v.warn("chip", data.line_of("chip"), f"'{dev.chip}' doesn't look like an STM32F1-class part number")
    dev.board_given = "board" in data
    v.board = dev.board = validate_board(v, dev, data)

    # midi
    midi = data.get("midi") or LDict()
    if "midi" in data and v.mapping(midi, "midi", data.line_of("midi"), MIDI_KEYS.names) is None:
        midi = LDict()
    tr = midi.get("transport", plat.default_transport)
    if tr not in plat.transports:
        v.err("midi.transport", midi.line_of("transport") if "transport" in midi else data.line,
              f"'{tr}' is not available on {plat.name} (use {' | '.join(plat.transports)})")
        tr = plat.default_transport
    v.transport = dev.transport = tr

    # usb
    dev.usb = (0, 0)
    daw_port = None
    if "usb" in data:
        usb = v.mapping(data["usb"], "usb", data.line_of("usb"), USB_KEYS.names)
        if usb is not None:
            dev.usb = (USB_KEYS.get(v, usb, "vid", "usb") or 0, USB_KEYS.get(v, usb, "pid", "usb") or 0)
            if "daw_port" in usb:
                daw_port = (USB_KEYS.get(v, usb, "daw_port", "usb"), usb.line_of("daw_port"))
            if tr != "usb":
                v.warn("usb", data.line_of("usb"), f"ignored: midi.transport is {tr}")
    elif tr == "usb":
        dev.usb = DEFAULT_USB
        v.warn("usb", data.line, "not given: using the pid.codes test IDs 1209:0002 (development only)")
    validate_feedback(v, dev, data)
    validate_mcu(v, dev, data, tr, daw_port)
    validate_din(v, dev, data, tr, plat)

    dev.scan_us = SCAN_AREA.get(v, data, "scan_interval", "", default=125)
    validate_hw_scan(v, dev, data, plat)

    # hubs (before anything that refers to them), in file order
    for key in data:
        if key in HUB_LISTS:
            lst = v.seq(data[key], key, data.line_of(key))
            for i, h in enumerate(lst):
                p, line = f"{key}[{i}]", lst.line_of(i)
                typ = hub_platform(v, key, h, p, line) if key in HUB_DOMAINS else key
                if typ is not None:
                    validate_hub(v, dev, h, typ, p, line)
    if len(dev.hubs) > 254:
        v.err("", data.line, f"{len(dev.hubs)} hubs: at most 254")
    for t in HUB_DEFS.values():
        hubs = [h for h in dev.hubs if h.type == t.key]
        if hubs:
            t.final_validate(v, dev, hubs)
    validate_screens(v, dev, data)

    # i2c
    on_i2c = [h for h in dev.hubs if h.type in I2C_HUBS] + [x for x in dev.screens if x.kind.i2c]
    need_i2c = bool(on_i2c)
    if need_i2c and not plat.i2c:
        for h in on_i2c:
            v.err(h.path, h.line, f"{h.type} isn't supported on {plat.name} yet (no I2C driver: "
                                  "the platform's hal_i2c_* are stubs)")
        need_i2c = False        # don't also ask for the i2c: pins
    i2c_buses.validate_buses(v, dev, data, on_i2c if need_i2c else [], plat.i2c_buses)

    preset_slots(v, dev, data)
    validate_midi_messages(v, dev, data)
    leds.validate(v, dev, data)
    buttons.validate(v, dev, data)
    analog.validate(v, dev, data)
    encoders.validate(v, dev, data)
    check_mcu_unique(v, dev, data)
    pads.validate(v, dev, data)
    validate_displays(v, dev, data)

    key_matrix.validate(v, dev, data)

    # boot combo: button ids
    if data.get("boot_combo") is not None:
        bc = v.seq(data["boot_combo"], "boot_combo", data.line_of("boot_combo"))
        for i in range(len(bc)):
            p, ln = f"boot_combo[{i}]", bc.line_of(i)
            idx = v.ref(bc, i, p, ln, "button", "buttons")
            if idx is None:
                continue
            if idx >= 32:
                v.err(p, ln, f"button '{bc[i]}' is button {idx}; boot_combo can only use the first 32 buttons")
            elif idx in dev.boot_combo:
                v.warn(p, ln, f"'{bc[i]}' is listed twice")
            else:
                dev.boot_combo.append(idx)

    validate_safety(v, dev, data)
    modes.validate(v, dev, data)
    buttons.validate_gestures(v, dev, data)
    validate_presets(v, dev, data)
    validate_layout(v, dev, data)
    validate_write_to_flash(v, dev, data)
    if not dev.pads:
        for p, line, act in dev.pad_action_uses:
            v.err(p, line, f"action {act} moves the pads' notes, and this device has no pads:")
    msg_features(v, dev, data)
    check_budget(v, dev, data)

    if len(dev.buttons) > 64:
        v.err("buttons", data.line_of("buttons"), f"{len(dev.buttons)} buttons: at most 64 (the runtime "
                                                  "keeps button state in a 64-bit mask)")
    for kind, lst in (("buttons", dev.buttons), ("leds", dev.leds), ("analog", dev.analogs),
                      ("encoders", dev.encoders), ("pads", dev.pads)):
        if len(lst) > 255:
            v.err(kind, data.line_of(kind), f"{len(lst)} items: at most 255")

    if dev.mcu is not None and dev.mcu.cable == 0 and (
            dev.matrix or any(b.action_name == "note" for b in dev.buttons) or buttons.lists_send_notes(dev)):
        v.warn("mcu", dev.mcu.line, "Mackie Control on the only port: hosts read every Note On there as an "
                                    "MCU button, keys included (a USB DAW port avoids that: usb.daw_port)")
    for h in dev.hubs:
        if not h.used:
            v.warn(h.path, h.line, f"hub '{h.id}' is not used by any component")
    v.check_uses()
    allocate_dma(v, dev, data)
    if not (dev.buttons or dev.analogs or dev.encoders or dev.matrix or dev.pads):
        v.warn("", data.line, "the device has no inputs (buttons, analog, encoders, pads or key_matrix)")
    return (None if errors(v.diags) else dev), v.diags


def hub_types_mask(dev):
    """GM_HUB_TYPES: a bit per hub type the device has (bit n = GM_HUB_* value n)."""
    m = 0
    for h in dev.hubs:
        m |= 1 << h.kind.num
    return m


def tim_req(t, q):
    """Timer t's DMA request q (0 = update, k = compare k) -> its name in STM32F1_DMA."""
    return f"TIM{t}_" + ("UP" if q == 0 else f"CH{q}")


def tim_requests(t):
    """'UP 7, CH1 1, CH2 4, CH3 5': timer t's DMA requests and their channels, for messages."""
    return ", ".join(f"{n[5:]} {ch}" for n, ch in STM32F1_DMA.items() if n.startswith(f"TIM{t}_"))


def scan_options(dev, scan, t, pre=None):
    """Timer t's ways to run a hardware scan, in the order auto tries them:
    [(scan, t, {role: request}, [(channel, request, role)])]. Within one option the channels differ."""
    out = []
    if scan == "analog":
        for q in HW_ADC_MUX.get(t, []):
            out.append((scan, t, {"mux": q}, [(1, "ADC1", "ADC1"), (STM32F1_DMA[tim_req(t, q)], tim_req(t, q),
                                                                      "mux")]))
        return out
    roles = ["sel", "in", "rd", "pre"] if (dev.hw_scan[2] if pre is None else pre) else ["sel", "rd"]
    for reqs in itertools.product(*(HW_SCAN_ROLES[r] for r in roles)):
        names = [tim_req(t, q) for q in reqs]
        if len(set(reqs)) < len(reqs) or any(n not in STM32F1_DMA for n in names):
            continue
        chans = [STM32F1_DMA[n] for n in names]
        if len(set(chans)) == len(chans):
            out.append((scan, t, dict(zip(roles, reqs)), list(zip(chans, names, roles))))
    return out


def scan_choices(dev, scans, auto=False, pre=None):
    """Per scan, its options on its YAML timer (every timer with auto, or when `auto`)."""
    return [[o for t in ([dev.hw_timer.get(s, (None,))[0]] if not auto and dev.hw_timer.get(s, (None,))[0]
                         else HW_SCAN_TIMERS[s])
             for o in scan_options(dev, s, t, pre)] for s in scans]


def dma_search(choices, taken):
    """The first pick of one option per scan (choices in order) with its own timer and channels, none
    of them in `taken`; or None."""
    for pick in itertools.product(*choices):
        timers = [o[1] for o in pick]
        chans = [ch for o in pick for ch, _r, _role in o[3]]
        if len(set(timers)) == len(timers) and len(set(chans)) == len(chans) and not set(chans) & taken:
            return pick
    return None


def pick_text(pick):
    return ", ".join(f"{o[0]} on TIM{o[1]}" for o in pick)


def allocate_dma(v, dev, data):
    """stm32f1: give each DMA user its channels -> dev.dma_map [(channel, request, user)] and the
    timers -> dev.scan_timers. The serial users' requests have fixed channels (STM32F1_DMA); the hardware
    scans pick a timer (hardware_scan.*.timer, or the first that fits with auto) whose requests land
    on free ones. When nothing fits, the clash is reported on the optional user (a dma: switch)."""
    dev.dma_map, dev.scan_timers = [], {}
    if dev.platform.name != "stm32f1":
        return
    serial = []                         # (user, path, line, requests, how to free it)
    for h in dev.hubs:
        if h.type == "maudio_pads" and h.dma:
            line = h.raw.line_of("dma") if "dma" in h.raw else h.line
            serial.append((f"maudio_pads '{h.id}'", f"{h.path}.dma", line, ["USART2_TX"],
                           "set dma: false on it (interrupt-driven)"))
        if h.type == "ws2812" and h.data.hub == 0 and h.data.num in Stm32F1Platform.LEDSTRIP_SPI:
            spi = Stm32F1Platform.LEDSTRIP_SPI[h.data.num]
            other = {1: "PB15 (SPI2, DMA1 channel 5)", 2: "PA7 (SPI1, DMA1 channel 3)"}[spi]
            line = h.raw.line_of("pin") if "pin" in h.raw else h.line
            serial.append((f"ws2812 '{h.id}'", f"{h.path}.pin", line, [f"SPI{spi}_TX"], f"move it to {other}"))
    if dev.din is not None and dev.din.dma:
        d = data["din_midi"]
        serial.append(("din_midi", "din_midi.dma", d.line_of("dma"),
                       ["USART1_TX"] + (["USART1_RX"] if dev.din.rx is not None else []),
                       "set din_midi dma: false (interrupt-driven, the default)"))
    taken = {}
    for user, path, line, reqs, how in serial:
        for r in reqs:
            ch = STM32F1_DMA[r]
            if ch in taken:             # two fixed requests on one channel (an LED strip's SPI and DIN)
                other, oreq = taken[ch]
                v.err(path, line, f"needs DMA1 channel {ch} ({r}), which {other} uses ({oreq}): {how}")
                return
            taken[ch] = (user, r)
    scans = [s for s, on in (("matrix", dev.hw_scan), ("analog", dev.hw_adc)) if on]
    pick = dma_search(scan_choices(dev, scans), set(taken))
    if pick is None:
        dma_clash(v, dev, scans, serial, taken)
        return
    for scan, t, roles, claims in pick:
        dev.scan_timers[scan] = (t, roles)
        for ch, r, role in claims:
            dev.dma_map.append((ch, r, f"hardware_scan.{scan}" + (f" {HW_SCAN_ROLE_NAMES[role]}"
                                                                    if role in HW_SCAN_ROLE_NAMES else "")))
    used = {o[1] for o in pick}
    dev.scan_timers["tick"] = next(t for t in TICK_TIMERS if t not in used)
    dev.dma_map += [(STM32F1_DMA[r], r, user) for user, _p, _l, reqs, _h in serial for r in reqs]
    dev.dma_map.sort()


def dma_clash(v, dev, scans, serial, taken):
    """allocate_dma found no fit: say why."""
    alone = dma_search(scan_choices(dev, scans), set())
    if alone is None:
        # Only explicit timers do this (TIM1 + TIM4 always fit): two on one timer, or one whose
        # requests can't make a scan.
        (mt, mpath, mline), (at, apath, aline) = dev.hw_timer.get("matrix", (None, "", 0)), \
            dev.hw_timer.get("analog", (None, "", 0))
        auto = dma_search(scan_choices(dev, scans, auto=True), set(taken))
        hint = f" (timer: auto picks {pick_text(auto)})" if auto else ""
        if mt is not None and mt == at:
            v.err(apath, aline, f"TIM{at} is hardware_scan.matrix's timer too: each scan needs its own{hint}")
        elif mt is not None and not dma_search(scan_choices(dev, ["matrix"]), {1} if dev.hw_adc else set()):
            k = len(scan_options(dev, "matrix", 1)[0][2])
            steps = ", ".join(HW_SCAN_ROLE_NAMES[r] for r in scan_options(dev, "matrix", 1)[0][2])
            chans = [ch for n, ch in STM32F1_DMA.items() if n.startswith(f"TIM{mt}_")]
            notes = [f"two share channel {c}" for c in sorted(set(chans)) if chans.count(c) > 1]
            if dev.hw_adc and 1 in chans:
                notes.append("channel 1 is ADC1's, for hardware_scan.analog")
            note = f" ({'; '.join(notes)})" if notes else ""
            v.err(mpath, mline, f"TIM{mt} can't pace the matrix scan: it needs {k} DMA requests of one "
                                f"timer on different channels ({steps}; only the select may be the "
                                f"update), and TIM{mt} has {tim_requests(mt)}{note}{hint}")
        else:
            v.err(apath if at is not None else mpath, aline if at is not None else mline,
                  f"hardware_scan.matrix on {f'TIM{mt}' if mt else 'auto'} and hardware_scan.analog on "
                  f"{f'TIM{at}' if at else 'auto'} can't both have their DMA channels{hint}")
        return
    # The scans fit by themselves: the serial users clash with every way they can run. Report the
    # clashes with the first, and what would fit instead.
    hint = ""
    explicit = any(dev.hw_timer.get(s, (None,))[0] for s in scans)
    auto = explicit and dma_search(scan_choices(dev, scans, auto=True), set(taken))
    if auto:
        hint = f"; or set the hardware_scan timers to auto (it finds: {pick_text(auto)})"
    elif dev.hw_scan and dev.hw_scan[2] and dma_search(scan_choices(dev, scans, auto=True, pre=False),
                                                        set(taken)):
        hint = ("; or turn hardware_scan.matrix precharge off: the scan then needs 2 channels "
                "instead of 4, and that fits (set a longer row_time and settle)")
    elif scans:
        hint = "; no hardware_scan timer choice leaves it free"
    users = {ch: (f"hardware_scan.{o[0]}", r) for o in alone for ch, r, _role in o[3]}
    for user, path, line, reqs, how in serial:
        for r in reqs:
            ch = STM32F1_DMA[r]
            if ch in users:
                other, oreq = users[ch]
                v.err(path, line, f"needs DMA1 channel {ch} ({r}), which {other} uses ({oreq}): "
                                  f"{how}, or turn {other} off{hint}")
                hint = ""                   # once is enough


def dma_text(dev):
    """'1 ADC1 (hardware_scan.analog), 3 TIM1_CH2 (...), ...; free: 2, 6' for the build output."""
    if dev.platform.name != "stm32f1":
        return ""
    used = ", ".join(f"{ch} {r} ({u})" for ch, r, u in dev.dma_map) or "none"
    free = [str(c) for c in range(1, STM32F1_DMA_CHANNELS + 1) if c not in {ch for ch, _r, _u in dev.dma_map}]
    return f"DMA1 channels: {used}; free: {', '.join(free) or 'none'}"


def timers_text(dev):
    """'timers: TIM1 hardware_scan.matrix, TIM2 scan tick, ...' for the build output."""
    if not dev.scan_timers:
        return ""
    names = {"matrix": "hardware_scan.matrix",
             "analog": "hardware_scan.analog (ADC1 started by {trig})", "tick": "scan tick"}
    ts = sorted((t if s == "tick" else t[0], s) for s, t in dev.scan_timers.items())
    return "timers: " + ", ".join(f"TIM{t} " + names[s].format(trig=HW_ADC_TRIGGER.get(t)) for t, s in ts)


_OWNERS_SEE = 'core/README.md "LED ownership"'
FEEDBACK_KEYS = _Keys(
    _Key("enabled", _Bool(), default=True,
         doc="`false`: ignore everything the host sends but the gemifi SysEx protocol (and Mackie Control, with "
             "`mcu:`); DAW scripts are off too."),
    _Key("script", _Bool(), default=True, see='core/README.md "DAW scripts (F3)"',
         doc="DAW scripts (F3): GET_COMPONENT, EVENT, BOOTED and the FB_* messages; `false` leaves them out."),
    # led_owner's message is its own ("unknown owner"), and led_timeout takes `never`: validate_feedback
    _Key("led_owner", _Enum(LED_OWNERS), default="host_then_local", see=_OWNERS_SEE,
         doc="Which state every LED shows, the host's or its own, unless the LED's `owner:` says."),
    _Key("led_timeout", TIMEOUT, default=f"{DEFAULT_LED_TIMEOUT_MS // 1000}s", see=_OWNERS_SEE,
         doc="With `host_then_local`: how long the host's last word for an LED lasts, unless the LED's "
             "`timeout:` says; `never`: from the host's first message on."),
)
MCU_KEYS = _Keys(
    _Key("extender", _Bool(), default=False,
         doc="Answer as an MCU XT (extender): SysEx device id 0x15 instead of 0x14."),
)
# The "feedback" area: DAW feedback (core/yaml/feedback.md)
FEEDBACK_AREA = _Keys(
    _Key("feedback", _OneOf(_Bool(), _Map(FEEDBACK_KEYS)), see='core/README.md "DAW feedback"',
         doc="What the device does with the MIDI the host sends back (F1) and DAW scripts (F3); `true` / "
             "`false`: as `enabled:`, the rest by default."),
    _Key("mcu", _OneOf(_Null(), _Map(MCU_KEYS)), see='core/README.md "Second USB port and Mackie Control"',
         doc="Mackie Control: on USB on a second MIDI port (`usb.daw_port`), on serial or BLE the whole device."),
)


def validate_feedback(v, dev, data):
    """feedback: true | false | { enabled, script, led_owner, led_timeout } (all optional): F1 host feedback
    on the main port, and (script) the DAW-script messages of sysex.md 4.6."""
    if data.get("feedback") is None:
        return
    if isinstance(data["feedback"], bool):      # the short form of enabled:
        dev.feedback = dev.host_fb = data["feedback"]
        return
    fb = v.mapping(data["feedback"], "feedback", data.line_of("feedback"), FEEDBACK_KEYS.names)
    if fb is None:
        return
    dev.feedback = FEEDBACK_KEYS.get(v, fb, "enabled", "feedback")
    dev.host_fb = dev.feedback and FEEDBACK_KEYS.get(v, fb, "script", "feedback")
    if fb.get("led_owner") is not None:
        o = LED_OWNERS.get(fb["led_owner"])
        if o is None:
            v.err("feedback.led_owner", fb.line_of("led_owner"),
                  f"unknown owner '{fb.raw_of('led_owner')}' (valid: {', '.join(LED_OWNERS)})")
        else:
            dev.led_owner = o
    dev.led_timeout = validate_timeout(v, fb, "led_timeout", "feedback", dev.led_timeout)
    if not dev.feedback and len(fb) > 1:
        v.warn("feedback", data.line_of("feedback"), "enabled: false: the other feedback settings only "
                                                     "matter for the MCU port")


DIN_KEYS = _Keys(
    _Key("tx_pin", _Pin(), required=True, doc="The UART's TX pin: MIDI Out (stm32f1: PA9, USART1)."),
    _Key("rx_pin", _Pin(), doc="The UART's RX pin: MIDI In (stm32f1: PA10). Without it, MIDI Out only."),
    _Key("mode", _Enum(DIN_MODES), default="usb_merge", see='core/README.md "DIN MIDI"',
         doc="How the jacks reach USB: on the device's own port (`usb_merge`), on a second USB-MIDI port "
             "(`separate_port`), or not at all (`din_only`)."),
    _Key("port_name", _Str(), shown_default="`<friendly_name> MIDI`",
         doc=f"The second port's name, with `separate_port`; at most {COMP_STR_MAX} characters."),
    _Key("thru", _Bool(), default=False, param="midi.thru", doc="DIN In is copied to DIN Out."),
    _Key("controller_out", _Bool(), default=True, param="midi.din_out",
         doc="The device's own messages (keys, pads, CCs, program changes) also go to DIN Out."),
    _Key("usb_to_din", _Bool(), default=False, param="midi.usb_to_din",
         doc="With `usb_merge`: the host's channel and realtime messages on the device's port go to DIN Out."),
    _Key("dma", _Bool(), default=False, see='core/README.md "DMA channels (stm32f1)"',
         doc="stm32f1: the UART's bytes move by DMA (channels 4 and 5) instead of by interrupt."),
)


def validate_din(v, dev, data, tr, plat):
    """din_midi: 5-pin DIN MIDI In / Out on a UART (core/README.md "DIN MIDI"). mode: how the jacks reach
    USB (DIN_KEYS: separate_port hangs Chrome's Web MIDI on Windows for now, see core/README.md)."""
    if "din_midi" not in data:
        return
    line = data.line_of("din_midi")
    d = v.mapping(data["din_midi"], "din_midi", line, DIN_KEYS.names)
    if d is None:
        return
    tx = v.pin(d.get("tx_pin"), "din_midi.tx_pin", d.line_of("tx_pin") if "tx_pin" in d else d.line, "out",
               allow_hub=False)
    rx = None
    if d.get("rx_pin") is not None:
        rx = v.pin(d["rx_pin"], "din_midi.rx_pin", d.line_of("rx_pin"), "in", allow_hub=False)
    mode = DIN_KEYS.get(v, d, "mode", "din_midi", default=DIN_MODES.index("usb_merge"))     # wrong: the default
    if "mode" in d:
        if mode == DIN_MODES.index("separate_port"):
            v.warn("din_midi.mode", d.line_of("mode"),
                   "separate_port: on Windows, Chrome's Web MIDI (the web updater) can hang for good when "
                   "a device with two USB-MIDI ports leaves or comes back; usb_merge has no such problem")
    if tx is not None and (rx is not None or d.get("rx_pin") is None):
        why = plat.din_uart(tx.num, rx.num if rx is not None else None)
        if why:
            v.err("din_midi", line, why)
    if mode != 0 and tr != "usb":
        v.err("din_midi.mode", d.line_of("mode") if "mode" in d else line,
              f"{DIN_MODES[mode]} links the jacks to USB, but midi.transport is {tr} (use din_only)")
    if mode == DIN_MODES.index("separate_port") and dev.daw_port:
        v.err("din_midi.mode", d.line_of("mode") if "mode" in d else line,
              "separate_port needs the second USB port, which the DAW port (mcu:) already uses: "
              "use usb_merge or din_only, or usb.daw_port: false")
    name = DIN_KEYS.get(v, d, "port_name", "din_midi")
    if name is not None and mode != DIN_MODES.index("separate_port"):
        v.warn("din_midi.port_name", d.line_of("port_name"), f"ignored: only separate_port has its own port "
                                                              f"(mode is {DIN_MODES[mode]})")
    if name is not None and len(name) > COMP_STR_MAX:
        v.err("din_midi.port_name", d.line_of("port_name"), f"{len(name)} characters: at most {COMP_STR_MAX}")
    usb = DIN_KEYS.get(v, d, "usb_to_din", "din_midi")
    if "usb_to_din" in d and mode != DIN_MODES.index("usb_merge"):
        v.warn("din_midi.usb_to_din", d.line_of("usb_to_din"),
               "ignored: only usb_merge has it (separate_port always sends its port's messages to DIN Out)")
    if rx is None and d.get("rx_pin") is None and "thru" in d:
        v.warn("din_midi.thru", d.line_of("thru"), "there is no rx_pin (MIDI In) to pass through")
    dev.din = Hub(tx=tx or NO_PIN, rx=rx, mode=mode, port_name=name,
                  thru=DIN_KEYS.get(v, d, "thru", "din_midi"), out=DIN_KEYS.get(v, d, "controller_out", "din_midi"),
                  usb=usb, dma=DIN_KEYS.get(v, d, "dma", "din_midi"))


def validate_mcu(v, dev, data, tr, daw_port):
    """mcu: { extender } turns on Mackie Control. On USB it gets the second cable (usb.daw_port, default
    on with mcu:); on serial / BLE (one port) the whole device speaks MCU."""
    if "mcu" in data:
        m = data["mcu"]
        if m is None:
            m = LDict()
            m.line = data.line_of("mcu")
        m = v.mapping(m, "mcu", data.line_of("mcu"), MCU_KEYS.names)
        if m is not None:
            ext = MCU_KEYS.get(v, m, "extender", "mcu")
            dev.mcu = Hub(extender=ext, cable=0, line=data.line_of("mcu"))
    if daw_port is not None:
        want, line = daw_port
        if want and tr != "usb":
            v.err("usb.daw_port", line, f"a second port needs USB MIDI (midi.transport is {tr})")
        elif want and dev.mcu is None:
            v.err("usb.daw_port", line, "the DAW port carries Mackie Control: add an `mcu:` section "
                                        "(or remove daw_port)")
        dev.daw_port = bool(want) and tr == "usb" and dev.mcu is not None
    else:
        dev.daw_port = dev.mcu is not None and tr == "usb"
    if dev.mcu is not None:
        dev.mcu.cable = 1 if dev.daw_port else 0


def validate_board(v, dev, data):
    """board: the board profile (platform/<platform>/boards/<board>.json, tools/builder/gemifi_board.py).
    Left out: the platform's default board (gbd.DEFAULT_BOARD). -> the loaded profile, or None."""
    plat = dev.platform.name
    given = "board" in data
    line = data.line_of("board") if given else data.line
    if plat not in gbd.PLATFORMS:
        if given:
            v.err("board", line, f"board profiles are for {', '.join(gbd.PLATFORMS)}; on {plat}, chip: names "
                                 "the board")
        return None
    name = data.get("board", gbd.DEFAULT_BOARD[plat])
    known = gbd.boards(plat)
    if not isinstance(name, str) or not name:
        v.err("board", line, f"must be the name of a board profile ({' | '.join(known)})")
        return None
    if name not in known:
        close = difflib.get_close_matches(name, known, 1)
        hint = f"; did you mean '{close[0]}'?" if close else ""
        v.err("board", line, f"unknown board '{name}' for {plat} (known: {', '.join(known)}{hint}; profiles "
                             f"are platform/{plat}/boards/*.json)")
        return None
    try:
        board = gbd.load(plat, name)
    except gbd.ProfileError as e:
        v.err("board", line, str(e))
        return None
    bid_line = data.line_of("board_id") if "board_id" in data else data.line
    fixed = board["board_id"]
    if fixed is not None and dev.board_id != fixed and given:
        # Without board: (every file from before board profiles) this is left to the build
        # (board_mismatch), so test configs that are only generated, never built, stay as they were.
        v.err("board_id", bid_line, board_mismatch(dev, board))
    elif fixed is None and dev.board_id not in gbd.DIY_IDS:
        v.err("board_id", bid_line, f"board '{name}' is a DIY board: its board_id must be in the DIY range "
                                    "0x3F00-0x3FFF (protocol/sysex.md 7), one per device design")
    if board["chip"] and dev.chip and not re.fullmatch(board["chip"], dev.chip, re.I):
        v.warn("chip", data.line_of("chip"), f"'{dev.chip}' isn't the part board '{name}' is made for "
                                             f"({board['chip']}): its flash and RAM layout may not fit")
    return board


def board_mismatch(dev, board=None):
    """Why dev's image can't be built for its board (board_id vs the profile's own), or None."""
    board = board or dev.board
    if board is None or board["board_id"] is None or dev.board_id == board["board_id"]:
        return None
    plat = dev.platform.name
    diy = [b for b in gbd.boards(plat) if gbd.load(plat, b)["board_id"] is None]
    alt = f" (board: {diy[0]})" if diy else ""
    return (f"0x{dev.board_id:04X} isn't the board_id of board '{board['id']}' (0x{board['board_id']:04X}"
            f"{'' if dev.board_given else f', the {plat} default'}), so its image can't be built: use "
            f"board_id: 0x{board['board_id']:04X}, or a DIY board profile{alt} with a DIY board_id (0x3F00-0x3FFF)")


def validate_names(v, dev, data):
    """name: the node name (ESPHome: lowercase letters, digits and -, at most 31); friendly_name: the
    display name (USB product string, HELLO, web UI), default the name."""
    name = data.get("name")
    friendly = v.string(data, "friendly_name", "")
    dev.node_name = "?"
    if name is None:
        v.err("name", data.line, "is required: the device's node name, lowercase letters, digits and - "
                                 "(e.g. `name: keystation-mini32`)")
    else:
        raw = data.raw_of("name")
        if isinstance(name, str) and NODE_NAME_RE.fullmatch(name):
            dev.node_name = name
        else:
            hint = f"; put '{raw}' in friendly_name:" if friendly is None and re.search(r"[A-Z ]", raw) else ""
            v.err("name", data.line_of("name"), f"'{raw}' isn't a node name: lowercase letters, digits and -, "
                                                f"at most 31, not starting or ending with - (e.g. "
                                                f"`name: {suggest_node_name(raw)}`{hint})")
    dev.name = friendly or dev.node_name


def suggest_node_name(text):
    s = re.sub(r"[^a-z0-9]+", "-", str(text).lower()).strip("-")[:31].strip("-")
    return s or "my-controller"


def mcu_needed(v, dev, path, line):
    if dev.mcu is None:
        v.err(path, line, "Mackie Control needs an `mcu:` section at the top level")
        return False
    return True


def mcu_name(v, dev, d, p, kind):
    """A component's `mcu:` (fader_1 / vpot_1 / ...), a Mackie Control name of a kind (mcu_table) -> its
    number, or None."""
    if d.get("mcu") is None:
        return None
    table = v.mcu_table(kind)
    raw, line = d.raw_of("mcu"), d.line_of("mcu")
    if not mcu_needed(v, dev, f"{p}.mcu", line):
        return None
    if d["mcu"] not in table:
        close = difflib.get_close_matches(raw, list(table), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{p}.mcu", line, f"unknown MCU control '{raw}' ({hint}valid: {', '.join(table)})")
        return None
    return table[d["mcu"]]


def daw_role(v, dev, d, p):
    """daw: a role for the DAW scripts (hosts/README.md) -> the string, or None."""
    if d.get("daw") is None:
        return None
    raw, line = d.raw_of("daw"), d.line_of("daw")
    role = d["daw"]
    if isinstance(role, str) and HOST_ROLE_RE.fullmatch(role) and len(role) > COMP_STR_MAX:
        v.err(f"{p}.daw", line, f"DAW role '{raw}' is {len(role)} characters: at most {COMP_STR_MAX}")
        return None
    if not isinstance(role, str) or not (role in DAW_ROLES or DAW_ROLE_RE.fullmatch(role) or
                                         HOST_ROLE_RE.fullmatch(role)):
        close = difflib.get_close_matches(str(raw), sorted(DAW_ROLES), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{p}.daw", line, f"unknown DAW role '{raw}' ({hint}roles: hosts/README.md; x_<name> for "
                                "your own; <host>.<command> for one host's, e.g. zynthian.toggle_audio_play)")
        return None
    if not dev.host_fb:
        v.warn(f"{p}.daw", line, "daw: is only read by DAW scripts, and feedback.script is off")
    return role


def check_mcu_unique(v, dev, data):
    seen = {}
    for kind, lst, table in (("analog", dev.analogs, MCU_FADERS), ("encoders", dev.encoders, MCU_VPOTS)):
        names = {n: k for k, n in table.items()}
        for i, c in enumerate(lst):
            if c.mcu is None:
                continue
            key = (kind, c.mcu)
            if key in seen:
                v.err(f"{kind}[{i}].mcu", data.line_of(kind), f"'{names[c.mcu]}' is already used by {seen[key]}")
            seen.setdefault(key, f"{kind}[{i}]")
    for b in dev.leds:
        if getattr(b, "local_line", None) is not None and not any(k == "button" for k, _ in b.used_by):
            v.warn(f"{b.path}.owner", b.local_line, "owner: local on an LED no button uses: it never lights "
                                                    "(it has no local state)")
        if any(k == "ring" for k, _ in b.used_by) and (len(b.used_by) > 1 or b.source):
            v.warn(b.path, b.line, f"LED '{b.id}' is in a V-Pot ring and also used elsewhere: the ring "
                                   "will overwrite it")


def validate_screens(v, dev, data):
    """display: the screens (components/display), each on a platform (components/<platform>), that text
    displays (displays:, `display: <id>`) draw on."""
    if "display" not in data:
        return
    lst = v.seq(data["display"], "display", data.line_of("display"))
    for i, d in enumerate(lst):
        p = f"display[{i}]"
        if not isinstance(d, dict):
            v.err(p, lst.line_of(i), f"must be a mapping, e.g. {{ id: oled, platform: ssd1306 }} (got {type_name(d)})")
            continue
        plat = d.get("platform")
        kind = DISPLAY_PLATFORMS.get(plat) if isinstance(plat, str) else None
        if kind is None:
            what = f"unknown platform '{plat}'" if plat is not None else "platform: is required"
            v.err(f"{p}.platform", d.line_of("platform") or d.line, f"{what} (use {', '.join(DISPLAY_PLATFORMS)})")
            continue
        v.mapping(d, p, lst.line_of(i), kind.keys)
        index = len(dev.screens)
        sid = v.ident(d, p, "display", index, allow_name=False)
        scr = Hub(id=sid or f"#{index}", type=plat, kind=kind, index=index, path=p, line=d.line, used=False,
                  addr=0, bus=0, width=0, height=0, raw=d)
        kind.validate(v, dev, scr, d, p)
        if sid is not None:
            if sid in v.hub_by_id:
                v.err(f"{p}.id", d.line_of("id"), f"'{sid}' is already a hub's id")
            v.screen_by_id[sid] = scr
        dev.screens.append(scr)
    if len(dev.screens) > 8:
        v.err("display", data.line_of("display"), f"{len(dev.screens)} displays: at most 8")


def hub_platform(v, domain, h, p, line):
    """The hub type item h of a domain's list names with platform: (led_strip: ws2812 ...), or None
    (reported)."""
    plats = HUB_DOMAINS[domain]
    if not isinstance(h, dict):
        v.err(p, line, f"must be a mapping, e.g. {{ id: ..., platform: {next(iter(plats))} }} (got {type_name(h)})")
        return None
    plat = h.get("platform")
    if isinstance(plat, str) and plat in plats:
        return plat
    what = f"unknown platform '{plat}'" if plat is not None else "platform: is required"
    v.err(f"{p}.platform", h.line_of("platform") or line, f"{what} (use {', '.join(plats)})")
    return None


def validate_hub(v, dev, h, typ, p, line):
    index = len(dev.hubs)
    if not isinstance(h, dict):
        v.err(p, line, f"must be a mapping (got {type_name(h)})")
        return
    kind = HUB_DEFS[typ]
    v.mapping(h, p, line, HUB_KEYS[typ], renamed=RENAMED_HUB)
    hid = v.ident(h, p, "hub", index, allow_name=False)
    hub = Hub(id=hid or f"#{index}", type=typ, kind=kind, index=index, path=p, line=line, used=False, chain=0,
              n_sel=0, addr=0, data=NO_PIN, clock=NO_PIN, latch=NO_PIN, sel=[], common=None,
              common_adc=False, enable=NO_PIN, settle_us=0, n_pins=0, en=[], group_lv=[], rows=[], cols=[],
              ram_bytes=0, intensity=0, grids=0, fade=False, touch_min=0, touch_max=0, dma=False, bus=0, raw=h)
    if hid is not None:
        v.hub_by_id[hid] = hub
    dev.hubs.append(hub)

    def ctl(key, role, required=True, open_drain=False):
        if h.get(key) is None:
            if required:
                v.err(f"{p}.{key}", h.line, "is required")
            return NO_PIN
        return v.pin(h[key], f"{p}.{key}", h.line_of(key), role, allow_hub=False, line_kind=kind.lines[key],
                     owner=index, open_drain=open_drain) or NO_PIN

    kind.validate(v, dev, hub, h, p, line, ctl)


def matrix_rows(v, d, path, why, hubs=None):
    """Rows of a key_matrix / switch_matrix: a list of pins (on hubs, by default the hubs that give rows;
    why says why a pin can't be one), or { decoder: <id>, first: n, count: n } for consecutive outputs of a
    decoder hub."""
    hubs = ROW_HUBS if hubs is None else hubs
    rows = d.get("rows")
    line = d.line_of("rows") if "rows" in d else d.line
    p = f"{path}.rows"
    if isinstance(rows, dict) and "decoder" in rows and "number" not in rows:
        m = v.mapping(rows, p, line, DECODER_ROWS_KEYS)
        if m is None:
            return []
        hub = v.hub_by_id.get(m.get("decoder")) if isinstance(m.get("decoder"), str) else None
        if hub is None:
            if v.ref(m, "decoder", p, line, "hub", "hubs") is not None:
                hub = v.hub_by_id.get(m.get("decoder"))
            if hub is None:
                return []
        if hub.type != "decoder":
            v.err(f"{p}.decoder", line, f"hub '{hub.id}' is a {hub.type}, not a decoder")
            return []
        first = v.integer(m, "first", p, 0, 255, default=0)
        count = v.integer(m, "count", p, 1, 255, required=True)
        if count is None:
            return []
        if first + count > hub.n_pins:
            v.err(p, line, f"outputs {first}..{first + count - 1} are out of range for decoder '{hub.id}' "
                           f"(0..{hub.n_pins - 1})")
            return []
        hub.used = True
        pins = []
        for n in range(first, first + count):
            desc = f"{hub.id}.{n}"
            v.use(("hub", hub.index, n), desc, Use(p, line))
            pins.append(Pin(hub.index + 1, 0, n, desc))
        return pins
    if not isinstance(rows, list) or not rows:
        v.err(p, line, "needs a list of pins, or { decoder: <id>, first: n, count: n }")
        return []
    if len(rows) > 255:
        v.err(p, line, f"{len(rows)} rows: at most 255")
    return [v.pin(x, f"{p}[{i}]", rows.line_of(i), "out", hubs=hubs, why=why) or NO_PIN
            for i, x in enumerate(rows)]


def matrix_columns(v, d, path, owner):
    """Columns: MCU input pins (read from the scan interrupt). They may be shared between a
    key_matrix and switch matrices on the same lines, with allow_other_uses on each use."""
    cols = d.get("columns")
    p = f"{path}.columns"
    if not isinstance(cols, list) or not cols:
        v.err(p, d.line_of("columns") if "columns" in d else d.line, "needs a list of pins")
        return []
    if len(cols) > 32:
        v.err(p, d.line_of("columns"), f"{len(cols)} columns: at most 32")
    return [v.pin(x, f"{p}[{i}]", cols.line_of(i), "in", allow_hub=False, line_kind="column", owner=owner)
            or NO_PIN for i, x in enumerate(cols)]


VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def validate_version(v, data):
    """project: { version: X.Y.Z } -> (major, minor, patch), or None when not given (the build's default
    is used). Packed as major << 24 | minor << 16 | patch, so major and minor 0..255, patch 0..65535."""
    if data.get("project") is None:
        return None
    proj = v.mapping(data["project"], "project", data.line_of("project"), PROJECT_KEYS)
    if proj is None or proj.get("version") is None:
        return None
    x = proj["version"]
    line, p = proj.line_of("version"), "project.version"
    m = VERSION_RE.match(x) if isinstance(x, str) else None
    if not m:
        hint = " (quote it: version: \"1.0.0\")" if isinstance(x, (int, float)) and not isinstance(x, bool) else ""
        v.err(p, line, f"must be major.minor.patch, like 0.1.0 (got {x!r}){hint}")
        return None
    ver = tuple(int(g) for g in m.groups())
    if ver[0] > 255 or ver[1] > 255 or ver[2] > 0xFFFF:
        v.err(p, line, f"{x} is out of range (major and minor 0..255, patch 0..65535)")
        return None
    return ver


# ---------------------------------------------------------------------------------------------
# displays (core/README.md "Displays")

TEXT_KEYS = {"display", "line", "x", "scale", "length"}         # type text only
RENAMED_DISPLAY = {"ssd1306": "replaced: name the screen with `display: <id>`, the id of a display: item "
                              "(an OLED is `display: [{ id: oled, platform: ssd1306 }]` now)" + MIGRATE_HINT}
SEGMENT_KEYS = {"chars", "segment_step", "segment_order"}      # segment types only
# gm_display.h GM_DISPLAY_*: the C name, and (pins per character, what they are)
DISPLAY_TYPES = {"segment14": 1, "segment7": 2, "text": 3}
DISPLAY_C = {1: "GM_DISPLAY_SEG14", 2: "GM_DISPLAY_SEG7", 3: "GM_DISPLAY_TEXT"}
DISPLAY_TEXT = 3
TEXT_W, TEXT_H = 6, 8                # a text display's character cell, in pixels (x scale)
DISPLAY_SEGS = {1: (15, "segments a..m and the dot"), 2: (8, "segments a..g and the point")}
# each type's segments in gm_display.h order (segment_order: names them in pin order)
SEG_NAMES = {1: ["a", "b", "c", "d", "e", "f", "g1", "g2", "h", "i", "j", "k", "l", "m", "dot"],
             2: ["a", "b", "c", "d", "e", "f", "g", "dp"]}
MAX_DISPLAY_CHARS = 16               # core/src/display.c MAXC (a segment display)
MAX_TEXT_CHARS = 21                  # a text display: 128 / 6
MAX_DISPLAYS = 4                     # RAM: 4 bytes a character of the widest, plus 9, per display
# gm_display.h GM_SHOWS_*: what a display shows while no host writes to it
DISPLAY_SHOWS = {"last_message": 0, "preset_number": 1, "preset_name": 2, "channel": 3, "none": 4}
DISPLAY_SHOWS_C = {0: "GM_SHOWS_LAST_MESSAGE", 1: "GM_SHOWS_PRESET_NUMBER", 2: "GM_SHOWS_PRESET_NAME",
                   3: "GM_SHOWS_CHANNEL", 4: "GM_SHOWS_NONE"}
_SEG = " A segment display's."
_TEXT = " A text display's."
# One displays: item, checked by validate_displays(): the segment types' keys, then the text type's. The
# ranges of line: and x: (the screen's size), and the type and shows names (their messages), stay as code.
DISPLAY = _Keys(
    _Key("id", _Ref("id", "id"), required=True, doc="What the rest of the file and a host's FB_TEXT call it."),
    _Key("type", _Enum(DISPLAY_TYPES), shown_default="`text` with `display:`, else `segment14`",
         doc="Its characters: 14 or 7 segments on hub pins, or text on a screen."),
    _Key("chars", _List(_OneOf(_Pin(), _List(_Pin()))),
         doc="Each character's first pin, or its segments' pins, left to right (hub pins)." + _SEG),
    _Key("segment_step", _Int(1, 64), default=1,
         doc="Segment k of a character is this many pins times k after its first pin." + _SEG),
    _Key("segment_order", _List(_Enum(sorted({n for ns in SEG_NAMES.values() for n in ns}))),
         shown_default="a, b, ... in gm_display.h order",
         doc="The segments in pin order from each first pin; ones left out aren't wired." + _SEG),
    _Key("shows", _Enum(DISPLAY_SHOWS), shown_default="`last_message` on the first display, else `none`",
         doc="What it shows of its own while no host writes to it."),
    _CONTROL_DAW.but(doc="What a DAW script shows on it (`time`, `track_name`, `param`, `status`)."),
    _Key("display", _Ref("id", "display id"), doc="The screen it is on: a `display:` item's id." + _TEXT),
    _Key("line", _Int(0, 7), default=0,
         doc="The text line its top is on, 8 pixels a line from the top (0..3 on a 32-pixel screen)." + _TEXT),
    _Key("x", _Int(0, 127), default=0, doc="The pixel column it starts at." + _TEXT),
    _Key("scale", _Int(1, 4), default=1,
         doc="Its size: a character is 6 × 8 pixels (with its gap) times this, and this many lines tall." + _TEXT),
    _Key("length", _Int(1, MAX_TEXT_CHARS), shown_default="as many as fit",
         doc="How many characters it has." + _TEXT),
)
DISPLAY_KEYS = DISPLAY.names
# The Core area (core/yaml/displays.md)
DISPLAYS_AREA = _Keys(_Key("displays", _List(_Map(DISPLAY)), see='core/README.md "Displays"',
                           doc=f"The displays, up to {MAX_DISPLAYS}: a row of segment characters on hub pins, or a "
                               "row of text on a screen."))


def validate_displays(v, dev, data):
    """displays: segment displays on hub output pins. Each item of chars: is a character, left to
    right: its first pin (segment a; the others are the pins after it, or every segment_step pins),
    or the list of its segments' pins, in segment order."""
    lst = v.seq(data.get("displays"), "displays", data.line_of("displays"))
    for i, d in enumerate(lst):
        p = f"displays[{i}]"
        d = v.mapping(d, p, lst.line_of(i), DISPLAY_KEYS, renamed={**RENAMED_ID, **RENAMED_DISPLAY})
        if d is None:
            continue
        if "ssd1306" in d:
            continue                # refused above (RENAMED_DISPLAY), with the new form
        ident = v.ident(d, p, "display", len(dev.displays))
        typ = d.get("type", "text" if d.get("display") is not None else "segment14")
        if typ not in DISPLAY_TYPES:
            v.err(f"{p}.type", d.line_of("type"), f"unknown display type '{typ}' (use {', '.join(DISPLAY_TYPES)})")
            continue
        t = DISPLAY_TYPES[typ]
        # display 0 shows the device's messages, as before shows: (the others: only a host's text)
        shows = d.get("shows", "last_message" if not dev.displays else "none")
        if shows not in DISPLAY_SHOWS:
            v.err(f"{p}.shows", d.line_of("shows"), f"unknown value '{shows}' (use {', '.join(DISPLAY_SHOWS)})")
            shows = "none"
        if shows in ("preset_number", "preset_name") and not dev.presets_given:
            v.warn(f"{p}.shows", d.line_of("shows"), f"{shows}, but the device has no presets: (it always "
                                                      "shows preset 1's)")
        wrong = sorted((SEGMENT_KEYS if t == DISPLAY_TEXT else TEXT_KEYS) & set(d))
        if wrong:
            v.err(f"{p}.{wrong[0]}", d.line_of(wrong[0]),
                  f"{', '.join(wrong)}: only for " + ("segment displays (a text display is placed with "
                                                      "display:, line:, x: and scale:)" if t == DISPLAY_TEXT
                                                      else "text displays (type: text)"))
            continue
        if t == DISPLAY_TEXT:
            x = validate_text_display(v, dev, d, p)
            if x is not None:
                x.update(id=ident or "", type=t, shows=DISPLAY_SHOWS[shows], path=p, line=d.line,
                         daw=daw_role(v, dev, d, p))
                dev.displays.append(Hub(**x))
            continue
        nseg, what = DISPLAY_SEGS[t]
        step = DISPLAY.get(v, d, "segment_step", p)
        order = segment_order(v, d, p, t)
        chars = v.seq(d.get("chars"), f"{p}.chars", d.line_of("chars"))
        if not chars:
            v.err(f"{p}.chars", d.line_of("chars") or d.line, "needs chars: each character's first pin "
                                                              "(segment a), left to right")
            continue
        if len(chars) > MAX_DISPLAY_CHARS:
            v.err(f"{p}.chars", d.line_of("chars"), f"{len(chars)} characters: at most {MAX_DISPLAY_CHARS}")
            continue
        pins, segs = [], []          # first pins; every segment's pin, character by character
        for j, raw in enumerate(chars):
            cp = f"{p}.chars[{j}]"
            line = chars.line_of(j)
            if isinstance(raw, list):
                got = display_char_map(v, raw, cp, line, nseg, what)
                if got is not None:
                    pins.append(got[0])
                    segs.append(got)
                continue
            if not isinstance(raw, dict):
                v.err(cp, line, f"must be a hub pin, e.g. {{ led_driver: leds_a, number: 0 }}: the {nseg} pins "
                                f"from it ({what}) must be in a row (or segment_step apart); or a list of "
                                "its segments' pins")
                continue
            pin = v.pin(raw, cp, line, "out")
            if pin is None:
                continue
            if pin.hub == 0:
                v.err(cp, line, f"must be a hub pin: the {nseg} pins from it ({what}) must be in a row")
                continue
            hub = dev.hubs[pin.hub - 1]
            # pin k (every segment_step) is segment order[k]; without segment_order, a..m then the
            # dot, the dot only where it fits (a TM1628's 7 grids: 7 segments)
            seg_at = order or list(range(nseg - 1 if step > 1 and pin.num + (nseg - 1) * step >= hub.n_pins
                                         else nseg))
            last = pin.num + (len(seg_at) - 1) * step
            if last >= hub.n_pins:
                every = f" every {step}" if step > 1 else ""
                what_ = "segment_order" if order else what.split(" and ")[0] if len(seg_at) < nseg else what
                v.err(cp, line, f"pins {pin.num}..{last}{every} ({what_}) don't fit on hub '{hub.id}' "
                                f"(0..{hub.n_pins - 1})")
                continue
            run = [NO_PIN] * nseg
            for k, sg in enumerate(seg_at):
                x = Pin(pin.hub, pin.flags, pin.num + k * step, f"{hub.id}.{pin.num + k * step}")
                run[sg] = x
                if k:                       # the first one was recorded by v.pin
                    v.use(("hub", hub.index, x.num), x.desc, Use(cp, line))
            pins.append(pin)
            segs.append(run)
        mapped = step > 1 or any(isinstance(x, list) for x in chars) or \
            (order is not None and order != list(range(nseg)))
        dev.displays.append(Hub(id=ident or "", type=t, chars=pins, segs=segs if mapped else None,
                                shows=DISPLAY_SHOWS[shows], path=p, line=d.line, daw=daw_role(v, dev, d, p),
                                n=len(pins), scale=0))
    if len(dev.displays) > MAX_DISPLAYS:
        v.err("displays", data.line_of("displays"), f"{len(dev.displays)} displays: at most {MAX_DISPLAYS}")


def validate_text_display(v, dev, d, p):
    """A text display (type: text): one row of 5x7 characters on a screen (display: <id>), from pixel x of
    text line `line` (8 pixels a line, from the top), each character 6 x scale pixels wide and `scale`
    lines tall. Returns the display's fields, or None after an error."""
    if d.get("display") is None:
        v.err(p, d.line, "a text display needs display: the id of the screen it is on (a display: item, "
                         "e.g. { id: oled, platform: ssd1306 })")
        return None
    sid = d["display"]
    scr = v.screen_by_id.get(sid) if isinstance(sid, str) else None
    if scr is None:
        known = [x.id for x in dev.screens]
        v.err(f"{p}.display", d.line_of("display"), f"no display with id '{sid}' (display: {', '.join(known) or 'none'})")
        return None
    scr.used = True
    lines = scr.height // TEXT_H
    scale = DISPLAY.get(v, d, "scale", p)
    line = v.integer(d, "line", p, 0, lines - 1, default=0)
    x = v.integer(d, "x", p, 0, scr.width - 1, default=0)
    if line + scale > lines:
        v.err(f"{p}.line", d.line_of("line") or d.line,
              f"line {line} at scale {scale} is {scale} line{'s' if scale > 1 else ''} tall: "
              f"'{scr.id}' has lines 0..{lines - 1} ({scr.height} pixels, 8 a line)")
        return None
    room = (scr.width - x) // (TEXT_W * scale)
    if room < 1:
        v.err(f"{p}.x", d.line_of("x"), f"no room for a character from x {x} at scale {scale} "
                                        f"({TEXT_W * scale} pixels; '{scr.id}' is {scr.width} wide)")
        return None
    n = DISPLAY.get(v, d, "length", p, default=min(room, MAX_TEXT_CHARS))
    if n > room:
        v.err(f"{p}.length", d.line_of("length"), f"{n} characters at scale {scale} from x {x} are "
                                                  f"{n * TEXT_W * scale} pixels: '{scr.id}' has room for {room}")
        return None
    rect = (x, line, x + n * TEXT_W * scale, line + scale)
    for o in dev.displays:
        if o.type == DISPLAY_TEXT and o.screen == scr.index and \
                rect[0] < o.rect[2] and o.rect[0] < rect[2] and rect[1] < o.rect[3] and o.rect[1] < rect[3]:
            v.err(p, d.line, f"overlaps display '{o.id}' on '{scr.id}' (lines {o.rect[1]}..{o.rect[3] - 1}, "
                             f"pixels {o.rect[0]}..{o.rect[2] - 1})")
            return None
    return dict(chars=None, segs=None, n=n, scale=scale, screen=scr.index, at=line * scr.width + x,
                at_desc=f"{scr.id} line {line} x {x}", rect=rect)


def segment_order(v, d, p, t):
    """segment_order: the segments' names in pin order (from each character's first pin): a list of
    segment indexes, or None (gm_display.h order). Segments left out aren't wired."""
    if d.get("segment_order") is None:
        return None
    names, sp, line = SEG_NAMES[t], f"{p}.segment_order", d.line_of("segment_order")
    lst = d["segment_order"]
    if not isinstance(lst, list) or not 1 <= len(lst) <= len(names):
        v.err(sp, line, f"must be a list of up to {len(names)} segment names, in pin order "
                        f"({', '.join(names)})")
        return None
    typ = next(n for n, c in DISPLAY_TYPES.items() if c == t)
    out = []
    for x in lst:
        k = str(x).lower()
        if k not in names:
            k = {"dot": "dp", "dp": "dot"}.get(k, k)     # either name for the dot
        if k not in names:
            v.err(sp, line, f"'{x}' is not a segment of a {typ} display ({', '.join(names)})")
            return None
        if names.index(k) in out:
            v.err(sp, line, f"'{x}' is named twice")
            return None
        out.append(names.index(k))
    return out


def display_char_map(v, raw, cp, line, nseg, what):
    """A character given as its segments' pins (hub output pins, in segment order; the dot may be left
    out): the list of Pins, NO_PIN for a dot not wired; None after an error."""
    if len(raw) not in (nseg, nseg - 1):
        v.err(cp, line, f"{len(raw)} pins: a character is {nseg} ({what}), or {nseg - 1} without the dot")
        return None
    out = []
    for k, x in enumerate(raw):
        sp = f"{cp}[{k}]"
        pin = v.pin(x, sp, raw.line_of(k), "out")
        if pin is None:
            return None
        if pin.hub == 0:
            v.err(sp, raw.line_of(k), "must be a hub pin (segments are written from the main loop, "
                                      "through the hub)")
            return None
        out.append(pin)
    return out + [NO_PIN] * (nseg - len(out))


def display_defines(dev):
    """gm_device_config.h: the display count, the widest display and the shows: values used."""
    h = ["", f"#define GM_N_DISPLAYS {len(dev.displays)} /* displays: (core/README.md \"Displays\") */"]
    if dev.displays:
        shows = 0
        for x in dev.displays:
            shows |= 1 << x.shows
        types = 0
        for x in dev.displays:
            types |= 1 << x.type
        h += [f"#define GM_DISPLAY_CHARS {max(x.n for x in dev.displays)} /* the widest display */",
              f"#define GM_DISPLAY_SHOWS 0x{shows:02X}u /* shows: values used, bit = GM_SHOWS_* */",
              f"#define GM_DISPLAY_TYPES 0x{types:02X}u /* display types used, bit = GM_DISPLAY_* */",
              f"#define GM_DISPLAY_MAP {1 if any(x.segs is not None for x in dev.displays) else 0} "
              "/* a display with a segment map */"]
    return h


def screen_defines(dev):
    """gm_device_config.h: the screens (display:) and their framebuffers' bytes (components/display)."""
    if not dev.screens:
        return []
    ram = sum(x.width * x.height // 8 for x in dev.screens)
    return [f"#define GM_N_SCREENS {len(dev.screens)} /* display: (components/display) */",
            f"#define GM_SCREEN_RAM {ram} /* their framebuffers' bytes (screen.c, twice: drawn and sent) */"]


def screen_tables(dev):
    """gm_screens[] (components/display/gm_screen.h gm_screen_t); nothing without screens."""
    if not dev.screens:
        return []
    c = ['#include "gm_screen.h"', "const gm_screen_t gm_screens[GM_N_SCREENS] = {"]
    for x in dev.screens:
        c.append(f"    {{ /* {c_comment(x.id)} */")
        c.append(f"        .type = {x.kind.enum}, .width = {x.width}, .height = {x.height},")
        c += x.kind.c_fields(x)
        c.append("    },")
    c += ["};", ""]
    return c


def display_tables(dev):
    """gm_displays[] (gm_display.h gm_display_t); nothing without displays."""
    if not dev.displays:
        return []
    c = ['#include "gm_display.h"']
    for i, x in enumerate(dev.displays):
        if x.segs is not None:
            nseg = DISPLAY_SEGS[x.type][0]
            c.append(f"static const gm_pin_t display_{i}_map[{len(x.segs) * nseg}] = {{")
            for k, run in enumerate(x.segs):
                c.append(f"    /* character {k} */")
                c += [f"    {pn.c()}, /* {c_comment(pn.desc)} */" for pn in run]
            c.append("};")
        elif x.chars is not None:
            c.append(f"static const gm_pin_t display_{i}_chars[{len(x.chars)}] = {{")
            c += [f"    {pn.c()}, /* {c_comment(pn.desc)} */" for pn in x.chars]
            c.append("};")
    c.append("const gm_display_t gm_displays[GM_N_DISPLAYS] = {")
    for i, x in enumerate(dev.displays):
        if x.type == DISPLAY_TEXT:
            c.append(f"    {{ {DISPLAY_C[x.type]}, {x.n}, 0, 0, {DISPLAY_SHOWS_C[x.shows]}, {x.scale}, {x.screen}, "
                     f"{x.at} }}, /* {c_comment(x.id)}: {c_comment(x.at_desc)} */")
            continue
        where = f"0, display_{i}_map" if x.segs is not None else f"display_{i}_chars, 0"
        c.append(f"    {{ {DISPLAY_C[x.type]}, {x.n}, {where}, {DISPLAY_SHOWS_C[x.shows]}, {x.scale}, 0, 0 }}, "
                 f"/* {c_comment(x.id)} */")
    c += ["};", ""]
    return c

# ---------------------------------------------------------------------------------------------
# layout: where the controls are, for drawing the device (sysex.md 4.7, core/README.md "Layout")

LAYOUT_VERSION = 1
LAYOUT_MAX = 255                     # units: the blob's u8 fields
LAYOUT_SHAPES = {"button": 0, "round_button": 1, "knob": 2, "encoder": 3, "fader": 4, "wheel": 5, "pad": 6,
                 "display": 7, "keys": 8, "xy": 9}


def _units(n, lo=0):
    """[x, y] / [x, y, w, h] / [w, h]: n whole units."""
    return {"type": "array", "items": {"type": "integer", "minimum": lo, "maximum": LAYOUT_MAX}, "minItems": n,
            "maxItems": n}


def _place(shapes, shape_doc):
    """A place as a mapping (layout_rect()), x and y required: its Keys, and its type (the lists, or it)."""
    keys = _Keys(
        _Key("x", _Int(0, LAYOUT_MAX), required=True, doc="Its left edge, in units from the left."),
        _Key("y", _Int(0, LAYOUT_MAX), required=True, doc="Its top edge, in units from the top."),
        _Key("w", _Int(1, LAYOUT_MAX), shown_default="its shape's", doc="Its width, in units."),
        _Key("h", _Int(1, LAYOUT_MAX), shown_default="its shape's", doc="Its height, in units."),
        _Key("shape", _Enum(shapes), doc=shape_doc),
    )
    as_map = _Raw({**keys.schema(), "required": ["x", "y"]}, "mapping", keys=keys)
    return keys, _OneOf(_Raw(_units(2), "`[x, y]`"), _Raw(_units(4), "`[x, y, w, h]`"), as_map)


# layout:, checked by validate_layout(): a control's place is checked by layout_rect(), its shape (the kind's)
# and the size: (what is placed) as code.
LAYOUT_PLACE, _PLACE = _place([x for x in LAYOUT_SHAPES if x != "keys"],
                              "How it is drawn (the shapes its kind can be).")
_KEYS_PLACE = _place(["keys"], "Always `keys`.")[1]
LAYOUT = _Keys(
    _Key("size", _Raw(_units(2, 1), "`[w, h]`"), shown_default="just big enough",
         doc=f"[w, h]: the drawing's size in units, 1..{LAYOUT_MAX}, at least what is placed."),
    _Key("keys", _KEYS_PLACE, shown_default="under the controls",
         doc="Where the key matrix's keys go: [x, y], [x, y, w, h] or { x, y, w, h }."),
    _Key("controls", _ById("control", _PLACE, names=ID_RE.pattern),
         doc="Each control's place, by id: [x, y] (with its shape's size), [x, y, w, h] or { x, y, w, h, shape }."),
)
LAYOUT_KEYS = LAYOUT.names
LAYOUT_ITEM_KEYS = LAYOUT_PLACE.names
# The Core area (core/yaml/layout.md)
LAYOUT_AREA = _Keys(_Key("layout", _Map(LAYOUT), see='core/README.md "Layout"',
                         doc="Where the controls are, in units of about 5 mm from the top left, for drawing the "
                             "device."))
LAYOUT_SIZES = {"button": (3, 2), "round_button": (2, 2), "knob": (3, 3), "encoder": (3, 3), "fader": (2, 8),
                "wheel": (2, 7), "pad": (4, 4), "display": (10, 3), "xy": (8, 8)}
LAYOUT_KINDS = {"button": 1, "keys": 2, "analog input": 3, "encoder": 4, "display": 6, "pad": 7}   # GET_COMPONENT
LAYOUT_KIND_SHAPES = {          # kind -> (default shape, allowed shapes)
    "button": ("button", ["button", "round_button", "pad"]),
    "analog input": ("knob", ["knob", "fader", "wheel", "xy"]),
    "encoder": ("encoder", ["encoder", "knob"]),
    "pad": ("pad", ["pad", "button", "round_button"]),
    "display": ("display", ["display"]),
}
WHITE_KEY_UNITS = 4                  # the generated keyboard: a white key is 4 units wide
KEYS_HEIGHT = 14


def white_keys(first_note, n):
    return sum(1 for k in range(n) if (first_note + k) % 12 in WHITE)


def layout_rect(v, x, p, line, default_wh):
    """A position: { x, y, w?, h?, shape? } or [x, y] / [x, y, w, h]. -> (x, y, w, h, shape or None) or None."""
    shape = None
    if isinstance(x, list):
        if len(x) not in (2, 4) or any(isinstance(n, bool) or not isinstance(n, int) for n in x):
            v.err(p, line, "must be [x, y] or [x, y, w, h] in whole units, or { x, y, w, h, shape }")
            return None
        vals = list(x) + (list(default_wh) if len(x) == 2 else [])
    else:
        d = v.mapping(x, p, line, LAYOUT_ITEM_KEYS)
        if d is None:
            return None
        got = [LAYOUT_PLACE.get(v, d, k, p) for k in "xy"]
        got += [LAYOUT_PLACE.get(v, d, k, p, default=dflt) for k, dflt in zip("wh", default_wh)]
        if None in got:
            return None
        vals = got
        if "shape" in d:
            shape = d["shape"]
            if shape not in LAYOUT_SHAPES:
                close = difflib.get_close_matches(str(shape), list(LAYOUT_SHAPES), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(f"{p}.shape", d.line_of("shape"), f"unknown shape '{shape}' "
                                                        f"({', '.join(LAYOUT_SHAPES)}{hint})")
                return None
    x0, y0, w, h = vals
    if not (0 <= x0 <= LAYOUT_MAX and 0 <= y0 <= LAYOUT_MAX and w >= 1 and h >= 1):
        v.err(p, line, f"out of range: x and y 0..{LAYOUT_MAX}, w and h at least 1")
        return None
    if x0 + w > LAYOUT_MAX or y0 + h > LAYOUT_MAX:
        v.err(p, line, f"reaches past {LAYOUT_MAX} units ({x0}+{w}, {y0}+{h}): the drawing is at most "
                       f"{LAYOUT_MAX} x {LAYOUT_MAX}")
        return None
    return x0, y0, w, h, shape


def layout_control(v, dev, ctl, cid, seen):
    """One layout.controls entry -> a layout item (Hub), or None after reporting why not."""
    p, line = f"layout.controls.{cid}", ctl.line_of(cid)
    target = v.ids.get(cid) if isinstance(cid, str) else None
    if target is not None and target[0] == "pad twin":
        v.err(p, line, f"'{cid}' is a layer twin: it is drawn where its base control is")
        return None
    if target is None or target[0] not in LAYOUT_KIND_SHAPES:
        known = [i for i, t in v.ids.items() if t[0] in LAYOUT_KIND_SHAPES]
        close = difflib.get_close_matches(str(cid), known, 1)
        hint = f"; did you mean '{close[0]}'?" if close else ""
        if target is None:
            # A warning: a device built on a package may !remove a control the package's layout places.
            v.warn(p, line, f"no control with id '{cid}': not drawn{hint}")
        else:
            v.err(p, line, f"'{cid}' is {with_article(target[0])}: layout: places buttons, analog inputs, encoders, "
                           f"pads and displays{hint}")
        return None
    kind, index = target[0], target[3]
    comp = {"button": dev.buttons, "analog input": dev.analogs, "encoder": dev.encoders,
            "pad": dev.pads, "display": dev.displays}[kind][index]
    if getattr(comp, "layer", 0):
        v.err(p, line, f"'{cid}' is a layer twin: it is drawn where its base control is")
        return None
    dshape, allowed = LAYOUT_KIND_SHAPES[kind]
    raw = ctl[cid]
    want = raw.get("shape", dshape) if isinstance(raw, dict) else dshape
    r = layout_rect(v, raw, p, line, LAYOUT_SIZES.get(want, LAYOUT_SIZES[dshape]))
    if r is None:
        return None
    x0, y0, w, h, given = r
    shape = given or dshape
    if shape not in allowed:
        v.err(f"{p}.shape", line, f"{with_article(kind)} can't be drawn as '{shape}' (use {' | '.join(allowed)})")
        return None
    extra = []
    if shape == "xy":
        pin = comp.pin
        hub = dev.hubs[pin.hub - 1] if 0 < pin.hub <= len(dev.hubs) else None
        if hub is None or hub.type not in XY_HUBS:
            v.err(f"{p}.shape", line, f"'{cid}' isn't on an xy_pad hub: shape xy draws the two axes of one")
            return None
        other = next((j for j, a in enumerate(dev.analogs) if a.pin.hub == pin.hub and a.pin.num != pin.num), None)
        if other is None:
            v.err(f"{p}.shape", line, f"the other axis of xy_pad '{hub.id}' isn't an analog input")
            return None
        xi, yi = (index, other) if pin.num == 0 else (other, index)
        for k in (xi, yi):
            if ("analog input", k) in seen:
                v.err(p, line, f"xy_pad '{hub.id}' is already placed (line {seen[('analog input', k)]}): one entry "
                               "with shape: xy draws both axes")
                return None
        seen[("analog input", xi)] = seen[("analog input", yi)] = line
        index, extra = xi, [yi]
    else:
        if (kind, index) in seen:
            v.err(p, line, f"'{cid}' is already placed (line {seen[(kind, index)]})")
            return None
        seen[(kind, index)] = line
    return Hub(kind=LAYOUT_KINDS[kind], index=index, shape=LAYOUT_SHAPES[shape], x=x0, y=y0, w=w, h=h,
               extra=extra, id=cid)


def layout_keys(v, dev, lay, items):
    """The key matrix's item: layout.keys, or under the controls, a white key WHITE_KEY_UNITS wide."""
    m = dev.matrix
    if m is None:
        if "keys" in lay:
            v.err("layout.keys", lay.line_of("keys"), "the device has no key_matrix")
        return None
    whites = white_keys(m.first_note, m.keys)
    default = (max(1, whites * WHITE_KEY_UNITS), KEYS_HEIGHT)
    if lay.get("keys") is not None:
        r = layout_rect(v, lay["keys"], "layout.keys", lay.line_of("keys"), default)
        if r is not None and r[4] not in (None, "keys"):
            v.err("layout.keys.shape", lay.line_of("keys"), "the keys are always drawn as keys")
            return None
    else:
        below = max([it.y + it.h for it in items], default=-1) + 1
        r = (0, below, *default, None)
        if r[2] > LAYOUT_MAX or r[1] + r[3] > LAYOUT_MAX:
            v.err("layout", lay.line, f"the keys don't fit under the controls ({r[2]} x {r[3]} units from y "
                                      f"{r[1]}): place them with keys: {{ x, y, w, h }}")
            return None
    if r is None:
        return None
    return Hub(kind=LAYOUT_KINDS["keys"], index=0, shape=LAYOUT_SHAPES["keys"], x=r[0], y=r[1], w=r[2], h=r[3],
               extra=[m.keys, m.first_note], id="keys")


def validate_layout(v, dev, data):
    """layout: each control's place, in units (about 5 mm). The key matrix goes under the controls unless
    keys: places it. Sets dev.layout = (width, height, [items]); None without layout:."""
    if data.get("layout") is None:
        return
    lay = v.mapping(data["layout"], "layout", data.line_of("layout"), LAYOUT_KEYS)
    if lay is None:
        return
    items, seen = [], {}
    ctl = lay.get("controls")
    if ctl is not None and not isinstance(ctl, dict):
        v.err("layout.controls", lay.line_of("controls"), "must be a mapping: control id -> { x, y, w, h, shape }")
        ctl = None
    for cid in ctl or []:
        it = layout_control(v, dev, ctl, cid, seen)
        if it is not None:
            items.append(it)
    keys = layout_keys(v, dev, lay, items)
    if keys is not None:
        items.append(keys)
    if not items:
        v.warn("layout", lay.line, "places nothing: no layout is built in")
        return
    width = max(it.x + it.w for it in items)
    height = max(it.y + it.h for it in items)
    if lay.get("size") is not None:
        sz, line = lay["size"], lay.line_of("size")
        if (not isinstance(sz, list) or len(sz) != 2
                or any(isinstance(n, bool) or not isinstance(n, int) or not 1 <= n <= LAYOUT_MAX for n in sz)):
            v.err("layout.size", line, f"must be [w, h] in whole units, 1..{LAYOUT_MAX}")
        elif sz[0] < width or sz[1] < height:
            v.err("layout.size", line, f"{sz[0]} x {sz[1]} is smaller than what is placed ({width} x {height})")
        else:
            width, height = sz
    dev.layout = (width, height, items)


def layout_item_bytes(it):
    body = [it.kind, it.index, it.shape, it.x, it.y, it.w, it.h, *it.extra]
    return [len(body), *body]


def layout_blob(dev):
    """The GET_LAYOUT blob (sysex.md 4.7 "Layout format (v1)")."""
    width, height, items = dev.layout
    out = bytearray([LAYOUT_VERSION, width, height, len(items) & 0xFF, len(items) >> 8])
    for it in items:
        out += bytes(layout_item_bytes(it))
    return bytes(out)


def layout_defines(dev):
    if not dev.layout:
        return []
    return [f"#define GM_LAYOUT_BYTES {len(layout_blob(dev))} /* layout: (GET_LAYOUT, sysex.md 4.7) */"]


def layout_tables(dev):
    """gm_layout[]: the blob, one item per line."""
    if not dev.layout:
        return []
    width, height, items = dev.layout
    names = {n: k for k, n in LAYOUT_SHAPES.items()}
    c = [f"/* layout: {width} x {height} units, {len(items)} items (sysex.md 4.7); each item: len, kind, index, "
         "shape, x, y, w, h, extras */",
         "const uint8_t gm_layout[GM_LAYOUT_BYTES] = {",
         f"    {LAYOUT_VERSION}, {width}, {height}, {len(items) & 0xFF}, {len(items) >> 8},"]
    c += [f"    {', '.join(map(str, layout_item_bytes(it)))}, /* {c_comment(it.id)}: {names[it.shape]} */"
          for it in items]
    return c + ["};", ""]


# ---------------------------------------------------------------------------------------------
# presets (research/core/presets-design.md §2, §10.2)

def preset_section(v, data):
    """presets: as a mapping, or None (not given, or not a mapping: reported once, here)."""
    if data.get("presets") is None:
        return None
    return v.mapping(data["presets"], "presets", data.line_of("presets"), PRESETS_KEYS)


def preset_slots(v, dev, data):
    """Before the components (preset_select checks its slot against it): dev.n_presets and the
    revision, from presets.slots (default: the number of listed presets). One slot is refused: without
    presets: the device has one, and presets are off."""
    ps = v.presets = preset_section(v, data)
    if ps is None:
        return
    dev.presets_given = True
    lst = ps.get("list")
    n_list = len(lst) if isinstance(lst, list) else 0
    slots = v.integer(ps, "slots", "presets", 1, MAX_PRESETS)
    if slots is not None and slots < n_list:
        v.err("presets.slots", ps.line_of("slots"), f"{slots} slot{'s' if slots != 1 else ''} can't hold the "
                                                     f"{n_list} presets in presets.list (set slots: {n_list} or more)")
        slots = None
    if n_list > MAX_PRESETS:
        v.err("presets.list", ps.line_of("list"), f"{n_list} presets: at most {MAX_PRESETS}")
    is_slots_bad = ps.get("slots") is not None and slots is None
    is_one_slot = not is_slots_bad and (slots or n_list) <= 1
    if is_one_slot:
        path = "presets.slots" if slots is not None else "presets"
        v.err(path, ps.line_of("slots") if slots is not None else data.line_of("presets"),
              "one preset slot is presets off, the same as leaving presets: out: give 2 or more slots "
              "(presets.slots, or 2 or more presets in presets.list), or leave presets: out")
    # An invalid slot count (reported above) allows every slot, so a preset: on a button or a mode
    # isn't reported as well; the build stops on the error anyway.
    dev.slots_invalid = is_slots_bad or is_one_slot
    dev.n_presets = MAX_PRESETS if dev.slots_invalid else min(slots or max(n_list, 1), MAX_PRESETS)
    dev.presets_revision = PRESETS_SPEC.get(v, ps, "revision", "presets")


def validate_presets(v, dev, data):
    """presets.list -> dev.presets (one Hub per slot: name, program, vals = sorted (id, index, value,
    comment) overrides that differ from slot 1)."""
    ps = v.presets
    lst = LList()
    if ps is not None and "list" in ps:
        lst = v.seq(ps["list"], "presets.list", ps.line_of("list"))
    names = {}
    dev.n_yaml_presets = max(1, min(len(lst), dev.n_presets))
    dev.presets = []
    for i in range(dev.n_presets):
        slot = i + 1
        p = f"presets.list[{i}]"
        if i >= len(lst):
            name = "Default" if slot == 1 else f"Preset {slot}"
            dev.presets.append(Hub(name=name, program=-1, vals=[], listed=False))
            if name.lower() in names:
                pth, ln = names[name.lower()]
                v.err(pth, ln, f"'{name}' is the name of empty slot {slot}: preset names are unique "
                               "(set a different name:)")
            continue
        d = v.mapping(lst[i], p, lst.line_of(i), PRESET_KEYS)
        if d is None:
            dev.presets.append(Hub(name=f"Preset {slot}", program=-1, vals=[], listed=True))
            continue
        name = preset_name(v, d, p, slot)
        if name.lower() in names:
            pth, ln = names[name.lower()]
            v.err(f"{p}.name" if "name" in d else p, d.line_of("name"),
                  f"'{name}' is already the name of {pth} (line {ln}): preset names are unique")
        names.setdefault(name.lower(), (p, d.line_of("name")))
        program = PRESET_ITEM.get(v, d, "program", p, default=-1)
        vals = preset_bank(v, dev, d, p, program)
        if slot == 1:
            for k in d:
                if k in PRESET_GLOBALS or k in PRESET_COMPONENTS:
                    v.err(f"{p}.{k}", d.line_of(k), "preset 1 is the component defaults written in the rest "
                                                    "of the file, so it takes only name: and program: (change "
                                                    "the component itself, or put this in another preset)")
        else:
            vals += preset_values(v, dev, d, p)    # none: a named copy of preset 1 (fine)
        dev.presets.append(Hub(name=name, program=program, vals=sorted(vals), listed=True, line=d.line))


def preset_name(v, d, p, slot):
    if d.get("name") is None:
        return "Default" if slot == 1 else f"Preset {slot}"
    name, line = d["name"], d.line_of("name")
    if not isinstance(name, str):
        name = d.raw_of("name")     # `name: 808` is the text 808
    bad = [c for c in name if not 32 <= ord(c) <= 126]
    if not name.strip():
        v.err(f"{p}.name", line, "must be a non-empty string")
    elif bad:
        v.err(f"{p}.name", line, f"'{name}' has {bad[0]!r}: a preset name is printable ASCII (it is stored "
                                 "and sent over SysEx as 7-bit text)")
    elif len(name) > PRESET_NAME_LEN:
        v.err(f"{p}.name", line, f"'{name}' is {len(name)} characters: at most {PRESET_NAME_LEN} "
                                 f"(e.g. '{name[:PRESET_NAME_LEN].rstrip()}')")
    else:
        return name
    return f"Preset {slot}"


def preset_values(v, dev, d, p):
    """A preset's overrides -> [(id, index, value, comment)], only those that differ from slot 1."""
    out = []

    def put(param, index, value, base, path, line, label, shown=None):
        """One override; label names it (`button.action[sustain]`), shown is the value as written."""
        if value is None:
            return
        text = f"{label} = {value if shown is None else shown}"
        if value == base:
            v.warn(path, line, f"the same as in preset 1 ({text}): ignored")
            return
        out.append((PARAM_ID[param], index, value, text))

    for sec, keys in PRESET_GLOBALS.items():
        if sec not in d:
            continue
        sp, sl = f"{p}.{sec}", d.line_of(sec)
        g = v.mapping(d[sec], sp, sl, keys.names, renamed=PRESET_NOT_HERE.get(sec))
        if g is None:
            continue
        for key in keys:
            k, param, typ = key.name, key.param, key.type
            if k not in g:
                continue
            kp, kl = f"{sp}.{k}", g.line_of(k)
            if not any(n == param and c(dev) for n, _i, _s, c in PARAMS):
                why = ("it needs a velocity key matrix (key_matrix: { type: velocity })"
                       if param == "velocity.curve" else "the device has no pads"
                       if param.startswith("pad.") else "the device has no buttons")
                v.err(kp, kl, f"{param} doesn't exist on this device: {why}")
                continue
            if isinstance(typ, _Bool):         # (Bool's check reads a wrong value as false)
                val = v.boolean(g, k, sp, default=None)
                val, shown = (None, None) if val is None else (int(val), str(val).lower())
            else:
                val = keys.get(v, g, k, sp, default=None)
                shown = (None if val is None or isinstance(typ, _Int) else f"{val} ms" if isinstance(typ, _Period)
                         else typ.values[val])
            put(param, 0, val, preset_base(dev, key), kp, kl, param, shown)

    for sec, (kind, plural, allowed, hw) in PRESET_COMPONENTS.items():
        if sec not in d:
            continue
        sp, sl = f"{p}.{sec}", d.line_of(sec)
        comps = d[sec]
        if not isinstance(comps, dict):
            v.err(sp, sl, f"must be a mapping of {kind} ids to what changes, e.g. "
                          f"`{sec}: {{ {example_id(kind)}: {{ cc: 11 }} }}` (got {type_name(comps)})")
            continue
        for cid in comps:
            cp, cl = f"{sp}.{cid}", comps.line_of(cid)
            idx = preset_ref(v, cid, cp, cl, kind, plural)
            if idx is None:
                continue
            o = v.mapping(comps[cid], cp, cl, allowed, renamed={k: f"{k}: is {_HW}" for k in hw})
            if o is None:
                continue
            if not o:
                v.warn(cp, cl, "changes nothing")
                continue
            line = lambda k: o.line_of(k) if k in o else o.line   # noqa: E731
            twin = sec in ("buttons", "encoders") and getattr((dev.buttons if sec == "buttons" else dev.encoders)[idx],
                                                              "layer", 0)
            if twin:
                v.err(cp, cl, f"'{cid}' is a layer twin: its settings are global (layer.{kind}.*), the same in "
                              "every preset; set them in the mode's layer:")
                continue
            if sec == "buttons" and idx in v.mode_toggles:
                v.err(cp, cl, f"button '{cid}' toggles mode '{v.mode_toggles[idx]}', so presets can't change it "
                              "(modes are the same in every preset, so a preset can't lock you out of them)")
                continue
            if sec == "buttons":
                b = dev.buttons[idx]
                act, num = button_action(v, dev, o, cp, base=(b.action_name, b.number))
                why = buttons.held_action_problem(dev, idx, act)
                if why:
                    v.err(f"{cp}.action", line("action"), why)
                ak = "actions" if "actions" in o else "action"
                if "action" in o or act != b.action_name:     # another list is only another number
                    put("button.action", idx, ACTION_IDS[act], b.action, f"{cp}.{ak}", line(ak),
                        f"button.action[{cid}]", act)
                nk = next((k for k in (*NUMBER_KEYS, "mcu", "actions") if k in o), None)
                if nk is not None or num != b.number:
                    put("button.number", idx, num, b.number, f"{cp}.{nk or 'action'}", line(nk or "action"),
                        f"button.number[{cid}]")
            elif sec == "analog":
                a = dev.analogs[idx]
                m = a.mode
                if "mode" in o:
                    m = control_mode(v, dev, o, cp, ANALOG_MODES)
                    put("analog.mode", idx, m, a.mode, f"{cp}.mode", line("mode"),
                        f"analog.mode[{cid}]", None if m is None else ANALOG_MODES[m])
                    m = a.mode if m is None else m
                nk = next((k for k in ANALOG_NUMBER_KEYS if k in o), None)
                if nk is not None:
                    put("analog.cc", idx, mode_number(v, dev, o, cp, ANALOG_MODES[m], False, ANALOG_NUMBER_KEYS),
                        a.cc, f"{cp}.{nk}", line(nk), f"analog.cc[{cid}]")
            elif sec == "pads":
                put("pad.note", idx, v.integer(o, "note", cp, 0, 127), dev.pads[idx].note, f"{cp}.note",
                    line("note"), f"pad.note[{cid}]")
            else:
                e = dev.encoders[idx]
                m = e.mode
                if "mode" in o:
                    m = control_mode(v, dev, o, cp, ENC_MODES)
                    put("encoder.mode", idx, m, e.mode, f"{cp}.mode", line("mode"), f"encoder.mode[{cid}]",
                        None if m is None else ENC_MODES[m])
                    m = e.mode if m is None else m
                nk = next((k for k in ENCODER_NUMBER_KEYS if k in o), None)
                if nk is not None:
                    put("encoder.cc", idx, mode_number(v, dev, o, cp, ENC_MODES[m], False, ENCODER_NUMBER_KEYS),
                        e.cc, f"{cp}.{nk}", line(nk), f"encoder.cc[{cid}]")
    return out


# ---------------------------------------------------------------------------------------------
# MIDI messages (core/README.md "MIDI messages")

_MSG_SEE = 'core/README.md "MIDI messages"'
SYSEX_KEYS = _Keys(
    _Key("id", _Id(), required=True, doc="What a control's `sysex:` calls it."),
    _Key("data", _Str(), required=True, see=_MSG_SEE,
         doc="Hex bytes F0 .. F7, with v, msb, lsb, ch and [ ... ] cs placeholders."),
)
# enable: and sysex: are checked by validate_midi_messages (aliases, names, the strings)
MIDI_MESSAGES_KEYS = _Keys(
    _Key("enable", _List(_Enum([*MSG_FEATURES, *MSG_ALIASES])), see=_MSG_SEE,
         doc="Kinds to compile in even if no control uses them yet, for the web page (each kind is in "
             "anyway when one does). `rpn` is `nrpn`'s, `bank_select` `program`'s."),
    _Key("null_rpn", _Bool(), default=False, see=_MSG_SEE,
         doc="After each NRPN or RPN message, the null RPN (101 / 100 = 127)."),
    _Key("mmc_device_id", _Hex(0, 0x7F), default=0x7F, shown_default="`0x7F` (all devices)",
         doc="The device id MMC commands are sent to."),
    _Key("sysex", _List(_Map(SYSEX_KEYS)), see=_MSG_SEE,
         doc="SysEx strings of your own, fixed at build time (at most 127), for `action: sysex` buttons and "
             "`mode: sysex` analog inputs."),
)
# The "midi" area: MIDI and USB (core/yaml/midi.md)
MIDI_AREA = _Keys(
    _Key("usb", _Map(USB_KEYS), shown_default="the pid.codes test ids 1209:0002, for development only",
         doc="The USB ids, on platforms with native USB (ignored with another `midi.transport`)."),
    _Key("midi", _Map(MIDI_KEYS), doc="How the device talks MIDI."),
    _Key("din_midi", _Map(DIN_KEYS), see='core/README.md "DIN MIDI"',
         doc="5-pin DIN MIDI In and Out on a UART, at 31250 baud (stm32f1 for now)."),
    _Key("midi_messages", _Map(MIDI_MESSAGES_KEYS), see=_MSG_SEE,
         doc="MIDI messages: 14-bit CC, NRPN/RPN, bank select, panic, MMC, SysEx."),
)
BANK_KEYS = ["msb", "lsb"]


def validate_midi_messages(v, dev, data):
    """midi_messages: { enable, null_rpn, mmc_device_id, sysex } (before the components: they refer to the
    SysEx strings by id)."""
    if data.get("midi_messages") is None:
        return
    mm = v.mapping(data["midi_messages"], "midi_messages", data.line_of("midi_messages"), MIDI_MESSAGES_KEYS.names)
    if mm is None:
        return
    if mm.get("enable") is not None:
        lst = v.seq(mm["enable"], "midi_messages.enable", mm.line_of("enable"))
        names = [*MSG_FEATURES, *MSG_ALIASES]
        for i, x in enumerate(lst):
            p, ln = f"midi_messages.enable[{i}]", lst.line_of(i)
            f = MSG_ALIASES.get(x, x) if isinstance(x, str) else None
            if f not in MSG_FEATURES:
                close = difflib.get_close_matches(str(x), names, 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(p, ln, f"unknown message kind '{lst.raw_of(i)}' (valid: {', '.join(names)}{hint}; SysEx is "
                             "on when midi_messages.sysex has strings)")
                continue
            if f in dev.msg_enable:
                v.warn(p, ln, f"'{x}' is listed twice")
            dev.msg_enable.add(f)
    if "null_rpn" in mm:
        dev.null_rpn = MIDI_MESSAGES_KEYS.get(v, mm, "null_rpn", "midi_messages")
        dev.null_rpn_line = mm.line_of("null_rpn")
    if "mmc_device_id" in mm:
        dev.mmc_id = MIDI_MESSAGES_KEYS.get(v, mm, "mmc_device_id", "midi_messages")
        dev.mmc_line = mm.line_of("mmc_device_id")
    if mm.get("sysex") is not None:
        lst = v.seq(mm["sysex"], "midi_messages.sysex", mm.line_of("sysex"))
        for i, d in enumerate(lst):
            p = f"midi_messages.sysex[{i}]"
            d = v.mapping(d, p, lst.line_of(i), SYSEX_KEYS.names)
            if d is None:
                continue
            ident = v.ident(d, p, "SysEx string", len(dev.sysex), allow_name=False)
            if "data" not in d or d["data"] is None:
                v.err(p, d.line, "needs data: the message in hex, e.g. "
                                 "`data: \"F0 41 10 42 12 [40 00 7F v] cs F7\"`")
                continue
            parsed = parse_sysex(v, d["data"], f"{p}.data", d.line_of("data"))
            if parsed is None or ident is None:
                continue
            body, flags = parsed
            dev.sysex_ids[ident] = len(dev.sysex)
            dev.sysex.append(Hub(id=ident, data=body, flags=flags, text=" ".join(str(d["data"]).split())))
        if len(dev.sysex) > 127:
            v.err("midi_messages.sysex", mm.line_of("sysex"), f"{len(dev.sysex)} strings: at most 127")


def parse_sysex(v, text, path, line):
    """A SysEx string: hex bytes from F0 to F7; v (the value, 7-bit), msb / lsb (a 14-bit value's top and
    bottom 7 bits), ch (the channel, 0..15); [ ... ] cs: the Roland checksum of the bytes in the brackets.
    -> (data with gm_device.h GM_SX_* codes, flags), or None after an error."""
    if not isinstance(text, str):
        v.err(path, line, "must be a string of hex bytes, e.g. \"F0 41 10 42 12 [40 00 7F v] cs F7\"")
        return None
    toks = text.replace("[", " [ ").replace("]", " ] ").split()
    out, sent, flags = [], 0, 0
    state = "none"          # none -> open -> closed -> done (the one checksum range)
    for k, t in enumerate(toks):
        low = t.lower()
        if low == "[":
            if state != "none":
                v.err(path, line, "only one [ ... ] cs checksum range")
                return None
            out.append(SX_SUM_START)
            state = "open"
        elif low == "]":
            if state != "open":
                v.err(path, line, "] without [")
                return None
            state = "closed"
        elif low == "cs":
            if state != "closed" or toks[k - 1] != "]":
                v.err(path, line, "cs (the checksum) goes right after the ] that ends its range: [ ... ] cs")
                return None
            out.append(SX_SUM)
            sent += 1
            state = "done"
        elif low in SYSEX_TOKENS:
            if state == "closed":
                v.err(path, line, "cs (the checksum) goes right after the ] that ends its range: [ ... ] cs")
                return None
            out.append(SYSEX_TOKENS[low])
            sent += 1
            flags |= 2 if low != "ch" else 0                            # GM_SX_HAS_VALUE
            flags |= 1 if low in ("msb", "lsb") else 0                  # GM_SX_WIDE
        elif re.fullmatch(r"(0x)?[0-9a-fA-F]{1,2}", t):
            b = int(t, 16)
            if state == "closed":
                v.err(path, line, "cs (the checksum) goes right after the ] that ends its range: [ ... ] cs")
                return None
            first, last = k == 0, k == len(toks) - 1
            if first and b != 0xF0:
                v.err(path, line, f"starts with {t}: a SysEx message starts with F0")
                return None
            if last and b != 0xF7:
                v.err(path, line, f"ends with {t}: a SysEx message ends with F7")
                return None
            if not first and not last and b > 0x7F:
                v.err(path, line, f"{t} is over 7F: the bytes between F0 and F7 are 00..7F")
                return None
            out.append(b)
            sent += 1
        else:
            v.err(path, line, f"'{t}' is not a hex byte or a placeholder (v, msb, lsb, ch, [ ] cs)")
            return None
    if not toks or toks[-1].lower() != "f7" or toks[0].lower() != "f0":
        v.err(path, line, "a SysEx message starts with F0 and ends with F7")
        return None
    if state in ("open", "closed"):
        v.err(path, line, "a [ ... ] range needs its cs (the checksum) right after the ]")
        return None
    names = {t.lower() for t in toks}
    if "v" in names and names & {"msb", "lsb"}:
        v.err(path, line, "use v (a 7-bit value) or msb / lsb (a 14-bit value), not both")
        return None
    if sent > SYSEX_MAX:
        v.err(path, line, f"{sent} bytes: at most {SYSEX_MAX}")
        return None
    return out, flags


def sysex_ref(v, dev, d, key, path):
    """sysex: <id> -> the string's index (0 after an error)."""
    x = d[key]
    if isinstance(x, str) and x in dev.sysex_ids:
        return dev.sysex_ids[x]
    known = list(dev.sysex_ids)
    close = difflib.get_close_matches(str(d.raw_of(key)), known, 1)
    hint = f"; did you mean '{close[0]}'?" if close else ""
    v.err(f"{path}.{key}", d.line_of(key), f"no SysEx string with id '{d.raw_of(key)}' (midi_messages.sysex: "
                                            f"{', '.join(known) or 'none'}{hint})")
    return 0


def control_mode(v, dev, d, p, modes):
    """An analog / encoder's mode: (or a preset's override of it), one of modes -> its index, or None (no
    mode:, or one that isn't in modes: reported). A mode that needs a message kind compiles it in."""
    if "mode" not in d:
        return None
    m = v.enum_value(d, "mode", f"{p}.mode", modes)
    if m is not None:
        mode_use(v, dev, modes[m], f"{p}.mode", d.line_of("mode"))
    return m


def mode_use(v, dev, mname, path, line):
    """An analog / encoder mode that needs a message kind: it is compiled in."""
    f = MODE_FEATURE.get(mname)
    if f is None:
        return
    if f == "sysex" and not dev.sysex:
        v.err(path, line, "mode sysex sends one of the midi_messages.sysex strings, and there are none")
    dev.msg_used.add(f)


def mode_number(v, dev, d, p, mname, required, keys):
    """analog.cc / encoder.cc from the key the mode takes: cc: (cc, cc14: 0..31; the default for other
    modes), nrpn: / rpn: (0..16383), sysex: (a string's id). Returns the number (0 when not given)."""
    want = MODE_NUMBER_KEY.get(mname, "cc")
    given = [k for k in keys if k in d]
    if len(given) > 1:
        v.err(p, d.line, f"give one of {', '.join(k + ':' for k in given)}, not {len(given)}")
    for k in given:
        if k != want:
            v.err(f"{p}.{k}", d.line_of(k), f"mode {mname} takes {want}:, not {k}:" if want != "cc" else
                  f"{k}: goes with mode: {' / '.join(m for m, n in MODE_NUMBER_KEY.items() if n == k)}")
            return 0
    if want == "cc":
        hi = 31 if mname == "cc14" else 127
        return v.integer(d, "cc", p, 0, hi, default=0, required=required)
    if want not in d:
        v.err(p, d.line, f"mode {mname} needs {want}: (" +
              ("the id of a midi_messages.sysex string" if want == "sysex" else "0..16383") + ")")
        return 0
    if want == "sysex":
        return sysex_ref(v, dev, d, "sysex", p)
    x = v.integer(d, want, p, 0, 16383, default=0)
    return 0 if x is None else x


def control_out(v, d, p, top):
    """channel:, min:, max: -> (min, max, channel) for gm_*_ctl[] (max CTL_TOP: the mode's top, channel 0:
    midi.channel), or None when none is given."""
    if not any(k in d for k in ("channel", "min", "max")):
        return None
    ch = v.integer(d, "channel", p, 1, 16, default=0) or 0
    lo = v.integer(d, "min", p, 0, top, default=0) or 0
    hi = v.integer(d, "max", p, 0, top, default=None)
    if lo == (top if hi is None else hi):
        v.err(p, d.line, f"min and max are both {lo}: the control would always send {lo}")
    return lo, CTL_TOP if hi is None else hi, ch


def bank_value(v, d, p):
    """bank: 0..16383, or { msb: 0..127, lsb: 0..127 } -> the 14-bit bank (CC 0 = bank >> 7, CC 32 = bank & 127)."""
    x = d["bank"]
    if isinstance(x, dict):
        b = v.mapping(x, f"{p}.bank", d.line_of("bank"), BANK_KEYS)
        if b is None:
            return None
        msb = v.integer(b, "msb", f"{p}.bank", 0, 127, default=0) or 0
        lsb = v.integer(b, "lsb", f"{p}.bank", 0, 127, default=0) or 0
        return msb << 7 | lsb
    return v.integer(d, "bank", p, 0, 16383)


def preset_bank(v, dev, d, p, program):
    """A preset's bank: (preset.bank: Bank Select before its Program Change) -> its value list."""
    if "bank" not in d:
        return []
    bank = bank_value(v, d, p)
    if bank is None:
        return []
    if program < 0:
        v.err(f"{p}.bank", d.line_of("bank"), "bank: is sent before the preset's Program Change: give program: too")
        return []
    dev.msg_used.add("program")
    return [(PARAM_ID["preset.bank"], 0, bank, f"preset.bank = {bank} (MSB {bank >> 7}, LSB {bank & 127})")]


def msg_features(v, dev, data):
    """What the firmware compiles in: the kinds the YAML uses and those midi_messages.enable lists."""
    dev.msg = set(dev.msg_enable) | dev.msg_used
    if dev.sysex:
        dev.msg.add("sysex")
    if dev.null_rpn and "nrpn" not in dev.msg:
        v.warn("midi_messages.null_rpn", dev.null_rpn_line, "nothing sends NRPN / RPN: add nrpn to "
                                                           "midi_messages.enable to pick it from the web page")
    if getattr(dev, "mmc_line", None) and "mmc" not in dev.msg:
        v.warn("midi_messages.mmc_device_id", dev.mmc_line, "no button has action mmc: add mmc to "
                                                            "midi_messages.enable to pick it from the web page")


def msg_defines(dev):
    m = dev.msg
    h = ["/* MIDI messages (core/README.md \"MIDI messages\"): kinds used or in midi_messages.enable */"]
    for f, macro in (("cc14", "GM_MSG_CC14"), ("nrpn", "GM_MSG_NRPN"), ("program", "GM_MSG_PROGRAM"),
                     ("panic", "GM_MSG_PANIC"), ("mmc", "GM_MSG_MMC")):
        h.append(f"#define {macro} {1 if f in m else 0}")
    if dev.null_rpn and "nrpn" in m:
        h.append("#define GM_NULL_RPN 1 /* midi_messages.null_rpn */")
    if "mmc" in m:
        h.append(f"#define GM_MMC_DEVICE 0x{dev.mmc_id:02X} /* midi_messages.mmc_device_id */")
    h.append(f"#define GM_N_SYSEX {len(dev.sysex)} /* gm_sysex[] (midi_messages.sysex) */")
    if dev.sysex:
        sent = max(len(x.data) - (1 if SX_SUM_START in x.data else 0) for x in dev.sysex)
        h.append(f"#define GM_SYSEX_MAX {sent} /* bytes of the longest, as sent */")
    flags = (("GM_BTN_CTL", any(getattr(b, "ctl", None) for b in dev.buttons), "gm_button_ctl[]"),
             ("GM_ANALOG_CTL", any(getattr(a, "ctl", None) for a in dev.analogs), "gm_analog_ctl[]"),
             ("GM_ENC_CTL", any(getattr(e, "ctl", None) for e in dev.encoders), "gm_encoder_ctl[]"),
             ("GM_BTN_BANK", any(getattr(b, "bank", None) is not None for b in dev.buttons), "gm_button_bank[]"),
             ("GM_ENC_MODES", any(getattr(e, "mode", 0) for e in dev.encoders), "gm_encoder_mode_defaults[]"))
    h += [f"#define {n} {1 if on else 0} /* 1: {t} */" for n, on, t in flags]
    return h


def msg_tables(dev):
    c = []

    def ctl_table(cname, count, lst):
        rows = []
        for x in lst:
            lo, hi, ch = getattr(x, "ctl", None) or (0, CTL_TOP, 0)
            rows.append(f"    {{ {lo}, {'GM_CTL_TOP' if hi == CTL_TOP else hi}, {ch} }}, /* {c_comment(x.id)} */")
        return [f"const gm_ctl_t {cname}[{count}] = {{", *rows, "};", ""]

    c.append("/* MIDI messages: per control { min, max, channel } (channel 0 = midi.channel) */")
    if any(getattr(b, "ctl", None) for b in dev.buttons):
        c += ctl_table("gm_button_ctl", "GM_N_BUTTONS", dev.buttons)
    if any(getattr(a, "ctl", None) for a in dev.analogs):
        c += ctl_table("gm_analog_ctl", "GM_N_ANALOGS", dev.analogs)
    if any(getattr(e, "ctl", None) for e in dev.encoders):
        c += ctl_table("gm_encoder_ctl", "GM_N_ENCODERS", dev.encoders)
    if any(getattr(b, "bank", None) is not None for b in dev.buttons):
        c.append("const uint16_t gm_button_bank[GM_N_BUTTONS] = { /* Bank Select before a program action */")
        c += [f"    {'GM_BANK_NONE' if getattr(b, 'bank', None) is None else b.bank}, /* {c_comment(b.id)} */"
              for b in dev.buttons]
        c += ["};", ""]
    if any(getattr(e, "mode", 0) for e in dev.encoders):
        c.append("const int32_t gm_encoder_mode_defaults[GM_N_ENCODERS] = { " +
                 ", ".join(str(getattr(e, "mode", 0)) for e in dev.encoders) + " }; /* " +
                 ", ".join(ENC_MODES[getattr(e, "mode", 0)] for e in dev.encoders) + " */")
        c.append("")
    if dev.sysex:
        for k, x in enumerate(dev.sysex):
            body = ", ".join(f"0x{b:02X}" for b in x.data)
            c.append(f"static const uint8_t sysex_{k}[{len(x.data)}] = {{ {body} }}; /* {c_comment(x.id)}: "
                     f"{c_comment(x.text)} */")
        c.append("const gm_sysex_t gm_sysex[GM_N_SYSEX] = {")
        c += [f"    {{ sysex_{k}, {len(x.data)}, {x.flags} }}," for k, x in enumerate(dev.sysex)]
        c += ["};", ""]
    return c


# ---------------------------------------------------------------------------------------------
# write_to_flash: what the device stores on its own (core/README.md "Writes the device makes on its own")

# item -> its default (all off). A new item goes here, in WTF_DOCS, and in write_to_flash_defines.
WTF_ITEMS = {"last_preset": False}
WTF_DOCS = {"last_preset": "Store the selected preset (`preset.active`) after a switch, so the device powers up on "
                           "the preset used last."}
WTF_DELAY = (10_000, 1_000, 3_600_000, "1s..1h")    # ms: default, min, max, range text
WTF_IDLE = (3_000, 100, 600_000, "100ms..10min")
_WTF_SEE = 'core/README.md "Writes the device makes on its own"'
# delay and idle: their ranges are checked by validate_write_to_flash, which words them (1h, 10min) its own way
WTF_KEYS = _Keys(
    *(_Key(k, _Bool(), default=d, see=_WTF_SEE, doc=WTF_DOCS[k]) for k, d in WTF_ITEMS.items()),
    _Key("delay", _Period(WTF_DELAY[1] * 1000, WTF_DELAY[2] * 1000, step_us=1000, example="10s"),
         default=f"{WTF_DELAY[0] // 1000}s",
         doc="A write waits this long after the last change."),
    _Key("idle", _Period(WTF_IDLE[1] * 1000, WTF_IDLE[2] * 1000, step_us=1000, example="3s"),
         default=f"{WTF_IDLE[0] // 1000}s",
         doc="... and until nothing has been played or touched for this long."),
)


def validate_write_to_flash(v, dev, data):
    """write_to_flash: true | false | { last_preset, delay, idle } -> dev.write_to_flash (item -> on,
    plus delay_ms, idle_ms). Only the writes the device makes on its own: host saves always work."""
    w = dict(WTF_ITEMS, delay_ms=WTF_DELAY[0], idle_ms=WTF_IDLE[0])
    dev.write_to_flash = w
    x = data.get("write_to_flash")
    line = data.line_of("write_to_flash")
    if x is None:
        return
    if isinstance(x, bool):
        w.update({k: x for k in WTF_ITEMS})
    else:
        if not isinstance(x, dict):
            got = type_name(x) + ("" if isinstance(x, list) else f" {data.raw_of('write_to_flash')!r}")
            v.err("write_to_flash", line, f"must be true, false or a mapping like `{{ last_preset: true }}` "
                                          f"(got {got})")
            return
        if v.mapping(x, "write_to_flash", line, WTF_KEYS.names) is None:
            return
        for k in WTF_ITEMS:
            w[k] = WTF_KEYS.get(v, x, k, "write_to_flash")
        for key, (dflt, lo, hi, rng) in (("delay", WTF_DELAY), ("idle", WTF_IDLE)):
            ms = v.period(x, key, "write_to_flash", 0, 0xFFFFFFFF * 1000, step_us=1000, example="10s")
            if ms is not None and not lo <= ms <= hi:
                v.err(f"write_to_flash.{key}", x.line_of(key), f"{x.raw_of(key)} is out of range ({rng})")
                ms = None
            w[f"{key}_ms"] = dflt if ms is None else ms
        if not any(w[k] for k in WTF_ITEMS):
            for key in ("delay", "idle"):
                if key in x:
                    v.warn(f"write_to_flash.{key}", x.line_of(key),
                           "only used when the device stores something on its own (e.g. last_preset: true)")
    if w["last_preset"] and not dev.has_presets:
        v.warn("write_to_flash.last_preset", line, f"nothing to remember (ignored): {PRESETS_OFF}")
        w["last_preset"] = False


def write_to_flash_defines(dev):
    """GM_WRITE_*: what the device stores on its own (0: nothing; the core leaves that code out)."""
    w = getattr(dev, "write_to_flash", None) or dict(WTF_ITEMS, delay_ms=WTF_DELAY[0], idle_ms=WTF_IDLE[0])
    return ["", "/* write_to_flash: writes the device makes on its own (host saves always work) */",
            f"#define GM_WRITE_LAST_PRESET {int(w['last_preset'])} /* write_to_flash.last_preset */",
            f"#define GM_WRITE_DELAY_MS {w['delay_ms']}u /* write_to_flash.delay: after the last change */",
            f"#define GM_WRITE_IDLE_MS {w['idle_ms']}u /* write_to_flash.idle: and nothing played for this long */"]


def preset_ref(v, cid, path, line, kind, plural):
    """A component id used as a key in a preset -> its index, or None after saying why."""
    target = v.ids.get(cid) if isinstance(cid, str) else None
    if target is not None and target[0] == kind:
        return target[3]
    if target is not None:
        v.err(path, line, f"'{cid}' is the id of {target[1]} ({with_article(target[0])}), not of {with_article(kind)}")
        return None
    known = [i for i, t in v.ids.items() if t[0] == kind]
    close = difflib.get_close_matches(str(cid), known, 1)
    hint = f"; did you mean '{close[0]}'?" if close else ""
    v.err(path, line, f"no {kind} with id '{cid}' ({plural}: {', '.join(known) or 'none'}{hint})")
    return None


# ---- safety (core/README.md "Factory-reset combo", "Physical unlock", "Status LED") ----
RESET_HOLD_DEFAULT_MS = 5000
UNLOCK_HOLD_DEFAULT_MS = 2000
COMBO_MAX = 8            # buttons in a combo (sysex.md 4.9 UNLOCK `n`)
# factory_reset_combo: / unlock: as a mapping (the button ids are checked by combo_buttons)
RESET_COMBO_KEYS = _Keys(
    _Key("combo", _List(_IdRef("button")), required=True, doc=f"The buttons, at most {COMBO_MAX}."),
    _Key("hold", _Period(1_000_000, 30_000_000, step_us=1000, example="5s"),
         default=f"{RESET_HOLD_DEFAULT_MS // 1000}s", doc="How long they're held before the settings are wiped."),
)
UNLOCK_KEYS = _Keys(
    _Key("combo", _List(_IdRef("button")), shown_default="`boot_combo`", doc=f"The buttons, at most {COMBO_MAX}."),
    _Key("hold", _Period(0, 10_000_000, step_us=1000, example="2s"), default=f"{UNLOCK_HOLD_DEFAULT_MS // 1000}s",
         doc="How long they're held to unlock; `0s`: a press."),
)


def combo_buttons(v, dev, seq, path, line):
    """A combo's button ids -> sorted button indices (the YAML's own buttons, not layer twins)."""
    out = []
    items = v.seq(seq, path, line)
    if not items:
        v.err(path, line, "needs at least one button id (e.g. `[pitch_down, pitch_up]`)")
    for i in range(len(items)):
        p, ln = f"{path}[{i}]", items.line_of(i)
        idx = v.ref(items, i, p, ln, "button", "buttons")
        if idx is None:
            continue
        if idx in out:
            v.warn(p, ln, f"'{items[i]}' is listed twice")
        else:
            out.append(idx)
    if len(out) > COMBO_MAX:
        v.err(path, line, f"{len(out)} buttons: a combo has at most {COMBO_MAX}")
    return sorted(out)


def combo_and_hold(v, dev, data, key, keys, default_ms):
    """`key: [ids]` or `key: { combo: [ids], hold: 2s }` (keys: RESET_COMBO_KEYS or UNLOCK_KEYS) -> (indices
    or None if not given, hold ms, combo given). combo may be left out of the mapping (None then)."""
    x, line = data[key], data.line_of(key)
    if isinstance(x, list):
        return combo_buttons(v, dev, x, key, line), default_ms, True
    d = v.mapping(x, key, line, keys.names)
    if d is None:
        return [], default_ms, True
    hold = keys.get(v, d, "hold", key, default=default_ms)
    if d.get("combo") is None:
        return None, hold, False
    return combo_buttons(v, dev, d["combo"], f"{key}.combo", d.line_of("combo")), hold, True


def validate_safety(v, dev, data):
    """factory_reset_combo:, unlock: and status_led: (sysex.md 4.9; core/README.md)."""
    boot = set(dev.boot_combo)
    if data.get("factory_reset_combo") is not None:
        line = data.line_of("factory_reset_combo")
        combo, dev.reset_hold_ms, given = combo_and_hold(v, dev, data, "factory_reset_combo", RESET_COMBO_KEYS,
                                                          RESET_HOLD_DEFAULT_MS)
        if not given:
            v.err("factory_reset_combo", line, "needs `combo:` (the button ids, e.g. `[oct_down, oct_up]`)")
        combo = combo or []
        if combo and boot and (set(combo) <= boot or boot <= set(combo)):
            names = lambda ix: " + ".join(dev.buttons[i].id for i in sorted(ix))   # noqa: E731
            how = "the same buttons as" if set(combo) == boot else (
                "inside" if set(combo) < boot else "a superset of")
            v.err("factory_reset_combo", line,
                  f"{names(combo)} is {how} boot_combo ({names(boot)}): held at power-up, one would "
                  "always set off the other. Pick buttons so that neither combo contains the other")
        dev.reset_combo = combo
    if "unlock" in data:
        x, line = data["unlock"], data.line_of("unlock")
        if x is False:
            dev.unlock = []
        elif x is True or x is None:
            dev.unlock = sorted(boot)
            if not boot:
                v.err("unlock", line, "unlock: true needs a combo: set `unlock: { combo: [...] }` or "
                                      "boot_combo (or `unlock: false`)")
        else:
            combo, dev.unlock_hold_ms, given = combo_and_hold(v, dev, data, "unlock", UNLOCK_KEYS,
                                                             UNLOCK_HOLD_DEFAULT_MS)
            if not given:
                combo = sorted(boot)
                if not boot:
                    v.err("unlock", line, "needs `combo:` (there is no boot_combo to default to)")
            dev.unlock = combo or []
    # no unlock: -> off (opt-in), even with a boot_combo
    if data.get("status_led") is not None:
        idx = v.ref(data, "status_led", "status_led", data.line_of("status_led"), "LED", "LEDs")
        if idx is not None:
            dev.status_led = idx


# The "safety" area: safety and storage (core/yaml/safety.md). boot_combo, the combos' buttons and status_led
# are ids, checked by validate() and validate_safety.
SAFETY_AREA = _Keys(
    _Key("boot_combo", _List(_IdRef("button")), see='core/README.md "Recovery and safety"',
         doc="Buttons held at reset: the NUC stays in its bootloader, an ESP starts in safe mode (default settings). "
             "The first 32 buttons only."),
    _Key("factory_reset_combo", _OneOf(_List(_IdRef("button")), _Map(RESET_COMBO_KEYS)),
         see='core/README.md "Factory-reset combo"',
         doc=f"Buttons held at power-up to wipe the settings (every preset, its name and the globals), for "
             f"{RESET_HOLD_DEFAULT_MS // 1000} s; or a mapping with the hold time."),
    _Key("unlock", _OneOf(_Bool(), _Map(UNLOCK_KEYS)), default=False, see='core/README.md "Physical unlock"',
         doc="Opt-in physical unlock: uploads, REBOOT to the bootloader and FACTORY_RESET over SysEx wait for the "
             f"user to hold a combo on the device. `true`: the `boot_combo` buttons, "
             f"{UNLOCK_HOLD_DEFAULT_MS // 1000} s."),
    _Key("status_led", _IdRef("LED"), see='core/README.md "Status LED"',
         doc="The LED that shows the blink codes; without it there are none."),
    _Key("write_to_flash", _OneOf(_Bool(), _Map(WTF_KEYS)), default=False, see=_WTF_SEE,
         doc="What the device writes to its flash on its own (host saves always work); `true` / `false`: every "
             "item on or off, with the default timings."),
)


def combo_mask(ix):
    """Button indices -> a C mask constant, 32-bit when it fits."""
    m = sum(1 << i for i in ix)
    return f"0x{m:X}u" if m < (1 << 32) else f"0x{m:X}ull"


# ---------------------------------------------------------------------------------------------
# storage budget (research/core/presets-design.md §10.5)

def record_bytes(n_values):
    """Worst-case bytes of one stored record holding n_values values (with a name)."""
    return 4 * (REC_HEADER_WORDS + REC_NAME_WORDS + 2 * n_values + REC_CRC_WORDS)


def packed_record_bytes(dev, scope):
    """Worst-case bytes of one packed (v3) record of scope's params, with a name: one group per param
    (platform/stm32f1/src/config_store.c), every value stored. A group of one int8 value is 1 word, of
    one int16 value 2; of count > 1 values a header, ceil(count / 32) bitmap words and the values, 4
    (int8) or 2 (int16) to a word."""
    words = 0
    for name, _i, s, count in PARAMS:
        c = count(dev)
        if s != scope or not c:
            continue
        wide = name in WIDE_PARAMS or (name in NRPN_WIDE_PARAMS and "nrpn" in getattr(dev, "msg", ()))
        per = 2 if wide else 4
        words += (1 if per == 4 else 2) if c == 1 else 1 + -(-c // 32) + -(-c // per)
    return 4 * (REC_HEADER_WORDS + REC_NAME_WORDS + words + REC_CRC_WORDS)


def store_budget_of(dev):
    """The platform's store (STORE_BUDGETS), with the page size and count from the board profile."""
    b = STORE_BUDGETS.get(dev.platform.name)
    if b is None or getattr(dev, "board", None) is None:
        return b
    page, pages = gbd.store_budget(dev.board)
    return dict(b, page=page, pages=pages)


def storage_need(plat_name, preset_rec, global_rec, slots):
    """-> (record_ok, used, capacity, unit) for slots presets + the globals at worst case.
    unit is "pages" for a journal, "B" otherwise. plat_name: a platform name, or its budget (a dict)."""
    b = plat_name if isinstance(plat_name, dict) else STORE_BUDGETS.get(plat_name)
    if b is None:
        return True, 0, 0, "B"
    big = max(preset_rec, global_rec)
    if b["kind"] == "journal":
        page = b["page"]
        if big > page:
            return False, 0, b["pages"] - b["spare"], "pages"
        per_page = page // preset_rec
        pages = -(-slots // per_page)
        left = page - ((slots % per_page) or per_page) * preset_rec
        if global_rec > left:
            pages += 1
        return True, pages, b["pages"] - b["spare"], "pages"
    total = slots * preset_rec + global_rec
    if b["kind"] == "log":
        page, n = b["page"], b["pages"]
        return big <= page, total, (n - 1) * page - (n - 2) * big, "B"
    limit = b.get("record", b["total"])
    return big <= limit, total, b["total"], "B"


def storage_budget(dev):
    """The worst case of what the device stores, against its platform's store. -> dict (also shown by
    gemifi_build.py and the dashboard)."""
    n_preset, n_global = stored_counts(dev)
    b = store_budget_of(dev)
    if b and b.get("format") == "packed":
        prec, grec = packed_record_bytes(dev, PRESET), packed_record_bytes(dev, GLOBAL)
    else:
        prec, grec = record_bytes(n_preset), record_bytes(n_global)
    ok, used, cap, unit = storage_need(b, prec, grec, dev.n_presets)
    fits = [s for s in range(1, MAX_PRESETS + 1)
            if (lambda r: r[0] and r[1] <= r[2])(storage_need(b, prec, grec, s))]
    calm = [s for s in fits if storage_need(b, prec, grec, s)[1] <=
            BUDGET_WARN * storage_need(b, prec, grec, s)[2]]
    return {"slots": dev.n_presets, "preset_values": n_preset, "global_values": n_global,
            "preset_record": prec, "global_record": grec, "record_ok": ok,
            "record_limit": None if b is None else b.get("record") or b.get("page") or b["total"],
            "worst_bytes": dev.n_presets * prec + grec, "used": used, "capacity": cap, "unit": unit,
            "fits": ok and used <= cap, "max_slots": max(fits) if fits else 0,
            "max_slots_calm": max(calm) if calm else 0, "store": b and b["what"],
            "page": b and b.get("page"), "pages": b and b.get("pages"), "kind": b and b["kind"]}


def budget_text(bd):
    """'8 slots, worst case 1836 B: 5 of 6 pages' (for the summary line)."""
    s = f"{bd['slots']} slot{'s' if bd['slots'] != 1 else ''}, worst case {bd['worst_bytes']} B"
    if bd["unit"] == "pages":
        return s + f" in {bd['used']} of {bd['capacity']} pages ({bd['capacity'] * bd['page']} B)"
    if bd["capacity"]:
        return s + f" of {bd['capacity']} B"
    return s


def check_budget(v, dev, data):
    bd = dev.budget = storage_budget(dev)
    if bd["store"] is None or dev.slots_invalid:
        return              # (an invalid presets.slots: already reported, and n_presets isn't the user's)
    ps = data.get("presets") if dev.presets_given else None
    path = "presets.slots" if isinstance(ps, dict) and "slots" in ps else "presets" if ps is not None else ""
    line = (ps.line_of("slots") if path == "presets.slots" else data.line_of("presets")) if ps is not None \
        else data.line
    where = f"{dev.platform.name} {bd['store']}"
    recs = (f"a preset record is up to {bd['preset_record']} B ({bd['preset_values']} per-preset values), "
            f"the globals' {bd['global_record']} B ({bd['global_values']} values)")
    if not bd["record_ok"]:
        # Without presets: it was already so with the v1 record (the device works, but saving every
        # value changed at once fails), so it stays a warning there.
        (v.err if dev.presets_given else v.warn)(
            path, line, f"the settings don't fit one record of the {where} (at most {bd['record_limit']} B): "
                        f"{recs}, so saving many changed values at once would fail. Fewer buttons / analog "
                        "inputs / encoders would fit")
        return
    if bd["unit"] == "pages":
        need = (f"{bd['slots']} presets + the globals need up to {bd['used']} of the {bd['capacity']} usable "
                f"{bd['page']} B pages (2 of {bd['capacity'] + 2} are kept free for rewrites)")
    elif bd["kind"] == "log":
        need = (f"{bd['slots']} presets + the globals need up to {bd['worst_bytes']} of {bd['capacity']} B "
                f"({bd['pages']} pages of {bd['page']} B, less the room one compaction needs)")
    else:
        need = f"{bd['slots']} presets + the globals need up to {bd['worst_bytes']} of {bd['capacity']} B"
    if not bd["fits"]:
        v.err(path, line, f"presets don't fit the {where}: {need}; {recs}. At most "
                          f"{bd['max_slots']} slots fit (slots: {bd['max_slots']})")
    elif bd["used"] > BUDGET_WARN * bd["capacity"]:
        v.warn(path, line, f"presets use {100 * bd['used'] // bd['capacity']}% of the {where} at worst case: "
                           f"{need}, so a full store means more erase cycles. Up to {bd['max_slots']} slots "
                           f"fit; {bd['max_slots_calm']} or fewer stay under {int(BUDGET_WARN * 100)}%")


def load_device(path, text=None, subs=None, preset_dir=None):
    """Load, resolve (packages, !include, substitutions: gemifi_yaml.py) and validate a device YAML.
    subs: {name: text} that override substitutions: (the CLI's -s). Returns (Device or None, [Diag])."""
    import gemifi_yaml      # it builds on this module's loader
    try:
        if text is None:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        data = load_yaml(text)
    except OSError as e:
        return None, [Diag("error", "", 0, f"can't read: {e.strerror}", path)]
    except UnicodeDecodeError:
        return None, [Diag("error", "", 0, "not UTF-8 text", path)]
    except YamlLoadError as e:
        return None, [Diag("error", "", e.line, e.msg, path)]
    res = gemifi_yaml.resolve(data, path, subs=subs, preset_dir=preset_dir)
    diags = res.diags
    dev = None
    if not errors(diags):
        digest = gemifi_yaml.config_hash(res.data)     # before validate(), which may add to the tree
        dev, more = validate(res.data, path)
        diags += more
    for d in diags:
        d.file = d.src or path          # a problem in a package or !include names that file
    if dev is not None:
        dev.config_hash = digest
        dev.sources = res.files
    return dev, diags


# ---------------------------------------------------------------------------------------------
# code generation

# c_str: components/api.py (imported above).


def generate(dev, source="device.yaml"):
    """-> {"gm_device_config.h": str, "gm_device_config.c": str, "gm_components.h": str}"""
    src = c_comment(source)
    head = (f"/* gm_device_config: do not edit; generated from {src}\n"
            f" * by tools/builder/gemifi_build.py (format: core/README.md). */\n")
    m = dev.matrix
    h = [head, "#ifndef GM_DEVICE_CONFIG_H", "#define GM_DEVICE_CONFIG_H", "",
         f"#define GM_DEVICE_NAME {c_str(dev.name)}   /* friendly_name */",
         f"#define GM_NODE_NAME {c_str(dev.node_name)}",
         f"#define GM_BOARD_ID 0x{dev.board_id:04X}",
         *([f"#define GM_FW_VERSION 0x{dev.version[0] << 24 | dev.version[1] << 16 | dev.version[2]:08X}u",
            f"#define GM_FW_VERSION_STR \"{'.'.join(map(str, dev.version))}\""] if dev.version else []),
         f"#define GM_PLATFORM {c_str(dev.platform.name)}",
         *([f"#define GM_BOARD_PROFILE {c_str(dev.board['id'])}   /* board: (platform/{dev.platform.name}/"
            f"boards/{dev.board['id']}.json) */"] if getattr(dev, "board", None) else []),
         *([f"#define GM_CONFIG_HASH 0x{dev.config_hash:08X}   /* config.hash: the resolved YAML's "
            "(core/README.md \"Config hash\") */"] if dev.config_hash is not None else []),
         f"#define GM_USB_VID 0x{dev.usb[0]:04X}",
         f"#define GM_USB_PID 0x{dev.usb[1]:04X}",
         f"#define GM_N_HUBS {len(dev.hubs)}",
         f"#define GM_HUB_TYPES 0x{hub_types_mask(dev):04X}u /* hub types used, bit = GM_HUB_*: "
         "hub_ops[] holds only their drivers (gm_components.h) */",
         f"#define GM_N_BUTTONS {len(dev.buttons)}",
         f"#define GM_N_LEDS {len(dev.leds)}",
         f"#define GM_N_ANALOGS {len(dev.analogs)}",
         f"#define GM_N_ENCODERS {len(dev.encoders)}",
         f"#define GM_HAS_MATRIX {1 if m else 0}",
         f"#define GM_MATRIX_KEYS {m.keys if m else 0}",
         f"#define GM_MATRIX_VELOCITY {1 if m and m.kind == 'velocity' else 0}",
         f"#define GM_HAS_I2C {1 if any(x.type in I2C_HUBS for x in dev.hubs) else 0}",
         f"#define GM_HW_SCAN {1 if dev.hw_scan else 0} /* hardware_scan.matrix */",
         f"#define GM_HW_ADC {1 if dev.hw_adc else 0} /* hardware_scan.analog */"]
    if dma_text(dev):
        h.append(f"/* {dma_text(dev)} */")
        h.append(f"/* {timers_text(dev)} */")
    if dev.hw_scan:
        row, settle, pre = dev.hw_scan
        h += [f"#define GM_HW_SCAN_ROW_NS {row}",
              f"#define GM_HW_SCAN_SETTLE_NS {settle}",
              f"#define GM_HW_SCAN_PRECHARGE {1 if pre else 0}"]
    h += hw_scan_sizes(dev)
    h += timer_defines(dev)
    h += key_matrix.c_defines(dev)
    hub_ram = sum(x.ram_bytes for x in dev.hubs if x.type in RAM_HUBS)
    if hub_ram:
        h.append(f"#define GM_HUB_RAM {hub_ram} /* hub RAM bytes (gm_hub_ram, components/hub): its LED strips' and drivers' */")
    strips = sum(1 for x in dev.hubs if x.type == "ws2812")
    if strips:
        h.append(f"#define GM_LEDSTRIPS {strips} /* ws2812 hubs (the platform's hal_ledstrip_*) */")
    if dev.platform.name == "stm32f1":
        for x in dev.hubs:
            spi = Stm32F1Platform.LEDSTRIP_SPI.get(x.data.num) if x.type == "ws2812" and x.data.hub == 0 else None
            if spi:
                h.append(f"#define GM_LEDSTRIP_SPI{spi}_BYTES {x.ram_bytes} /* ws2812 '{c_comment(x.id)}' on "
                         f"{x.data.desc}: a frame's bytes (platform/stm32f1/src/hal.c) */")
    if any(x.type in TM_HUBS and x.fade for x in dev.hubs):
        h.append("#define GM_TM_FADE 1 /* a tm1628 hub with fade: */")
    h += key_matrix.c_param_defaults(dev)
    h += screen_defines(dev)
    h += i2c_buses.c_defines(dev)
    h += feedback_defines(dev)
    h += ["", "/* presets (research/core/presets-design.md §10.1) */",
          f"#define GM_N_PRESETS {dev.n_presets} /* " +
          ("presets.slots */" if dev.presets_given else "no presets: in the YAML */"),
          f"#define GM_PRESETS_REVISION {dev.presets_revision} /* presets.revision */",
          f"#define GM_N_YAML_PRESETS {dev.n_yaml_presets} /* slots 1..N come from presets.list */",
          f"#define GM_N_MODES {len(dev.modes)} /* modes: (components/modes/README.md \"Modes\") */"]
    h += modes.c_rgb_defines(dev)
    h += write_to_flash_defines(dev)
    h += modes.c_layer_defines(dev)
    h += encoders.c_defines(dev)
    h += msg_defines(dev)
    h += buttons.c_defines(dev)
    h += modes.c_defines(dev)
    h += pads.c_defines(dev)
    h += display_defines(dev)
    h += safety_defines(dev)
    h += layout_defines(dev)
    if dev.platform.is_esp:
        h += ["", "#ifndef GM_MIDI_USB", "#define GM_MIDI_USB 1", "#define GM_MIDI_SERIAL 2",
              "#define GM_MIDI_BLE 3", "#endif",
              f"#define GM_MIDI_TRANSPORT {MIDI_TRANSPORT_MACRO[dev.transport]}"]
    h += ["", "#endif", ""]

    c = [head, '#include "gm_device.h"', '#include "gm_device_config.h"', ""]

    if dev.hubs:
        for hb in dev.hubs:
            c += hb.kind.c_tables(hb)
        c.append("static const gm_hub_t hubs[GM_N_HUBS] = {")
        for hb in dev.hubs:
            fields = hb.kind.c_fields(hb)
            c += [f"    {{ /* {hb.index + 1}: {c_comment(hb.id)} ({hb.type}) */",
                  f"        .type = {hb.kind.enum}, {fields[0].lstrip()}", *fields[1:], "    },"]
        c += ["};", ""]
    c += buttons.c_tables(dev)
    c += leds.c_tables(dev)
    c += analog.c_tables(dev)
    c += encoders.c_tables(dev)
    c += key_matrix.c_tables(dev)
    combo = " | ".join(f"(1u << {i})" for i in sorted(dev.boot_combo)) or "0"
    c += ["const gm_device_t gm_device = {",
          "    .name = GM_DEVICE_NAME,",
          "    .node_name = GM_NODE_NAME,",
          "    .board_id = GM_BOARD_ID,",
          "    .usb_vid = GM_USB_VID,",
          "    .usb_pid = GM_USB_PID,",
          f"    .hubs = {'hubs' if dev.hubs else '0'}, .n_hubs = GM_N_HUBS,",
          f"    .buttons = {'buttons' if dev.buttons else '0'}, .n_buttons = GM_N_BUTTONS,",
          f"    .leds = {'leds' if dev.leds else '0'}, .n_leds = GM_N_LEDS,",
          f"    .analogs = {'analogs' if dev.analogs else '0'}, .n_analogs = GM_N_ANALOGS,",
          f"    .encoders = {'encoders' if dev.encoders else '0'}, .n_encoders = GM_N_ENCODERS,",
          f"    .matrix = {'&matrix' if m else '0'},",
          f"    .boot_combo = {combo},",
          f"    .scan_us = {dev.scan_us},",
          "};", "",
          "/* param defaults; button.action values: components/buttons/README.md \"Button actions\" */"]
    c += buttons.c_param_defaults(dev)
    c += analog.c_param_defaults(dev)
    c += encoders.c_param_defaults(dev)
    c += [""]
    c += msg_tables(dev)
    c += feedback_tables(dev)
    c += preset_tables(dev)
    c += modes.c_tables(dev)
    c += pads.c_tables(dev)
    c += screen_tables(dev)
    c += display_tables(dev)
    c += layout_tables(dev)
    c += label_tables(dev)
    return {"gm_device_config.h": "\n".join(h), "gm_device_config.c": "\n".join(c),
            **component_header(dev, source)}


def safety_defines(dev):
    """factory_reset_combo:, unlock: and status_led: (core/src/safety.c, app_proto.c)."""
    names = lambda ix: " + ".join(dev.buttons[i].id for i in ix)   # noqa: E731
    h = ["", "/* safety (core/README.md \"Factory-reset combo\", \"Physical unlock\", \"Status LED\") */"]
    if dev.unlock:
        reply = 5 + sum(1 + min(31, len(dev.buttons[i].name)) for i in dev.unlock)
        h += ["#define GM_UNLOCK 1",
              f"#define GM_UNLOCK_COMBO {combo_mask(dev.unlock)} /* {names(dev.unlock)} */",
              f"#define GM_UNLOCK_N {len(dev.unlock)}",
              f"#define GM_UNLOCK_HOLD_MS {dev.unlock_hold_ms}",
              f"#define GM_UNLOCK_REPLY {reply} /* UNLOCK reply payload bytes */"]
    else:
        h.append("#define GM_UNLOCK 0")
    if dev.reset_combo:
        h += [f"#define GM_RESET_COMBO {combo_mask(dev.reset_combo)} /* {names(dev.reset_combo)} */",
              f"#define GM_RESET_HOLD_MS {dev.reset_hold_ms}"]
    else:
        h.append("#define GM_RESET_COMBO 0")
    led = dev.status_led
    h.append(f"#define GM_STATUS_LED {led if led is not None else '(-1)'}"
             + (f" /* {dev.leds[led].id} */" if led is not None else ""))
    return h

HW_SCAN_MAX_ROWS = 32   # gm_controller.h GM_SCAN_MAX_ROWS
HW_ADC_MAX_STEPS, HW_ADC_MAX_PINS = 16, 8   # gm_controller.h GM_ADC_MAX_STEPS, GM_ADC_MAX_PINS


def hw_scan_sizes(dev):
    """The largest plans the hardware scans can get (gm_scan_plan, gm_adc_plan), so the
    platform's DMA rings and the readings are sized for this device, not the maximum:
    GM_HW_SCAN_ROWS = key rows + switch-matrix rows (vpin_scan_plan: those within the 32-bit
    cache; shared selects only make it fewer), even; GM_HW_ADC_PINS / _STEPS = the distinct
    MCU ADC pins (direct, or a mux's common) and the most mux steps among the analog inputs
    and pads (xy_pad: read in software); GM_HW_ADC_PEAK_PINS = those of them the pads are on
    (core/src/vpin.c keeps a sweep's highest and lowest readings for those pins' cells only)."""
    out = []
    if dev.hw_scan:
        rows = len(dev.matrix.rows) if dev.matrix else 0
        for h in dev.hubs:
            if h.type == "switch_matrix":
                rows += sum(1 for r in range(len(h.rows)) if r * len(h.cols) < 32)
        rows = min(max(rows + (rows & 1), 2), HW_SCAN_MAX_ROWS)
        out.append(f"#define GM_HW_SCAN_ROWS {rows} /* hardware scan plan: rows at most */")
    if dev.hw_adc:
        pins, pad_pins, steps = set(), set(), 1
        sources = [(a.pin, False) for a in dev.analogs] + [(p.pin, True) for p in dev.pads]
        for pin, pad in sources:
            adc = None
            if pin.hub == 0:
                adc = pin.num
            elif 0 < pin.hub <= len(dev.hubs):
                h = dev.hubs[pin.hub - 1]
                if h.type in MUX_HUBS and h.common is not None and h.common.hub == 0:
                    adc = h.common.num
                    steps = max(steps, 1 << h.n_sel)
            if adc is not None:
                pins.add(adc)
                if pad:
                    pad_pins.add(adc)
        out.append(f"#define GM_HW_ADC_PINS {min(max(len(pins), 1), HW_ADC_MAX_PINS)} "
                   "/* hardware analog scan: ADC pins at most */")
        out.append(f"#define GM_HW_ADC_STEPS {min(steps, HW_ADC_MAX_STEPS)} /* ... mux steps at most */")
        out.append(f"#define GM_HW_ADC_PEAK_PINS {min(len(pad_pins), HW_ADC_MAX_PINS)} "
                   "/* ... of them with pads: peaks kept */")
    return out


def label_tables(dev):
    """The settings' index labels (core/src/settings.c): each component's display name, as the gm_device
    tables have it, in flash. Pad names only with pads, mode names only with modes."""
    def table(cname, count, names):
        body = ", ".join(c_str(n) for n in names) or "0"
        return f"const char *const {cname}[{count if names else '1'}] = {{ {body} }};"

    c = ["/* index labels of the per-component settings: the components' names (core/src/settings.c) */",
         table("gm_button_names", "GM_N_BUTTONS", [b.name for b in dev.buttons]),
         table("gm_analog_names", "GM_N_ANALOGS", [a.name for a in dev.analogs]),
         table("gm_encoder_names", "GM_N_ENCODERS", [e.name for e in dev.encoders])]
    if dev.pads:
        c.append(table("gm_pad_names", "GM_N_PADS", [x.name for x in dev.pads]))
    if dev.modes:
        c.append(table("gm_mode_names", "GM_N_MODES", [m.name if m.name is not None else m.id for m in dev.modes]))
    return c + [""]


OWNER_NAMES = {n: k for k, n in LED_OWNERS.items()}
SOURCE_ENUM = {0: "GM_FB_NONE", 1: "GM_FB_NOTE", 2: "GM_FB_CC", 3: "GM_FB_CLOCK", 4: "GM_FB_PLAYING", 5: "GM_FB_MCU"}
OWNER_ENUM = {0: "GM_OWNER_HOST_THEN_LOCAL", 1: "GM_OWNER_HOST", 2: "GM_OWNER_LOCAL"}


def fmt_timeout(t):
    if t == TIMEOUT_NEVER:
        return "never"
    return f"{t / 100:g}s" if t % 10 == 0 and t >= 100 else fmt_us(t * 10_000)


def led_fb_rows(dev):
    """Per LED (source, num, channel, owner, timeout); None when every LED uses the device defaults
    (then no table is generated)."""
    rows, special = [], False
    # A cycle's LED always shows its step (modes.c mode_led), whatever a host sends: say so
    # (GET_COMPONENT owner), so DAW scripts and the feedback demo leave it alone.
    cycle_leds = {m.led for m in getattr(dev, "modes", []) if m.cycle and m.led is not None}
    for i, x in enumerate(dev.leds):
        src = x.source or (0, 0, 0)
        owner = x.owner
        if owner is None:
            mcu_driven = src[0] == LED_SOURCES["mcu"] or any(
                k == "ring" or a == "mcu" for k, a in x.used_by)
            owner = (LED_OWNERS["host"] if mcu_driven else LED_OWNERS["local"] if i in cycle_leds
                     else dev.led_owner)
        timeout = dev.led_timeout if x.timeout is None else x.timeout
        if src[0] or owner != dev.led_owner or timeout != dev.led_timeout:
            special = True
        rows.append((src, owner, timeout))
    return rows if special else None


def timer_defines(dev):
    """stm32f1: the timers and DMA requests allocate_dma picked (platform/stm32f1/include/dma_map.h).
    Requests: 0 = update, k = compare k; a _CH is its DMA1 channel (0 = not used)."""
    h = []
    if "matrix" in dev.scan_timers:
        t, roles = dev.scan_timers["matrix"]
        h.append(f"#define GM_HW_SCAN_TIM {t} /* hardware_scan.matrix on TIM{t} */")
        for role in ("sel", "in", "rd", "pre"):
            q = roles.get(role)
            what = f"{tim_req(t, q)}: {HW_SCAN_ROLE_NAMES[role]}" if q is not None else "no precharge"
            h += [f"#define GM_HW_SCAN_{role.upper()} {q or 0} /* {what} */",
                  f"#define GM_HW_SCAN_{role.upper()}_CH {STM32F1_DMA[tim_req(t, q)] if q is not None else 0}"]
    if "analog" in dev.scan_timers:
        t, roles = dev.scan_timers["analog"]
        q = roles["mux"]
        h += [f"#define GM_HW_ADC_TIM {t} /* hardware_scan.analog on TIM{t}: ADC1 by {HW_ADC_TRIGGER[t]} */",
              f"#define GM_HW_ADC_MUX {q} /* {tim_req(t, q)}: mux step */",
              f"#define GM_HW_ADC_MUX_CH {STM32F1_DMA[tim_req(t, q)]}"]
    if "tick" in dev.scan_timers:
        h.append(f"#define GM_SCAN_TICK_TIM {dev.scan_timers['tick']} /* the scan tick interrupt */")
    return h


def feedback_defines(dev):
    mcu = dev.mcu
    pickup = any(a.pickup for a in dev.analogs)
    h = ["", "/* host feedback / DAW (core/README.md \"DAW feedback\") */",
         f"#define GM_FEEDBACK {1 if f1_on(dev) else 0}",
         f"#define GM_LED_FB {1 if led_fb_rows(dev) else 0}",
         f"#define GM_LED_OWNER {dev.led_owner} /* {OWNER_NAMES[dev.led_owner]} */",
         f"#define GM_LED_TIMEOUT {dev.led_timeout} /* {fmt_timeout(dev.led_timeout)}, 10 ms units */",
         f"#define GM_ANALOG_PICKUP {1 if pickup else 0}",
         f"#define GM_ANALOG_TOUCH {1 if any(a.touch for a in dev.analogs) else 0} /* analog touch: */",
         f"#define GM_MCU {1 if mcu else 0}",
         f"#define GM_HOST_FB {1 if dev.host_fb else 0} /* DAW-script messages (sysex.md 4.6) */",
         f"#define GM_RINGS {1 if mcu or dev.host_fb else 0}",
         f"#define GM_USB_DAW_PORT {1 if dev.daw_port else 0}"]
    if mcu:
        h += [f"#define GM_MCU_CABLE {mcu.cable} /* {'the DAW port' if mcu.cable else 'the only port'} */",
              f"#define GM_MCU_ID 0x{0x15 if mcu.extender else 0x14:02X} /* {'MCU XT' if mcu.extender else 'MCU'} */"]
    if dev.daw_port:
        h.append(f"#define GM_DAW_PORT_NAME {c_str(dev.name + ' DAW')}")
    din = dev.din
    h.append(f"#define GM_DIN {1 if din else 0} /* din_midi: (core/README.md \"DIN MIDI\") */")
    if din:
        rx = din.rx.num if din.rx is not None else 0xFFFF
        h += [f"#define GM_DIN_MODE {din.mode} /* {DIN_MODES[din.mode]} */",
              f"#define GM_DIN_TX_PIN {din.tx.num} /* {din.tx.desc} */",
              f"#define GM_DIN_RX_PIN {rx} /* {din.rx.desc if din.rx is not None else 'no MIDI In'} */",
              f"#define GM_DIN_THRU_DEFAULT {1 if din.thru else 0}",
              f"#define GM_DIN_OUT_DEFAULT {1 if din.out else 0}",
              f"#define GM_DIN_USB_DEFAULT {1 if din.usb else 0}",
              f"#define GM_DIN_DMA {1 if din.dma else 0} /* din_midi.dma */"]
        if din.mode == DIN_MODES.index("separate_port"):
            name = din.port_name or (dev.name + " MIDI")[:COMP_STR_MAX]
            h += ["#define GM_DIN_CABLE 1", f"#define GM_DIN_PORT_NAME {c_str(name)}"]
    return h


def feedback_tables(dev):
    c = []
    rows = led_fb_rows(dev)
    if rows:
        c.append("const gm_led_fb_t gm_led_fb[GM_N_LEDS] = {")
        for x, (src, owner, timeout) in zip(dev.leds, rows):
            to = "GM_TIMEOUT_NEVER" if timeout == TIMEOUT_NEVER else str(timeout)
            c.append(f"    {{ {SOURCE_ENUM[src[0]]}, {src[1]}, {src[2]}, {OWNER_ENUM[owner]}, {to} }}, "
                     f"/* {c_comment(x.id)} */")
        c += ["};", ""]
    if any(a.pickup for a in dev.analogs):
        c.append("const uint8_t gm_analog_pickup[GM_N_ANALOGS] = { " +
                 ", ".join("1" if a.pickup else "0" for a in dev.analogs) + " };")
        c.append("")
    if any(a.touch for a in dev.analogs):
        c.append("const uint8_t gm_analog_touch[GM_N_ANALOGS] = { " +
                 ", ".join(str(a.touch) for a in dev.analogs) + " }; /* GM_TOUCH_* */")
        c.append("")
    if dev.mcu:
        def mcu_arr(lst):
            return ", ".join("GM_MCU_NONE" if x.mcu is None else str(x.mcu) for x in lst) or "GM_MCU_NONE"
        c.append(f"const uint8_t gm_analog_mcu[{'GM_N_ANALOGS' if dev.analogs else '1'}] = "
                 f"{{ {mcu_arr(dev.analogs)} }};")
        c.append(f"const uint8_t gm_encoder_mcu[{'GM_N_ENCODERS' if dev.encoders else '1'}] = "
                 f"{{ {mcu_arr(dev.encoders)} }};")
        c.append("")
    if dev.mcu or dev.host_fb:
        rings = []
        for i, e in enumerate(dev.encoders):
            center = "GM_RING_NO_CENTER" if getattr(e, "ring_center", None) is None else str(e.ring_center)
            if e.ring:
                c.append(f"static const uint8_t ring_{i}[{len(e.ring)}] = {{ {', '.join(map(str, e.ring))} }}; "
                         f"/* {c_comment(e.id)} */")
                rings.append(f"{{ ring_{i}, {len(e.ring)}, {center} }}")
            else:
                rings.append(f"{{ 0, 0, {center} }}")
        c.append(f"const gm_ring_t gm_encoder_ring[{'GM_N_ENCODERS' if dev.encoders else '1'}] = "
                 f"{{ {', '.join(rings) or '{ 0, 0, GM_RING_NO_CENTER }'} }};")
        c.append("")
    if dev.host_fb:
        c += component_tables(dev)
    return c


MCU_NOTE_NAMES = {n: k for k, n in MCU_NOTES.items()}
MCU_FADER_NAMES = {n: k for k, n in MCU_FADERS.items()}
MCU_VPOT_NAMES = {n: k for k, n in MCU_VPOTS.items()}


def comp_role(x, mcu_name=None):
    """A component's role for GET_COMPONENT: daw:, else its mcu: name (channel_left / channel_right as
    strip_prev / strip_next), else empty (sysex.md 4.6.1)."""
    return getattr(x, "daw", None) or MCU_ROLES.get(mcu_name, mcu_name) or ""


def comp_str(s):
    return c_str(str(s)[:COMP_STR_MAX])


def component_tables(dev):
    """GET_COMPONENT's id / name / role per component (sysex.md 4.6.1)."""
    c = ["/* DAW-script component descriptions: { id, name, role } (sysex.md 4.6.1) */"]

    def table(cname, count, rows):
        body = ", ".join(f"{{ {comp_str(i)}, {comp_str(n)}, {comp_str(r)} }}" for i, n, r in rows) \
            or '{ "", "", "" }'
        c.append(f"const gm_comp_t {cname}[{count if rows else '1'}] = {{ {body} }};")

    table("gm_comp_buttons", "GM_N_BUTTONS",
          [(b.id, b.name, comp_role(b, MCU_NOTE_NAMES.get(b.number) if b.action_name == "mcu" else None))
           for b in dev.buttons])
    table("gm_comp_analogs", "GM_N_ANALOGS",
          [(a.id, a.name, comp_role(a, MCU_FADER_NAMES.get(a.mcu))) for a in dev.analogs])
    table("gm_comp_encoders", "GM_N_ENCODERS",
          [(e.id, e.name, comp_role(e, MCU_VPOT_NAMES.get(e.mcu))) for e in dev.encoders])
    table("gm_comp_leds", "GM_N_LEDS",
          [(x.id, x.name, comp_role(x, MCU_NOTE_NAMES.get(x.source[1])
                                    if x.source and x.source[0] == LED_SOURCES["mcu"] else None))
           for x in dev.leds])
    m = dev.matrix
    table("gm_comp_matrix", "1", [("keys", "Keys", comp_role(m))] if m else [])
    table("gm_comp_displays", "GM_N_DISPLAYS",
          [(x.id, x.id.replace("_", " ").capitalize() or f"Display {i + 1}", comp_role(x))
           for i, x in enumerate(dev.displays)])
    if dev.pads:
        table("gm_comp_pads", "GM_N_PADS + GM_PAD_TWINS" if dev.pad_twins else "GM_N_PADS",
              [(x.id, x.name, comp_role(x)) for x in dev.pads + dev.pad_twins])
    c.append("")
    return c


def preset_tables(dev):
    """gm_presets[]: slot n = gm_presets[n - 1]; each preset's overrides (vs slot 1) sorted by (id, index)."""
    c = ["/* presets: overrides of the slot 1 values, as { param id, index, value } (ids: gm_settings.h) */"]
    rows = []
    presets = dev.presets or [Hub(name="Default", program=-1, vals=[], listed=False)]
    for n, pr in enumerate(presets, 1):
        arr = "0"
        if pr.vals:
            arr = f"preset_{n}_vals"
            c.append(f"static const gm_preset_val_t {arr}[{len(pr.vals)}] = {{ /* {c_comment(pr.name)} */")
            c += [f"    {{ {pid}, {idx}, {val} }}, /* {c_comment(text)} */" for pid, idx, val, text in pr.vals]
            c.append("};")
        prog = "GM_PROGRAM_NONE" if pr.program < 0 else str(pr.program)
        rows.append(f"    {{ {c_str(pr.name)}, {prog}, {len(pr.vals)}, {arr} }}, /* slot {n} */")
    c += ["const gm_preset_def_t gm_presets[GM_N_PRESETS] = {", *rows, "};", ""]
    return c


def component_lists(dev, source):
    """gm_components.mk / gm_components.cmake: the components the device builds, their C sources and include
    dirs, under the repo root (GM_ROOT, set by the Makefile or CMakeLists that includes them)."""
    used = components.used(sorted({h.kind.key for h in dev.hubs}), sorted({x.type for x in dev.screens}),
                           components.controls(dev))
    srcs = [s for name in used for s in components.sources(name)]
    incs = [f"components/{name}" for name in used if components.has_headers(name)]
    head = (f"gm_components: do not edit; generated from {c_comment(source)} by tools/builder/gemifi_build.py.\n"
            "The components this device builds (components/<name>/), their C sources and include dirs, under the\n"
            "repo root GM_ROOT, which the including build sets.")
    mk = ["\n".join(f"# {line}" for line in head.split("\n")),
          f"GM_COMPONENTS := {' '.join(used)}",
          f"GM_COMPONENT_SRCS := {' '.join(f'$(GM_ROOT)/{s}' for s in srcs)}".rstrip(),
          f"GM_COMPONENT_INCS := {' '.join(f'$(GM_ROOT)/{d}' for d in incs)}".rstrip(), ""]
    cm = ["\n".join(f"# {line}" for line in head.split("\n")),
          f"set(GM_COMPONENTS {' '.join(used)})",
          f"set(GM_COMPONENT_SRCS {' '.join(f'${{GM_ROOT}}/{s}' for s in srcs)})",
          f"set(GM_COMPONENT_INCS {' '.join(f'${{GM_ROOT}}/{d}' for d in incs)})", ""]
    return {"gm_components.mk": "\n".join(mk), "gm_components.cmake": "\n".join(cm)}


def component_header(dev, source):
    """gm_components.h: the C side of the components the device builds: the headers of its hub types whose
    code is in components/ (HubType.c_ops; c_header, if it has one), and what they add to the hub base:
    GM_HUB_CONFIG_MEMBERS (gm_hub_t's union), GM_HUB_STATE_MEMBERS (gm_hub_state_t's), GM_HUB_OPS_EXTERNS
    and GM_HUB_OPS (core/src/vpin.c hub_ops[]); and what its screens' platforms add to the display base
    (components/display/gm_screen.h): GM_SCREEN_CONFIG_MEMBERS, GM_SCREEN_OPS_EXTERNS and GM_SCREEN_OPS."""
    kinds = sorted({h.kind.key: h.kind for h in dev.hubs if h.kind.c_ops}.values(), key=lambda t: (t.num, t.key))
    by_enum = {}
    for t in kinds:
        by_enum.setdefault(t.enum, t)       # types sharing an enum share their C parts
    parts = list(by_enum.values())
    screens = sorted({x.kind.platform: x.kind for x in dev.screens}.values(), key=lambda t: t.num)
    headers = list(dict.fromkeys([t.c_header for t in parts if t.c_header] + [t.c_header for t in screens]))

    def macro(name, items):
        return f"#define {name}" + "".join(f" \\\n    {i}" for i in items)
    lines = [f"/* gm_components.h: do not edit; generated from {c_comment(source)}",
             " * by tools/builder/gemifi_build.py. The C side of the components this device builds:",
             " * their headers, and what they add to the hub base (components/hub/gm_hub.h) and the",
             " * display base (components/display/gm_screen.h). */",
             "#ifndef GM_COMPONENTS_H", "#define GM_COMPONENTS_H", "",
             *[f'#include "{h}"' for h in headers], *([""] if headers else []),
             macro("GM_HUB_CONFIG_MEMBERS", [f"{t.c_config[0]} {t.c_config[1]};" for t in parts if t.c_config]),
             # platforms of one base may share its state (led_driver)
             macro("GM_HUB_STATE_MEMBERS", list(dict.fromkeys(f"{t.c_state[0]} {t.c_state[1]};" for t in parts
                                                              if t.c_state))),
             macro("GM_HUB_OPS_EXTERNS", [f"extern const gm_hub_ops_t {t.c_ops};" for t in parts]),
             macro("GM_HUB_OPS", [f"[{t.enum}] = &{t.c_ops}," for t in parts]),
             macro("GM_SCREEN_CONFIG_MEMBERS", [f"{t.c_config[0]} {t.c_config[1]};" for t in screens]),
             macro("GM_SCREEN_OPS_EXTERNS", [f"extern const gm_screen_ops_t {t.c_ops};" for t in screens]),
             macro("GM_SCREEN_OPS", [f"[{t.enum}] = &{t.c_ops}," for t in screens]),
             "", "#endif", ""]
    return {"gm_components.h": "\n".join(lines)}


def write_generated(dev, out_dir, source):
    os.makedirs(out_dir, exist_ok=True)
    files = {**generate(dev, source), **component_lists(dev, source)}
    for name, text in files.items():
        p = os.path.join(out_dir, name)
        try:
            with open(p, encoding="utf-8") as f:
                if f.read() == text:
                    continue        # unchanged: keep the mtime, so make doesn't rebuild everything
        except OSError:
            pass
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, p)
    return [os.path.join(out_dir, n) for n in files]


# The device's top-level keys by area (the docs' Core pages, core/yaml/<area>.md): each a Keys of top-level
# keys, which the JSON schema types and the docs table lists. Every top-level key is in an area or is a
# component's (KEY_SECTIONS, a hub list); tests/test_core_keys.py checks.
CORE_AREAS = {
    "device": DEVICE_KEYS,
    "midi": MIDI_AREA,
    "scanning": SCAN_AREA,
    "presets": PRESETS_AREA,
    "feedback": FEEDBACK_AREA,
    "displays": DISPLAYS_AREA,
    "layout": LAYOUT_AREA,
    "safety": SAFETY_AREA,
}
