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
import gemifi_board as gbd  # noqa: E402

try:
    import yaml
except ImportError:  # pragma: no cover - reported by gemifi_build.py
    yaml = None

YAML_HINT = ("PyYAML is required: `pip install pyyaml` (or `sudo apt install python3-yaml`, "
             "or `python3 -m venv tools/builder/.venv && tools/builder/.venv/bin/pip install pyyaml`)")

FORMAT_VERSION = 2
# The builder's version: min_version: is checked against it, and /api/info sends it.
# tools/release/set-version.mjs sets it at each release.
VERSION = "0.45.2"

# core/README.md "Button actions": YAML name -> button.action value (gm_settings.h ACT_*). The preset
# actions (13-15) need 2+ preset slots; `mcu` (16) only exists on devices with an `mcu:` section; the
# MIDI message actions (17-20) are compiled in only on devices that use them (MSG_ACTIONS).
ACTION_IDS = {"none": 0, "cc_momentary": 1, "cc_toggle": 2, "cc_ramp": 3, "note": 4, "octave_down": 5,
              "octave_up": 6, "transpose_down": 7, "transpose_up": 8, "bend_down": 9, "bend_up": 10,
              "program_next": 11, "program_prev": 12, "preset_next": 13, "preset_prev": 14,
              "preset_select": 15, "mcu": 16, "program": 17, "panic": 18, "mmc": 19, "sysex": 20,
              "pad_octave_down": 21, "pad_octave_up": 22}
ACTIONS = list(ACTION_IDS)
CC_ACTIONS = {"cc_momentary", "cc_toggle", "cc_ramp"}
PRESET_ACTIONS = {"preset_next", "preset_prev", "preset_select"}
PROGRAM_ACTIONS = {"program_next", "program_prev", "program"}     # these take bank:
PAD_ACTIONS = {"pad_octave_down", "pad_octave_up"}                  # only on devices with pads
# the YAML keys that give button.number (mcu: aside)
NUMBER_KEYS = ("cc", "note", "preset", "program", "mmc", "sysex")


def number_key(act):
    """The key that gives button.number for an action (None: the action takes no number)."""
    if act in ("program", "mmc", "sysex"):
        return act
    return "note" if act == "note" else "cc" if act in CC_ACTIONS else "preset" if act == "preset_select" else None


# ---- MIDI messages (core/README.md "MIDI messages") ----
# The message kinds a firmware may compile in (gm_settings.h GM_MSG_*): each is on when the YAML uses it
# or lists it in midi_messages.enable. sysex is on when midi_messages.sysex has strings.
MSG_FEATURES = ["cc14", "nrpn", "program", "panic", "mmc"]
MSG_ALIASES = {"rpn": "nrpn", "bank_select": "program"}
MSG_ACTIONS = {"program": "program", "panic": "panic", "mmc": "mmc", "sysex": "sysex"}   # action -> feature
# MMC commands (MIDI 1.0 MMC, `F0 7F dev 06 cmd F7`): the YAML's mmc: names -> button.number
MMC_COMMANDS = {"stop": 1, "play": 2, "deferred_play": 3, "fast_forward": 4, "rewind": 5, "record": 6,
                "record_exit": 7, "record_pause": 8, "pause": 9, "eject": 10, "chase": 11, "reset": 13}
SYSEX_MAX = 64              # bytes of one sent SysEx string (the stack buffer in controller.c)
SYSEX_TOKENS = {"v": 0x80, "msb": 0x81, "lsb": 0x82, "ch": 0x83}   # gm_device.h GM_SX_*
SX_SUM_START, SX_SUM = 0x84, 0x85
BANK_NONE = 0xFFFF          # gm_device.h GM_BANK_NONE
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
    """layer.enabled: one per mode, on a device with layer twins or a cycle (core/README.md "Cycles")."""
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
    ("preset.active", 17, GLOBAL, lambda d: 1),
    ("preset.program", 18, PRESET, lambda d: 1 if d.n_presets > 1 else 0),
    ("preset.dirty", 19, VOLATILE, lambda d: 1 if d.n_presets > 1 else 0),
    ("pad.note", 20, PRESET, lambda d: len(d.pads)),
    *((n, i, s, (lambda d: 1 if d.pads else 0)) for n, i, s in (
        ("pad.channel", 21, PRESET), ("pad.curve", 22, PRESET), ("pad.aftertouch", 23, PRESET),
        ("pad.threshold", 24, GLOBAL), ("pad.release", 25, GLOBAL), ("pad.full", 26, GLOBAL),
        ("pad.window", 27, GLOBAL), ("pad.retrigger", 28, GLOBAL), ("pad.octave", 40, PRESET))),
    ("analog.mode", 29, PRESET, lambda d: len(d.analogs)),
    ("midi.thru", 30, GLOBAL, lambda d: 1 if d.din else 0),
    ("midi.din_out", 31, GLOBAL, lambda d: 1 if d.din else 0),
    ("midi.usb_to_din", 32, GLOBAL, lambda d: 1 if d.din and d.din.mode == DIN_MODES.index("usb_merge") else 0),
    ("preset.count", 33, GLOBAL, lambda d: 1 if d.n_presets > 1 else 0),
    # layers (core/README.md "Button layers", "Cycles"): global, so they count once in the budget
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
    ("preset.bank", 41, PRESET, lambda d: 1 if d.n_presets > 1 and "program" in getattr(d, "msg", ()) else 0),
]
# STM32F1 DMA1 (RM0008 table 78): every DMA request is wired to one channel. allocate_dma() hands
# them out: the YAML's DMA users (hardware scans, maudio_pad_leds, din_midi dma:) may not share one.
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
               "pad.threshold", "pad.release", "pad.full", "pad.retrigger", "preset.bank"}
# ... and these on a device with NRPN / RPN (GM_MSG_NRPN: their numbers go up to 16383)
NRPN_WIDE_PARAMS = {"analog.cc", "encoder.cc", "layer.encoder.cc"}
PARAM_ID = {n: i for n, i, _s, _c in PARAMS}
PARAM_NAME = {i: n for n, i, _s, _c in PARAMS}


def stored_counts(dev):
    """(per-preset values, global values) the device stores at most: the record sizes of §10.5."""
    out = {PRESET: 0, GLOBAL: 0, VOLATILE: 0}
    for _n, _i, scope, count in PARAMS:
        out[scope] += count(dev)
    return out[PRESET], out[GLOBAL]


# The per-preset globals a preset may set (§10.2): YAML section -> key -> (param, default, kind, lo, hi).
# Defaults and ranges as in core/src/settings.c. kind: "int", "ms" (a time period, whole ms) or the
# enum's YAML names in value order.
CURVES = ["normal", "soft", "hard", "fixed"]
# encoder.mode (gm_settings.h ENC_*), analog.mode (AN_MODE_*). From cc14 on they need a message kind
# (MODE_FEATURE), and the number key that gives encoder.cc / analog.cc follows the mode (MODE_NUMBER_KEY).
ENC_MODES = ["absolute", "relative_twos", "relative_offset", "relative_sign", "cc14", "nrpn", "nrpn14",
             "rpn", "rpn14"]
ANALOG_MODES = ["cc", "pitch_bend", "channel_pressure", "cc14", "nrpn", "nrpn14", "rpn", "rpn14", "sysex"]
MODE_FEATURE = {"cc14": "cc14", "nrpn": "nrpn", "nrpn14": "nrpn", "rpn": "nrpn", "rpn14": "nrpn", "sysex": "sysex"}
MODE_NUMBER_KEY = {"nrpn": "nrpn", "nrpn14": "nrpn", "rpn": "rpn", "rpn14": "rpn", "sysex": "sysex"}
WIDE_MODES = {"cc14", "nrpn14", "rpn14"}       # 14-bit values (a SysEx string with msb / lsb too)
PRESET_GLOBALS = {
    "midi": {"channel": ("midi.channel", 1, "int", 1, 16),
             "transpose": ("midi.transpose", 0, "int", -24, 24),
             "octave": ("midi.octave", 0, "int", -4, 4)},
    "velocity": {"curve": ("velocity.curve", 0, CURVES, 0, 3),
                 "fixed": ("velocity.fixed", 100, "int", 1, 127)},
    "bend": {"glide": ("bend.glide", 80, "ms", 0, 2000)},
    "ramp": {"time": ("ramp.time", 300, "ms", 0, 5000)},
    # default: the device's own pad_settings: (a function of the device)
    "pad_settings": {"channel": ("pad.channel", lambda d: d.pad_set["channel"], "int", 0, 16),
                     "curve": ("pad.curve", lambda d: d.pad_set["curve"], CURVES, 0, 3),
                     "aftertouch": ("pad.aftertouch", lambda d: d.pad_set["aftertouch"], "bool", 0, 1),
                     "octave": ("pad.octave", 0, "int", -4, 4)},
}
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
PRESET_COMPONENTS = {   # section -> (component kind, plural, allowed keys, refused keys)
    "buttons": ("button", "buttons", ["action", "cc", "note", "preset", "mcu", "program", "mmc", "sysex"],
                ["id", "name", "pin", "led", "daw", "channel", "min", "max", "bank"]),
    "analog": ("analog input", "analog inputs", ["cc", "mode", "nrpn", "rpn", "sysex"],
               ["id", "name", "pin", "adc_min", "adc_max", "takeover", "mcu", "daw", "channel", "min", "max"]),
    "encoders": ("encoder", "encoders", ["cc", "mode", "nrpn", "rpn"],
                 ["id", "name", "pin_a", "pin_b", "steps_per_detent", "mcu", "ring", "ring_center", "daw",
                  "acceleration", "channel", "min", "max"]),
    "pads": ("pad", "pads", ["note"], ["id", "name", "pin"]),
}
PRESET_KEYS = ["name", "program", "bank", *PRESET_GLOBALS, *PRESET_COMPONENTS]
PRESETS_KEYS = ["slots", "revision", "list"]
MAX_PRESETS = 64
PRESET_NAME_LEN = 16           # config_store.h CS_NAME_LEN
PRESET_REVISION_MAX = 0x3FFF

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
# LED owners (gm_device.h GM_OWNER_*): who drives an LED that both the host and the device want.
LED_OWNERS = {"host_then_local": 0, "host": 1, "local": 2}
LED_SOURCES = {"note": 1, "cc": 2, "clock": 3, "transport": 4, "mcu": 5}   # gm_device.h GM_FB_*
LED_SOURCE_VALUES = {"clock": ["beat"], "transport": ["playing"]}
TAKEOVER = ["none", "pickup"]
TOUCH = {"none": 0, "movement": 2}  # analog touch: -> gm_device.h GM_TOUCH_* (1 = a sensor, reserved)
DEFAULT_LED_TIMEOUT_MS = 2000
TIMEOUT_NEVER = 0xFFFF            # gm_device.h GM_TIMEOUT_NEVER (10 ms units otherwise)

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
# { sn74hc595: leds, number: 3 }. ESPHome's pin key for the MCP23017 is mcp23xxx.
#
# Each hub type is a HubType: its YAML keys, what its pins can be, its validation and the C of its
# gm_hub_t entry. The tables below (HUB_ENUM, HUB_KEYS, OUT_HUBS, ...) are made from HUB_DEFS. To add
# a type: a subclass here, an instance in HUB_DEFS, and its driver in core/src/vpin.c (core/README.md
# "Adding a hub type").
class HubType:
    key = ""                    # the YAML list key, and the pin key ({ <key>: <hub id>, number: n })
    pin_key = None              # ... unless the pin key differs (mcp23017: mcp23xxx)
    aliases = ()                # older names, still taken as the list key and the pin key
    enum = ""                   # gm_device.h GM_HUB_*
    num = 0                     # ... its value
    keys = ("id",)              # a hub's YAML keys
    lines = {}                  # control-pin keys: the line kind, for allow_other_uses (same kind on each)
    roles = frozenset()         # what its pins can be: "in", "out", "analog" (and "row": decoder outputs)
    ram = False                 # its pins are in the hub RAM (GM_HUB_RAM): ram_bytes of it
    # Its LEDs show any colour (vpin.c: the type has an `rgb` op): an LED is colour_pins(hub) pins in a
    # row from LED * colour_pins (red, green, blue[, white]), and takes full-colour looks.
    full_colour = False
    pullup = False              # its pins have a pull-up (pin mode INPUT_PULLUP)
    schema = {}                 # key -> (min, max) where the JSON schema's shared range differs
    i2c = False                 # on the i2c: bus (address:)
    what = "pin"                # a pin's name in messages

    def validate(self, v, dev, hub, h, p, line, ctl):
        """Check hub h (the YAML mapping at path p) and fill in hub. ctl(key, role, ...) resolves a
        control pin."""

    def pin_name(self, s):
        """The pin number of a pin given by name (s upper-case, e.g. mcp23017 "GPA3"), or None."""
        return None

    def pin_hint(self, hub):
        return f"a number 0..{hub.n_pins - 1}"

    def range_text(self, hub):
        """Said after the type when a pin number is out of range, e.g. ' (2 chips x 8)'."""
        return ""

    def colour_pins(self, hub):
        """full_colour: the pins of one LED (3: red, green, blue; 4 with white)."""
        return 3

    def c_tables(self, hub):
        """C lines before the hub table: static arrays the entry points at."""
        return []

    def c_fields(self, hub):
        """The C lines of the entry's config: its gm_hub_t union member (core/include/gm_device.h), e.g.
        `.sr = { ... },`. The first line goes after .type."""
        raise NotImplementedError(self.key)


class ShiftRegister(HubType):
    """74HC595 (outputs) / 74HC165 (inputs): sr_count chips in a chain, 8 pins each."""
    lines = {"clock_pin": "clock", "latch_pin": "latch", "load_pin": "latch", "data_pin": "data", "oe_pin": "enable"}

    def __init__(self, key, enum, num, out):
        self.key, self.enum, self.num, self.out = key, enum, num, out
        self.keys = ("id", "data_pin", "clock_pin", "latch_pin", "oe_pin", "sr_count") if out else \
            ("id", "data_pin", "clock_pin", "load_pin", "sr_count")
        self.roles = frozenset({"out"} if out else {"in"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "sr_count", p, 1, 4, default=1)   # vpin.c: at most 4 chips (32 pins)
        hub.n_pins = 8 * hub.chain
        hub.data = ctl("data_pin", "out" if self.out else "in")
        hub.clock = ctl("clock_pin", "out")
        if self.out:
            hub.latch = ctl("latch_pin", "out")
            hub.enable = ctl("oe_pin", "out", required=False)
        else:
            hub.latch = ctl("load_pin", "out")

    def range_text(self, hub):
        return f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8)"

    def c_fields(self, hub):
        return [f"        .sr = {{ .data = {hub.data.c()}, .clock = {hub.clock.c()}, .latch = {hub.latch.c()},",
                f"                .oe = {hub.enable.c()}, .chain = {hub.chain} }},"]


class Mux(HubType):
    """CD74HC4051 (3 select lines, 8 channels) / 4067 (4, 16): one common pin routed to a channel;
    analog or digital inputs."""
    enum, num = "GM_HUB_MUX", 3
    lines = {"common_pin": "common", "enable_pin": "enable",
             "pin_s0": "s0", "pin_s1": "s1", "pin_s2": "s2", "pin_s3": "s3"}
    roles = frozenset({"in", "analog"})
    what = "channel"

    def __init__(self, key, n_sel):
        self.key, self.n_sel = key, n_sel
        self.keys = ("id", *(f"pin_s{i}" for i in range(n_sel)), "common_pin", "enable_pin", "delay")

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.n_sel = self.n_sel
        hub.sel = [ctl(f"pin_s{i}", "out") for i in range(hub.n_sel)]
        hub.n_pins = 1 << hub.n_sel
        hub.common = ctl("common_pin", "common")
        if hub.common.hub == 0:
            hub.common_adc = v.plat.adc(hub.common.num)[0]
        else:
            hub.common = None
        hub.enable = ctl("enable_pin", "out", required=False)
        hub.settle_us = v.period(h, "delay", p, 0, 0xFFFF, default=0, example="5us")   # 0: vpin.c waits 2us
        if "delay" in h and hub.settle_us == 0:
            v.warn(f"{p}.delay", h.line_of("delay"), "0us means the default, 2us (vpin.c)")

    def c_fields(self, hub):
        sel = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        return [f"        .mux = {{ .sel = {{ {', '.join(sel)} }},",
                f"                 .common = {(hub.common or NO_PIN).c()}, .enable = {hub.enable.c()}, "
                f".settle_us = {hub.settle_us}, .n_sel = {hub.n_sel} }},"]


class I2CExpander(HubType):
    """MCP23017 (16 GPIO) / PCF8574 (8 quasi-bidirectional) on the i2c: bus, read and written from
    the main loop."""
    keys = ("id", "address")
    roles = frozenset({"in", "out"})
    i2c = True

    def __init__(self, key, enum, num, n_pins, addrs, addr_text, pin_key=None):
        self.key, self.enum, self.num, self.n_pins = key, enum, num, n_pins
        self.addrs, self.addr_text, self.pin_key = addrs, addr_text, pin_key
        self.pullup = key == "mcp23017"

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x20)
        hub.n_pins = self.n_pins
        if hub.addr not in self.addrs:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for a {self.key} "
                                                         f"(usually {self.addr_text})")

    def c_fields(self, hub):
        return [f"        .i2c = {{ .addr = 0x{hub.addr:02X} }},"]

    def pin_name(self, s):
        if self.key == "mcp23017":
            m = re.fullmatch(r"(?:GP)?([AB])([0-7])", s)
            return (8 if m.group(1) == "B" else 0) + int(m.group(2)) if m else None
        m = re.fullmatch(r"P([0-7])", s)
        return int(m.group(1)) if m else None


class Decoder(HubType):
    """A 74HC138-style one-of-N decoder: outputs only, and only as matrix rows."""
    key, enum, num = "decoder", "GM_HUB_DECODER", 6
    keys = ("id", "address", "groups")
    roles = frozenset({"row"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        validate_decoder(v, hub, h, p, hub.index)

    def c_fields(self, hub):
        addr = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        en = [x.c() for x in hub.en] + ["GM_NO_PIN"] * (3 - len(hub.en))
        lv = [str(x) for x in hub.group_lv] + ["0"] * (4 - len(hub.group_lv))
        return [f"        .dec = {{ .addr = {{ {', '.join(addr)} }},",
                f"                 .en = {{ {', '.join(en)} }},",
                f"                 .n_addr = {hub.n_sel}, .n_en = {len(hub.en)}, .n_groups = {len(hub.group_lv)}, "
                f".group_lv = {{ {', '.join(lv)} }} }},"]


class SwitchMatrix(HubType):
    """A switch matrix scanned from the scan interrupt: inputs only, pin n = row n / columns,
    column n % columns."""
    key, enum, num = "switch_matrix", "GM_HUB_SWMATRIX", 8
    keys = ("id", "rows", "columns", "settle", "rows_per_scan")
    roles = frozenset({"in"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        # Rows may name a decoder declared further down: pins are resolved after all hubs
        # (resolve_switch_matrices). The size is known now, for pin numbers on this hub.
        rows, cols = h.get("rows"), h.get("columns")
        nr = rows.get("count", 0) if isinstance(rows, dict) else len(rows) if isinstance(rows, list) else 0
        nc = len(cols) if isinstance(cols, list) else 0
        hub.n_pins = max(nr if isinstance(nr, int) else 0, 0) * nc
        if hub.n_pins > 32:
            v.err(p, line, f"{nr} rows x {nc} columns = {hub.n_pins} switches: at most 32 per switch_matrix "
                           "(split it into two)")
        hub.settle_us = v.period(h, "settle", p, 0, 255, default=1, example="2us")
        # rows read per scan, in turn (0 = all): spreads a big panel over several scans
        hub.rows_per_scan = v.integer(h, "rows_per_scan", p, 1, 32, default=0)
        if hub.rows_per_scan and isinstance(nr, int) and nr and hub.rows_per_scan >= nr:
            v.warn(f"{p}.rows_per_scan", h.line_of("rows_per_scan"), f"{hub.rows_per_scan} >= its {nr} rows: "
                                                                      "every row is read on every scan anyway")

    def c_tables(self, hub):
        if not hub.rows or not hub.cols:
            return []
        c = []
        for nm, lst in (("rows", hub.rows), ("cols", hub.cols)):
            c.append(f"static const gm_pin_t hub{hub.index + 1}_{nm}[{len(lst)}] = {{")
            c += [f"    {x.c()}, /* {_comment(x.desc)} */" for x in lst]
            c += ["};"]
        return c

    def c_fields(self, hub):
        if not hub.rows or not hub.cols:
            return [f"        .sm = {{ .settle_us = {hub.settle_us} }},"]
        n = hub.index + 1
        return [f"        .sm = {{ .rows = hub{n}_rows, .cols = hub{n}_cols, .n_rows = {len(hub.rows)}, "
                f".n_cols = {len(hub.cols)},",
                f"                .settle_us = {hub.settle_us}, .rows_per_scan = {hub.rows_per_scan or 0} }},"]


# TM1628-class LED drivers (TM1628, TM1638, TM1620 ...): pin = display RAM bit, byte * 8 + bit.
TM_RAM_MAX = 16                 # TM1638 has 16 bytes, TM1628 14


class TM1628(HubType):
    """A TM1628-class LED driver, bit-banged from the main loop: pin = display RAM bit."""
    key, enum, num = "tm1628", "GM_HUB_TM1628", 7
    keys = ("id", "clk_pin", "dio_pin", "stb_pin", "ram_bytes", "intensity", "grids", "fade")
    # TM1628 lines only pair with each other: they're bit-banged from the main loop
    lines = {"clk_pin": "tm_clk", "dio_pin": "tm_dio", "stb_pin": "tm_stb"}
    roles = frozenset({"out"})
    ram = True

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.ram_bytes = v.integer(h, "ram_bytes", p, 1, TM_RAM_MAX, default=14)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = v.integer(h, "intensity", p, 0, 7, default=7)
        hub.grids = v.integer(h, "grids", p, 4, 7, default=0)   # 0: no display-mode command
        hub.fade = v.boolean(h, "fade", p, default=False)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        hub.latch = ctl("stb_pin", "out")

    def range_text(self, hub):
        return f" ({hub.ram_bytes} RAM bytes x 8; pin = byte * 8 + bit)"

    def c_fields(self, hub):
        return [f"        .tm = {{ .clk = {hub.clock.c()}, .dio = {hub.data.c()}, .stb = {hub.latch.c()},",
                f"                .ram_bytes = {hub.ram_bytes}, .brightness = {hub.intensity}, "
                f".grids = {hub.grids}, .fade = {1 if hub.fade else 0} }},"]


# The M-Audio Code pad LED board (maudio_pad_leds, formerly rgb_serial): RGB LEDs behind a serial
# link, its own protocol (vpin.c). pin = LED * 3 + colour (0 red, 1 green, 2 blue). Kept in the hub
# RAM, sent from the main loop.
MAUDIO_PADS_MAX = 32


class MaudioPadLeds(HubType):
    """The M-Audio Code pad LED board: RGB LEDs behind a serial link, pin = LED * 3 + colour."""
    key, enum, num = "maudio_pad_leds", "GM_HUB_MAUDIO_PADS", 9
    aliases = ("rgb_serial",)
    keys = ("id", "tx_pin", "rx_pin", "count", "dma", "gamma", "white")
    lines = {"tx_pin": "uart_tx", "rx_pin": "uart_rx"}
    roles = frozenset({"out"})
    ram = True
    full_colour = True

    def validate(self, v, dev, hub, h, p, line, ctl):
        if any(x.type == self.key for x in dev.hubs if x is not hub):
            # one LED board: its UART is fixed (USART2 on stm32f1)
            v.err(p, line, f"a second {self.key} hub: the firmware drives one (one LED board)")
        hub.chain = v.integer(h, "count", p, 1, MAUDIO_PADS_MAX, default=16)
        hub.n_pins = 3 * hub.chain
        hub.ram_bytes = hub.n_pins                       # a byte per colour
        hub.gamma, hub.white = validate_rgb_calibration(v, h, p)
        hub.data = ctl("tx_pin", "out")
        hub.latch = ctl("rx_pin", "in", required=False)     # optional: the board's answers (acks)
        hub.dma = v.boolean(h, "dma", p, default=True)
        if hub.data.hub == 0 and h.get("tx_pin") is not None:
            why = v.plat.serial_tx(hub.data.num)
            if why:
                v.err(f"{p}.tx_pin", h.line_of("tx_pin"), why)
            elif h.get("rx_pin") is not None and hub.latch is not NO_PIN:
                why = v.plat.serial_rx(hub.data.num, hub.latch.num)
                if why:
                    v.err(f"{p}.rx_pin", h.line_of("rx_pin"), why)

    def range_text(self, hub):
        return f" ({hub.chain} LEDs x 3; pin = LED * 3 + colour, 0 red 1 green 2 blue)"

    def c_tables(self, hub):
        if hub.gamma == 1:
            return []
        t = rgb_curve(hub.gamma)
        return [f"/* {_comment(hub.id)}: gamma {hub.gamma:g} ({self.key} gamma:) */",
                f"static const uint8_t rgb_curve_{hub.index + 1}[256] = {{",
                *[f"    {', '.join(str(x) for x in t[k:k + 16])}," for k in range(0, 256, 16)], "};"]

    def c_fields(self, hub):
        return [f"        .mpads = {{ .tx = {hub.data.c()}, .rx = {hub.latch.c()}, .count = {hub.chain}, "
                f".dma = {1 if hub.dma else 0},",
                f"                 .white = {{ {', '.join(str(x) for x in hub.white)} }}, "
                f".curve = {f'rgb_curve_{hub.index + 1}' if hub.gamma != 1 else '0'} }},"]


# Addressable LED chains (WS2812B, SK6812 RGB / RGBW) on one MCU pin: pin = LED * channels + colour (0 red,
# 1 green, 2 blue, 3 white). A byte per colour in the hub RAM; the platform sends frames (hal_ledstrip_*).
WS2812_MAX = 256
WS2812_ORDERS = sorted({"".join(p) for p in itertools.permutations("RGB")}) + \
    sorted({"".join(p) for p in itertools.permutations("RGBW")})


class WS2812(HubType):
    """An addressable LED strip (WS2812B / SK6812, RGB or RGBW): pin = LED * 3 (4) + colour."""
    key, enum, num = "ws2812", "GM_HUB_WS2812", 11
    keys = ("id", "pin", "count", "order", "gamma", "white")
    lines = {"pin": "ledstrip"}
    roles = frozenset({"out"})
    ram = True
    full_colour = True

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "count", p, 1, WS2812_MAX, required=True) or 1
        order = h.get("order", "GRB")
        order = order.strip().upper() if isinstance(order, str) else order
        if order not in WS2812_ORDERS:
            close = difflib.get_close_matches(str(order).upper(), WS2812_ORDERS, 1)
            hint = f"did you mean {close[0]}? " if close else ""
            v.err(f"{p}.order", h.line_of("order"), f"unknown order '{h.raw_of('order')}' ({hint}the colours as the "
                                                    "LED takes them: GRB (WS2812B, the default), RGB, BRG, ...; "
                                                    "GRBW / RGBW for RGBW LEDs, e.g. SK6812 RGBW)")
            order = "GRB"
        hub.order = order
        hub.channels = len(order)
        hub.n_pins = hub.channels * hub.chain
        hub.ram_bytes = hub.n_pins                       # a byte per colour
        hub.gamma, hub.white = validate_rgb_calibration(v, h, p, white_n=hub.channels)
        hub.data = ctl("pin", "out")
        if hub.data.hub == 0 and h.get("pin") is not None:
            why = v.plat.ledstrip(hub.data.num, [x.data.num for x in dev.hubs if x.type == self.key and
                                                 x is not hub and x.data.hub == 0])
            if why:
                v.err(f"{p}.pin", h.line_of("pin"), why)

    def colour_pins(self, hub):
        return hub.channels

    def range_text(self, hub):
        names = " ".join(f"{k} {c}" for k, c in enumerate(("red", "green", "blue", "white")[:hub.channels]))
        return f" ({hub.chain} LEDs x {hub.channels}; pin = LED * {hub.channels} + colour, {names})"

    def wire_order(self, hub):
        """gm_hub_ws2812_t.order: byte k of an LED is colour (order >> 2k) & 3."""
        return sum("RGBW".index(ch) << (2 * k) for k, ch in enumerate(hub.order))

    def c_tables(self, hub):
        if hub.gamma == 1:
            return []
        t = rgb_curve(hub.gamma)
        return [f"/* {_comment(hub.id)}: gamma {hub.gamma:g} ({self.key} gamma:) */",
                f"static const uint8_t rgb_curve_{hub.index + 1}[256] = {{",
                *[f"    {', '.join(str(x) for x in t[k:k + 16])}," for k in range(0, 256, 16)], "};"]

    def c_fields(self, hub):
        white = list(hub.white) + [255] * (4 - len(hub.white))
        return [f"        .ws = {{ .pin = {hub.data.c()}, .count = {hub.chain}, .channels = {hub.channels}, "
                f".order = 0x{self.wire_order(hub):02X} /* {hub.order} */,",
                f"                .white = {{ {', '.join(str(x) for x in white)} }}, "
                f".curve = {f'rgb_curve_{hub.index + 1}' if hub.gamma != 1 else '0'} }},"]


# A 4-wire resistive XY pad: two analog channels, 0 = x, 1 = y (vpin.c reads it in software).
XY_TOUCH = (570, 3430)          # stock Code 61: 0x250 - 22 .. 0xD50 + 22


class XYPad(HubType):
    key, enum, num = "xy_pad", "GM_HUB_XY", 10
    keys = ("id", "x_plus_pin", "x_minus_pin", "y_plus_pin", "y_minus_pin", "touch_min", "touch_max")
    lines = {"x_plus_pin": "xy_xp", "x_minus_pin": "xy_xm", "y_plus_pin": "xy_yp", "y_minus_pin": "xy_ym"}
    roles = frozenset({"analog"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        validate_xy_pad(v, hub, h, p, ctl)

    def pin_hint(self, hub):
        return "x or y"

    def c_fields(self, hub):
        drive = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        return [f"        .xy = {{ .drive = {{ {', '.join(drive)} }},",
                f"                .sense = {{ {hub.data.c()}, {hub.clock.c()} }}, "
                f".touch_min = {hub.touch_min}, .touch_max = {hub.touch_max} }},"]


# MAX7219 / MAX7221 LED drivers in a chain: pin = chip * 64 + digit * 8 + segment (0..6 = A..G,
# 7 = DP), so a 7-segment character is its digit's 8 pins (displays: type segment7).
MAX7219_CHIPS = 8


class MAX7219(HubType):
    """A chain of MAX7219 LED drivers (ESPHome max7219 keys), bit-banged from the main loop."""
    key, enum, num = "max7219", "GM_HUB_MAX7219", 12
    keys = ("id", "clk_pin", "din_pin", "cs_pin", "num_chips", "intensity", "scan_limit")
    # chains may share CLK and DIN, each with its own CS
    lines = {"clk_pin": "mx_clk", "din_pin": "mx_din", "cs_pin": "mx_cs"}
    roles = frozenset({"out"})
    ram = True
    schema = {"num_chips": (1, MAX7219_CHIPS), "intensity": (0, 15), "scan_limit": (1, 8)}

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "num_chips", p, 1, MAX7219_CHIPS, default=1)
        hub.n_pins = 64 * hub.chain
        hub.ram_bytes = 8 * hub.chain                    # a digit register per byte
        hub.intensity = v.integer(h, "intensity", p, 0, 15, default=15)
        hub.grids = v.integer(h, "scan_limit", p, 1, 8, default=8)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("din_pin", "out")
        hub.latch = ctl("cs_pin", "out")

    def range_text(self, hub):
        return (f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8 digits x 8; pin = chip * 64 + "
                "digit * 8 + segment, 0..6 = A..G, 7 = DP)")

    def c_fields(self, hub):
        return [f"        .mx = {{ .clk = {hub.clock.c()}, .din = {hub.data.c()}, .cs = {hub.latch.c()},",
                f"                .chips = {hub.chain}, .intensity = {hub.intensity}, "
                f".scan_limit = {hub.grids} }},"]


class TM1637(HubType):
    """A TM1637 LED driver (2-wire, ESPHome tm1637 keys), bit-banged from the main loop: pin = grid * 8
    + segment (bit 0 = SEG1 = a ... bit 7 = SEG8)."""
    key, enum, num = "tm1637", "GM_HUB_TM1637", 13
    keys = ("id", "clk_pin", "dio_pin", "intensity", "length")
    lines = {"clk_pin": "tm37_clk", "dio_pin": "tm37_dio"}
    roles = frozenset({"out"})
    ram = True
    schema = {"length": (1, 6), "intensity": (0, 7)}

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.ram_bytes = v.integer(h, "length", p, 1, 6, default=6)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = v.integer(h, "intensity", p, 0, 7, default=7)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        for k in ("clk_pin", "dio_pin"):
            x = h.get(k)
            if isinstance(x, dict) and x.get("allow_other_uses"):
                v.err(f"{p}.{k}", h.line_of(k), "a tm1637 has no chip select: its clk_pin and dio_pin "
                                               "can't be shared")

    def range_text(self, hub):
        return f" ({hub.ram_bytes} grids x 8; pin = grid * 8 + segment)"

    def c_fields(self, hub):
        return [f"        .tm37 = {{ .clk = {hub.clock.c()}, .dio = {hub.data.c()}, .grids = {hub.ram_bytes}, "
                f".brightness = {hub.intensity} }},"]


class HT16K33(HubType):
    """An HT16K33 LED driver on the I2C bus (the 14- and 7-segment backpacks), written from the main
    loop: pin = COM * 16 + ROW. ESP only for now, as the other I2C hubs."""
    key, enum, num = "ht16k33", "GM_HUB_HT16K33", 14
    keys = ("id", "address", "intensity")
    roles = frozenset({"out"})
    ram = True
    i2c = True
    schema = {"intensity": (0, 15)}

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x70)
        if not 0x70 <= hub.addr <= 0x77:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for an ht16k33 "
                                                         "(0x70-0x77)")
        hub.ram_bytes = 16
        hub.n_pins = 128
        hub.intensity = v.integer(h, "intensity", p, 0, 15, default=15)

    def range_text(self, hub):
        return " (16 RAM bytes x 8; pin = COM * 16 + ROW)"

    def c_fields(self, hub):
        return [f"        .ht = {{ .addr = 0x{hub.addr:02X}, .brightness = {hub.intensity} }},"]


# SSD1306 / SH1106 OLEDs on the I2C bus: a framebuffer (width x height / 8 bytes) in the hub RAM, no
# pins; text displays (displays: type text) draw on it.
OLED_MODELS = {"ssd1306": 0, "sh1106": 1}       # gm_device.h GM_OLED_SH1106
OLED_SIZES = {"ssd1306": [(128, 64), (128, 32), (96, 16), (64, 48), (64, 32)], "sh1106": [(128, 64)]}
OLED_8BIT = {0x78: 0x3C, 0x7A: 0x3D}           # the 8-bit write addresses printed on many modules


class SSD1306(HubType):
    """An SSD1306 or SH1106 OLED on the I2C bus, written from the main loop a run of changed bytes at a
    time. No pins: text displays (displays: type text, ssd1306: <id>) draw on it. ESP only for now, as
    the other I2C hubs."""
    key, enum, num = "ssd1306", "GM_HUB_SSD1306", 15
    keys = ("id", "address", "model", "width", "height", "contrast", "rotate")
    roles = frozenset()
    ram = True
    i2c = True
    schema = {"width": (64, 128), "height": (16, 64), "contrast": (0, 255)}

    def validate(self, v, dev, hub, h, p, line, ctl):
        model = h.get("model", "ssd1306")
        model = model.lower() if isinstance(model, str) else model
        if model not in OLED_MODELS:
            v.err(f"{p}.model", h.line_of("model"), f"unknown model '{h.raw_of('model')}' "
                                                    f"(use {', '.join(OLED_MODELS)})")
            model = "ssd1306"
        hub.model = model
        addr = v.integer(h, "address", p, 0x08, 0x7A, default=0x3C)
        if addr in OLED_8BIT:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{addr:02X} is the 8-bit form (as printed on many "
                                                         f"boards) of I2C address 0x{OLED_8BIT[addr]:02X}: using "
                                                         f"0x{OLED_8BIT[addr]:02X} (write that to drop this warning)")
            addr = OLED_8BIT[addr]
        elif addr > 0x77:
            v.err(f"{p}.address", h.line_of("address"), f"0x{addr:02X} is reserved on I2C (an OLED is 0x3C or "
                                                        "0x3D; 0x78 / 0x7A are those as 8-bit addresses)")
        elif addr not in (0x3C, 0x3D):
            v.warn(f"{p}.address", h.line_of("address"), f"0x{addr:02X} is unusual for an OLED (0x3C or 0x3D)")
        hub.addr = addr
        sizes = OLED_SIZES[model]
        hub.width = v.integer(h, "width", p, 64, 128, default=sizes[0][0])
        hub.height = v.integer(h, "height", p, 16, 64, default=sizes[0][1])
        if (hub.width, hub.height) not in sizes:
            v.err(p, h.line_of("height") or h.line_of("width") or line,
                  f"{hub.width}x{hub.height}: a {model} is " + ", ".join(f"{w}x{ht}" for w, ht in sizes))
            hub.width, hub.height = sizes[0]
        hub.intensity = v.integer(h, "contrast", p, 0, 255, default=0xCF)
        rot = h.get("rotate", 0)
        if rot not in (0, 180):
            v.err(f"{p}.rotate", h.line_of("rotate"), f"rotate: {h.raw_of('rotate')}: 0 or 180 (degrees)")
            rot = 0
        hub.rotate = rot
        hub.ram_bytes = hub.width * hub.height // 8
        hub.n_pins = 0

    def range_text(self, hub):
        return " (no pins: text displays draw on it, displays: type text)"

    def c_fields(self, hub):
        flags = " | ".join(f for f, on in (("GM_OLED_SH1106", hub.model == "sh1106"),
                                          ("GM_OLED_ROTATE", hub.rotate == 180)) if on) or "0"
        return [f"        .oled = {{ .addr = 0x{hub.addr:02X}, .width = {hub.width}, .height = {hub.height}, "
                f".contrast = {hub.intensity}, .flags = {flags} }},"]


HUB_DEFS = {t.key: t for t in (
    ShiftRegister("sn74hc595", "GM_HUB_HC595", 1, out=True),
    ShiftRegister("sn74hc165", "GM_HUB_HC165", 2, out=False),
    Mux("cd74hc4051", 3), Mux("cd74hc4067", 4),
    I2CExpander("mcp23017", "GM_HUB_MCP23017", 4, 16, range(0x20, 0x28), "0x20-0x27", pin_key="mcp23xxx"),
    I2CExpander("pcf8574", "GM_HUB_PCF8574", 5, 8, list(range(0x20, 0x28)) + list(range(0x38, 0x40)),
                "0x20-0x27, PCF8574A 0x38-0x3F"),
    Decoder(), SwitchMatrix(), TM1628(), MaudioPadLeds(), XYPad(), WS2812(), MAX7219(), TM1637(), HT16K33(), SSD1306())}
HUB_ENUM = {k: t.enum for k, t in HUB_DEFS.items()}
HUB_TYPES = list(HUB_DEFS)
# older names of a type (rgb_serial: maudio_pad_leds), as a list key and as a pin key
HUB_ALIAS = {a: k for k, t in HUB_DEFS.items() for a in t.aliases}
HUB_PIN_KEY = {k: t.pin_key or k for k, t in HUB_DEFS.items()}
PIN_KEY_HUB = {**{k: t for t, k in HUB_PIN_KEY.items()}, **HUB_ALIAS}
HUB_KEYS = {k: list(t.keys) for k, t in HUB_DEFS.items()}
I2C_HUBS = {k for k, t in HUB_DEFS.items() if t.i2c}
SHIFT_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, ShiftRegister)}
MUX_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, Mux)}
MUX_SELECT = {k: HUB_DEFS[k].n_sel for k in MUX_HUBS}
XY_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, XYPad)}
TM_HUBS = {k for k, t in HUB_DEFS.items() if isinstance(t, TM1628)}
RAM_HUBS = {k for k, t in HUB_DEFS.items() if t.ram}
FULL_COLOUR_HUBS = [k for k, t in HUB_DEFS.items() if t.full_colour]
OUT_HUBS = {k for k, t in HUB_DEFS.items() if "out" in t.roles}
IN_HUBS = {k for k, t in HUB_DEFS.items() if "in" in t.roles}
ANALOG_HUBS = {k for k, t in HUB_DEFS.items() if "analog" in t.roles}
# A decoder's outputs are one-of-N selects: only matrix rows (key_matrix, switch_matrix).
ROW_HUBS = {k for k, t in HUB_DEFS.items() if "row" in t.roles}

TOP_KEYS = ["gemifi", "name", "friendly_name", "description", "project", "board_id", "platform", "chip", "board",
            "usb", "midi", "i2c", "scan_interval", "hardware_scan", "boot_combo", "factory_reset_combo", "unlock",
            "status_led", "feedback", "mcu", *HUB_TYPES, *HUB_ALIAS,
            "buttons", "led_defaults", "leds", "analog", "encoders", "encoder_settings", "key_matrix", "pads",
            "pad_settings", "presets", "modes", "displays", "din_midi", "write_to_flash", "midi_messages", "layout"]
PIN_KEYS = ["number", "inverted", "mode", "allow_other_uses", *PIN_KEY_HUB]
# The keys of each section (the v.mapping() calls below). gemifi_schema.py builds the JSON schema from
# these lists, so a new key goes here.
MIDI_KEYS = ["transport"]
USB_KEYS = ["vid", "pid", "daw_port"]
I2C_KEYS = ["sda", "scl"]
PROJECT_KEYS = ["version"]
FEEDBACK_KEYS = ["enabled", "script", "led_owner", "led_timeout"]
MCU_KEYS = ["extender"]
DIN_KEYS = ["tx_pin", "rx_pin", "mode", "port_name", "thru", "controller_out", "usb_to_din", "dma"]
HW_SCAN_KEYS = ["matrix", "analog"]
HW_SCAN_MATRIX_KEYS = ["row_time", "settle", "precharge", "timer"]
HW_SCAN_ANALOG_KEYS = ["timer"]
COMBO_KEYS = ["combo", "hold"]        # factory_reset_combo: / unlock: as a mapping
LED_DEFAULTS_KEYS = ["idle", "active"]
LED_KEYS = ["id", "name", "pin", "pin2", "pin3", "idle", "active", "feedback", "owner", "timeout", "daw"]
BUTTON_KEYS = ["id", "name", "pin", "action", "cc", "note", "preset", "led", "mcu", "daw", "program", "mmc", "sysex",
               "bank", "channel", "min", "max"]
ANALOG_KEYS = ["id", "name", "pin", "cc", "mode", "adc_min", "adc_max", "takeover", "touch", "mcu", "daw", "nrpn",
               "rpn", "sysex", "channel", "min", "max"]
ENCODER_KEYS = ["id", "name", "pin_a", "pin_b", "cc", "steps_per_detent", "mcu", "ring", "ring_center", "daw",
                "acceleration", "mode", "nrpn", "rpn", "channel", "min", "max"]
MATRIX_KEYS = ["type", "rows", "columns", "contacts", "key_map", "keys", "first_note", "settle", "velocity", "daw"]
VELOCITY_KEYS = ["fast", "slow"]
DECODER_ROWS_KEYS = ["decoder", "first", "count"]
NODE_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?")

# ESPHome pin modes (esphome.io "Pin Schema"): a string shorthand or a mapping of flags.
MODE_FLAGS = ["input", "output", "pullup", "pulldown", "open_drain"]
MODE_STRINGS = {"INPUT": {"input"}, "OUTPUT": {"output"}, "OUTPUT_OPEN_DRAIN": {"output", "open_drain"},
                "ANALOG": {"analog"}, "INPUT_PULLUP": {"input", "pullup"},
                "INPUT_PULLDOWN": {"input", "pulldown"},
                "INPUT_OUTPUT_OPEN_DRAIN": {"input", "output", "open_drain"}}

# Format 1 keys: an old file gets one error per old key, at its line, saying what replaced it.
RENAMED_ID = {"label": "renamed: use `id:` (lower_snake_case, e.g. `oct_down`, used for references) "
                       "and/or `name:` (display text)"}
RENAMED_TOP = {
    "hubs": "replaced: declare each hub under its chip's key, ESPHome style, e.g. "
            "`sn74hc595: [{ id: leds, data_pin: ..., clock_pin: ..., latch_pin: ... }]` "
            "(chips: " + ", ".join(HUB_TYPES) + ")",
    "scan_us": "renamed: use `scan_interval:` with a unit, e.g. `scan_interval: 125us`",
    "version": "moved: use `project: { version: 1.0.0 }` (as ESPHome's `project:`)",
}
RENAMED_PIN = {
    "pin": "renamed: use `number:` (e.g. `{ number: PA1, inverted: true }`)",
    "channel": "renamed: use `number:` (e.g. `{ cd74hc4051: faders, number: 0 }`)",
    "hub": "replaced: name the hub by its chip, e.g. `{ sn74hc595: leds, number: 3 }` "
           "(mcp23017 hubs: `mcp23xxx:`)",
    "pullup": "replaced: use `mode: INPUT_PULLUP` (or `mode: { input: true, pullup: true }`)",
    "pulldown": "replaced: use `mode: INPUT_PULLDOWN` (or `mode: { input: true, pulldown: true }`)",
    "mcp23017": "the pin key for an MCP23017 hub is `mcp23xxx:` (as in ESPHome)",
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
RENAMED_BUTTON = dict(RENAMED_ID, number="renamed: use `cc:` (cc_momentary / cc_toggle / cc_ramp) or "
                                         "`note:` (action note)")
RENAMED_ANALOG = dict(RENAMED_ID, raw_min="renamed: use `adc_min:`", raw_max="renamed: use `adc_max:`")
RENAMED_ENCODER = dict(RENAMED_ID, a="renamed: use `pin_a:` (as ESPHome's rotary_encoder)",
                       b="renamed: use `pin_b:` (as ESPHome's rotary_encoder)")
RENAMED_MATRIX = {"kind": "renamed: use `type:` (switch | velocity)", "cols": "renamed: use `columns:`",
                  "settle_us": "renamed: use `settle:` with a unit, e.g. `settle: 1us`"}
RENAMED_VELOCITY = {"fast_ms": "renamed: use `fast:` with a unit, e.g. `fast: 1.6ms`",
                    "slow_ms": "renamed: use `slow:` with a unit, e.g. `slow: 120ms`"}

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

INVERT, PULLUP, PULLDOWN, OPEN_DRAIN = 1, 2, 4, 8
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


def errors(diags):
    return [d for d in diags if d.level == "error"]


# ---------------------------------------------------------------------------------------------
# YAML with line numbers

class LDict(dict):
    """dict that remembers the line of itself and of each key (1-based)."""
    line = 0

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.key_lines = {}
        self.raw = {}           # key -> the value's source text, for scalars

    def line_of(self, key):
        return self.key_lines.get(key, self.line)

    def raw_of(self, key):
        """The value as written in the file (so a `no` read as False can be shown as `no`)."""
        return self.raw.get(key, str(self.get(key)))


class LList(list):
    line = 0

    def __init__(self, *a):
        super().__init__(*a)
        self.item_lines = []
        self.item_raw = []

    def line_of(self, i):
        return self.item_lines[i] if i < len(self.item_lines) else self.line

    def raw_of(self, i):
        r = self.item_raw[i] if i < len(self.item_raw) else None
        return str(self[i]) if r is None else r


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
        """-> None if num can be an maudio_pad_leds tx_pin, else why not."""
        return f"maudio_pad_leds isn't supported on {self.name} yet"

    def serial_rx(self, tx, rx):
        """-> None if rx can be the maudio_pad_leds rx_pin that goes with tx_pin tx, else why not."""
        return f"maudio_pad_leds rx_pin isn't supported on {self.name} yet"

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

    def adc(self, num):
        return num in self.ADC, None

    def serial_tx(self, num):
        """-> None if num can be an maudio_pad_leds tx_pin, else why not."""
        return None if num == 2 else "only PA2 (USART2 TX) is supported as an maudio_pad_leds tx_pin on stm32f1"

    def serial_rx(self, tx, rx):
        return None if tx == 2 and rx == 3 else "the maudio_pad_leds rx_pin on stm32f1 is PA3 (USART2 RX, with tx_pin PA2)"

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

class Pin:
    """A resolved gm_pin_t: hub 0 = MCU, n = hubs[n-1]; GM_HUB_NONE (0xFF) = no pin."""

    def __init__(self, hub, flags, num, desc):
        self.hub, self.flags, self.num, self.desc = hub, flags, num, desc

    def c(self):
        if self.hub == 0xFF:
            return "GM_NO_PIN"
        fl = [n for b, n in ((INVERT, "GM_PIN_INVERT"), (PULLUP, "GM_PIN_PULLUP"),
                             (PULLDOWN, "GM_PIN_PULLDOWN"), (OPEN_DRAIN, "GM_PIN_OPEN_DRAIN")) if self.flags & b]
        return f"{{ {self.hub}, {' | '.join(fl) or '0'}, {self.num} }}"


NO_PIN = Pin(0xFF, 0, 0, "-")


class Hub:
    def __init__(self, **kw):
        self.__dict__.update(kw)


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
        self.matrix = None
        self.version = None
        self.boot_combo = []
        # safety (core/README.md "Factory-reset combo", "Physical unlock", "Status LED")
        self.reset_combo, self.reset_hold_ms = [], RESET_HOLD_DEFAULT_MS
        self.unlock, self.unlock_hold_ms = [], UNLOCK_HOLD_DEFAULT_MS   # [] = unlock off
        self.status_led = None   # LED index
        self.i2c = None
        self.feedback = True
        self.led_owner, self.led_timeout = 0, DEFAULT_LED_TIMEOUT_MS // 10
        self.host_fb = True          # feedback.script: GET_COMPONENT and FB_* (sysex.md 4.6)
        self.mcu = None              # Hub(extender=bool, cable=0|1) when mcu: is given
        self.daw_port = False
        self.din = None              # Hub(tx, rx, mode, port_name, thru, out, usb) for din_midi:
        # presets (research/core/presets-design.md): slot n = presets[n - 1], Hub(name, program, vals)
        self.n_presets = 1
        self.slots_invalid = False   # preset_slots: presets.slots is given but invalid
        self.presets_given = False   # the YAML has presets:
        self.presets_revision = 0
        self.presets = []
        self.modes = []              # Hub per mode (validate_modes), in YAML order; a cycle's steps in a row
        self.n_cycles = 0            # modes: cycle: groups
        self.pads = []               # Hub per pad (validate_pads), in YAML order
        self.pad_twins = []          # Hub per pad layer twin (mode_layer): GET_COMPONENT pads after dev.pads
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
                     f"id '{v}' of {how(path, derived)} is already the id of {how(p0, d0)} (line {l0}, {a_(k0)}); "
                     "ids are unique across the device: set a different id:")
            return None
        self.ids[v] = (kind, path, line, index, derived)
        return v

    def display_name(self, d, path, ident):
        """name: (free text, optional), else derived from the id: oct_down -> "Oct down"."""
        name = self.string(d, "name", path)
        return name if name is not None else humanize(ident or "")

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
            self.err(path, line, f"'{raw}' is the id of {target[1]} ({a_(target[0])}), not of {a_(kind)}")
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
            keys = [k for k in PIN_KEY_HUB if k in v]
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
                                         f"({adc_text(self.plat)}; or use a mux hub channel)")
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
        if PIN_KEY_HUB[hub_key] != hub.type:
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
                                 f"channels can't be analog ({adc_text(self.plat)})")
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


def validate_hw_scan(v, dev, data, plat):
    if "hardware_scan" not in data:
        return
    line = data.line_of("hardware_scan")
    hs = v.mapping(data["hardware_scan"], "hardware_scan", line, HW_SCAN_KEYS)
    if hs is None:
        return
    if plat.name not in HW_SCAN_PLATFORMS:
        v.err("hardware_scan", line, f"not available on {plat.name} (only on: "
                                     f"{', '.join(sorted(HW_SCAN_PLATFORMS))})")
        return
    if "analog" in hs:
        a = hs["analog"]
        if isinstance(a, dict):         # `analog: { timer: tim3 }`
            if v.mapping(a, "hardware_scan.analog", hs.line_of("analog"), HW_SCAN_ANALOG_KEYS) is not None:
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
    mx = v.mapping(hs["matrix"], path, mline, HW_SCAN_MATRIX_KEYS)
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
    row = v.period(mx, "row_time", path, 0.5, 50, step_us=0.001, example="3.5us")
    settle = v.period(mx, "settle", path, 0.25, 49.5, step_us=0.001, example="3us")
    pre = v.boolean(mx, "precharge", path)
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


def type_name(v):
    return {dict: "mapping", LDict: "mapping", list: "list", LList: "list", str: "string", int: "integer",
            bool: "boolean", float: "number", type(None): "nothing"}.get(type(v), type(v).__name__)


def adc_text(plat):
    if plat.name == "nuvoton-nuc1xx":
        return "ADC pins: PA0-PA7"
    if plat.name == "stm32f1":
        return "ADC pins: PA0-PA7, PB0-PB1, PC0-PC5"
    return f"ADC pins: {_ranges(plat.adc1 | plat.adc2, lambda n: f'GPIO{n}')}"


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
            "hub": "faders", "pad": "pad_1"}.get(kind, "item")


def a_(kind):
    """'an LED', 'an analog input', 'a button'."""
    return f"{'an' if kind[:1] in 'aeiouAEIOUL' else 'a'} {kind}"


def humanize(ident):
    """The default display name of an id: oct_down -> "Oct down", fader_1 -> "Fader 1"."""
    s = " ".join(ident.replace("_", " ").split())
    return s[:1].upper() + s[1:]


# ---------------------------------------------------------------------------------------------

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
    dev.board_id = v.integer(data, "board_id", "", 0, 0x3FFF, required=True) or 0
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
    if "midi" in data and v.mapping(midi, "midi", data.line_of("midi"), MIDI_KEYS) is None:
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
        usb = v.mapping(data["usb"], "usb", data.line_of("usb"), USB_KEYS)
        if usb is not None:
            dev.usb = (v.integer(usb, "vid", "usb", 0, 0xFFFF, required=True) or 0,
                       v.integer(usb, "pid", "usb", 0, 0xFFFF, required=True) or 0)
            if "daw_port" in usb:
                daw_port = (v.boolean(usb, "daw_port", "usb"), usb.line_of("daw_port"))
            if tr != "usb":
                v.warn("usb", data.line_of("usb"), f"ignored: midi.transport is {tr}")
    elif tr == "usb":
        dev.usb = DEFAULT_USB
        v.warn("usb", data.line, "not given: using the pid.codes test IDs 1209:0002 (development only)")
    validate_feedback(v, dev, data)
    validate_mcu(v, dev, data, tr, daw_port)
    validate_din(v, dev, data, tr, plat)

    dev.scan_us = v.period(data, "scan_interval", "", 1, 0xFFFF, default=125)
    validate_hw_scan(v, dev, data, plat)

    # hubs (before anything that refers to them), in file order
    for key in data:
        if key in HUB_ENUM or key in HUB_ALIAS:
            lst = v.seq(data[key], key, data.line_of(key))
            for i, h in enumerate(lst):
                validate_hub(v, dev, h, HUB_ALIAS.get(key, key), f"{key}[{i}]", lst.line_of(i))
    if len(dev.hubs) > 254:
        v.err("", data.line, f"{len(dev.hubs)} hubs: at most 254")
    resolve_switch_matrices(v, dev)

    # i2c
    need_i2c = any(h.type in I2C_HUBS for h in dev.hubs)
    if need_i2c and not plat.i2c:
        for h in dev.hubs:
            if h.type in I2C_HUBS:
                v.err(h.path, h.line, f"{h.type} isn't supported on {plat.name} yet (no I2C driver: "
                                      "the platform's hal_i2c_* are stubs)")
        need_i2c = False        # don't also ask for the i2c: pins
    if "i2c" in data:
        i2c = v.mapping(data["i2c"], "i2c", data.line_of("i2c"), I2C_KEYS)
        if i2c is not None:
            sda = v.pin(i2c.get("sda"), "i2c.sda", i2c.line_of("sda"), "out", allow_hub=False)
            scl = v.pin(i2c.get("scl"), "i2c.scl", i2c.line_of("scl"), "out", allow_hub=False)
            if sda and scl:
                dev.i2c = (sda, scl)
            if not need_i2c:
                v.warn("i2c", data.line_of("i2c"), "no I2C hub (mcp23017 / pcf8574 / ht16k33 / ssd1306) uses it")
    elif need_i2c:
        first = next(h for h in dev.hubs if h.type in I2C_HUBS)
        v.err("i2c", data.line, f"hub '{first.id}' ({first.type}) needs the I2C pins: "
                                "add i2c: { sda: ..., scl: ... }")
    addrs = {}
    for h in dev.hubs:
        if h.type in I2C_HUBS:
            if h.addr in addrs:
                v.err(f"{h.path}.address", h.line, f"I2C address 0x{h.addr:02X} is already used by hub "
                                                   f"'{addrs[h.addr]}'")
            addrs[h.addr] = h.id
    stbs = {}
    for h in dev.hubs:
        if h.type in TM_HUBS and h.latch.hub == 0:
            if h.latch.num in stbs:
                v.err(f"{h.path}.stb_pin", h.line, f"{h.latch.desc} is already the stb_pin of hub "
                                                   f"'{stbs[h.latch.num]}': chips can share clk_pin and "
                                                   "dio_pin, but each needs its own stb_pin")
            stbs.setdefault(h.latch.num, h.id)
    css = {}
    for h in dev.hubs:
        if h.type == "max7219" and h.latch.hub == 0:
            if h.latch.num in css:
                v.err(f"{h.path}.cs_pin", h.line, f"{h.latch.desc} is already the cs_pin of hub '{css[h.latch.num]}': "
                                                  "chains can share clk_pin and din_pin, but each needs its own cs_pin")
            css.setdefault(h.latch.num, h.id)

    preset_slots(v, dev, data)
    validate_midi_messages(v, dev, data)
    validate_components(v, dev, data)
    validate_pads(v, dev, data)
    validate_displays(v, dev, data)

    if data.get("key_matrix") is not None:
        validate_matrix(v, dev, data["key_matrix"], data.line_of("key_matrix"))

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
    validate_modes(v, dev, data)
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
            dev.matrix or any(b.action_name == "note" for b in dev.buttons)):
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
        if h.type == "maudio_pad_leds" and h.dma:
            line = h.raw.line_of("dma") if "dma" in h.raw else h.line
            serial.append((f"maudio_pad_leds '{h.id}'", f"{h.path}.dma", line, ["USART2_TX"],
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


def validate_timeout(v, d, key, path, default):
    """An LED timeout: a time period (10 ms steps, 10ms..655s) or `never`. -> 10 ms units."""
    if key not in d or d[key] is None:
        return default
    if isinstance(d[key], str) and d[key].strip().lower() == "never":
        return TIMEOUT_NEVER
    t = v.period(d, key, path, 10_000, 655_340_000, step_us=10_000, example="2s")
    return default if t is None else t


def validate_owner(v, d, path):
    if d.get("owner") is None:
        return None
    o = d["owner"]
    if o not in LED_OWNERS:
        close = difflib.get_close_matches(str(o), list(LED_OWNERS), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{path}.owner", d.line_of("owner"), f"unknown owner '{d.raw_of('owner')}' ({hint}valid: "
                                                   f"{', '.join(LED_OWNERS)})")
        return None
    return LED_OWNERS[o]


def validate_feedback(v, dev, data):
    """feedback: { enabled, script, led_owner, led_timeout } (all optional): F1 host feedback on the main
    port, and (script) the DAW-script messages of sysex.md 4.6."""
    if data.get("feedback") is None:
        return
    fb = v.mapping(data["feedback"], "feedback", data.line_of("feedback"), FEEDBACK_KEYS)
    if fb is None:
        return
    dev.feedback = v.boolean(fb, "enabled", "feedback", default=True)
    dev.host_fb = dev.feedback and v.boolean(fb, "script", "feedback", default=True)
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


def validate_din(v, dev, data, tr, plat):
    """din_midi: 5-pin DIN MIDI In / Out on a UART (core/README.md "DIN MIDI"). mode: how the jacks reach
    USB: usb_merge (on the main port, the default), separate_port (a second USB-MIDI port: hangs Chrome's
    Web MIDI on Windows for now, see core/README.md) or din_only. thru,
    controller_out and usb_to_din are the start values of midi.thru, midi.din_out, midi.usb_to_din."""
    if "din_midi" not in data:
        return
    line = data.line_of("din_midi")
    d = v.mapping(data["din_midi"], "din_midi", line, DIN_KEYS)
    if d is None:
        return
    tx = v.pin(d.get("tx_pin"), "din_midi.tx_pin", d.line_of("tx_pin") if "tx_pin" in d else d.line, "out",
               allow_hub=False)
    rx = None
    if d.get("rx_pin") is not None:
        rx = v.pin(d["rx_pin"], "din_midi.rx_pin", d.line_of("rx_pin"), "in", allow_hub=False)
    mode = DIN_MODES.index("usb_merge")
    if "mode" in d:
        m = enum_value(v, d, "mode", "din_midi.mode", DIN_MODES)
        mode = mode if m is None else m
        if m == DIN_MODES.index("separate_port"):
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
    name = v.string(d, "port_name", "din_midi")
    if name is not None and mode != DIN_MODES.index("separate_port"):
        v.warn("din_midi.port_name", d.line_of("port_name"), f"ignored: only separate_port has its own port "
                                                              f"(mode is {DIN_MODES[mode]})")
    if name is not None and len(name) > COMP_STR_MAX:
        v.err("din_midi.port_name", d.line_of("port_name"), f"{len(name)} characters: at most {COMP_STR_MAX}")
    usb = v.boolean(d, "usb_to_din", "din_midi", default=False)
    if "usb_to_din" in d and mode != DIN_MODES.index("usb_merge"):
        v.warn("din_midi.usb_to_din", d.line_of("usb_to_din"),
               "ignored: only usb_merge has it (separate_port always sends its port's messages to DIN Out)")
    if rx is None and d.get("rx_pin") is None and "thru" in d:
        v.warn("din_midi.thru", d.line_of("thru"), "there is no rx_pin (MIDI In) to pass through")
    dev.din = Hub(tx=tx or NO_PIN, rx=rx, mode=mode, port_name=name,
                  thru=v.boolean(d, "thru", "din_midi", default=False),
                  out=v.boolean(d, "controller_out", "din_midi", default=True), usb=usb,
                  dma=v.boolean(d, "dma", "din_midi", default=False))


def validate_mcu(v, dev, data, tr, daw_port):
    """mcu: { extender } turns on Mackie Control. On USB it gets the second cable (usb.daw_port, default
    on with mcu:); on serial / BLE (one port) the whole device speaks MCU."""
    if "mcu" in data:
        m = data["mcu"]
        if m is None:
            m = LDict()
            m.line = data.line_of("mcu")
        m = v.mapping(m, "mcu", data.line_of("mcu"), MCU_KEYS)
        if m is not None:
            ext = v.boolean(m, "extender", "mcu")
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


# LED looks (idle: / active:): which of the LED's pins are lit, bit 0 = pin, 1 = pin2, 2 = pin3. Names per
# kind of LED; "on" is the LED's default active look on every kind.
LED_LOOKS = {
    "single": {"off": 0, "on": 1},
    "bicolour": {"off": 0, "on": 1, "colour1": 1, "colour2": 2, "both": 3},
    "rgb": {"off": 0, "on": 7, "red": 1, "green": 2, "yellow": 3, "blue": 4, "magenta": 5, "cyan": 6,
            "white": 7},
}
LED_LOOK_NAMES = {n: m for k in ("rgb", "bicolour", "single") for n, m in LED_LOOKS[k].items() if n != "on"}
LED_DEFAULT_LOOK = {"single": (0, 1), "bicolour": (0, 1), "rgb": (0, 7)}   # (idle, active)
# Full-colour looks, for LEDs that can show any colour (one LED of a full-colour hub, FULL_COLOUR_HUBS):
# "#rrggbb", or an RGB look name with a brightness, "blue 25%". Elsewhere (led_defaults, a cycle step's
# colour) they show as the nearest on/off look.
RGB_LOOK_COLOURS = {"red": 0xFF0000, "green": 0x00FF00, "yellow": 0xFFFF00, "blue": 0x0000FF,
                    "magenta": 0xFF00FF, "cyan": 0x00FFFF, "white": 0xFFFFFF}
RGB_LOOK_RE = re.compile(r"^#([0-9a-f]{6})$|^([a-z]+)\s+(\d{1,3})\s*%$")


def colour_look(word):
    """'#rrggbb' or '<colour> <n>%' -> 0xRRGGBB, else None."""
    m = RGB_LOOK_RE.match(word)
    if not m:
        return None
    if m.group(1):
        return int(m.group(1), 16)
    base, pct = RGB_LOOK_COLOURS.get(m.group(2)), int(m.group(3))
    if base is None or pct > 100:
        return None
    return sum((((base >> sh) & 0xFF) * pct + 50) // 100 << sh for sh in (16, 8, 0))


def colour_mask(c):
    """The pins an on/off RGB LED lights for colour c: those at least half the brightest (as FB_LED_RGB)."""
    ch = [(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF]
    top = max(ch)
    return sum(1 << k for k in range(3) if top and 2 * ch[k] >= top)


def led_full_colour(dev, pin, pin2, pin3):
    """The LED's pins are one LED of a full-colour hub (its red, green and blue in a row, from LED *
    colour_pins): it can show any colour (controller.c led_full_colour, vpin_rgb)."""
    if pin in (None, NO_PIN) or pin.hub in (0, 0xFF) or pin.hub > len(dev.hubs):
        return False
    hub = dev.hubs[pin.hub - 1]
    return (hub.kind.full_colour and pin.num % hub.kind.colour_pins(hub) == 0 and
            pin2 is not None and pin2.hub == pin.hub and pin2.num == pin.num + 1 and
            pin3 is not None and pin3.hub == pin.hub and pin3.num == pin.num + 2)


COLOUR_LOOK_NEEDS = ("needs an LED that can show any colour: an RGB LED whose pin, pin2 and pin3 are one LED "
                     f"of a full-colour hub ({', '.join(FULL_COLOUR_HUBS)}: numbers LED * 3, + 1, + 2; "
                     "LED * 4 on an RGBW strip)")


def colour_nearest(kind, c):
    """Full colour c on an on/off LED of this kind: the pins of its nearest look (as FB_LED_RGB: each channel
    lit if at least half the brightest), on a bicolour or single LED the pins of that it has, else its first
    colour; black is off."""
    want = colour_mask(c)
    if not want:
        return 0
    has = {"single": 1, "bicolour": 3, "rgb": 7}[kind]
    return (want & has) or 1


def colour_exact(kind, c):
    """An on/off LED of this kind shows colour c as it is (each channel 0 or 255, and one it has)."""
    chans = [(c >> sh) & 0xFF for sh in (16, 8, 0)]
    return kind == "rgb" and all(x in (0, 255) for x in chans)


def validate_rgb_calibration(v, h, p, white_n=3):
    """A full-colour hub's gamma: (1.0..3.0, default 1 = straight) and white: [r, g, b] (or [r, g, b, w] with
    white_n 4; 1..255 each, default 255s)."""
    gamma = 1.0
    if h.get("gamma") is not None:
        g = h["gamma"]
        if isinstance(g, bool) or not isinstance(g, (int, float)) or not 1.0 <= float(g) <= 3.0:
            v.err(f"{p}.gamma", h.line_of("gamma"), f"must be a number from 1.0 to 3.0 (1 = straight; LEDs "
                                                    f"usually look right at about 2.2), not {g!r}")
        else:
            gamma = float(g)
    white = [255] * white_n
    if h.get("white") is not None:
        w = h["white"]
        if (not isinstance(w, list) or len(w) != white_n or
                any(isinstance(x, bool) or not isinstance(x, int) or not 1 <= x <= 255 for x in w)):
            what = "[red, green, blue]" if white_n == 3 else "[red, green, blue, white]"
            v.err(f"{p}.white", h.line_of("white"), f"must be {what}, each 1..255: what full white "
                                                    "sends, e.g. [255, 200, 220] to take some green and blue out")
        else:
            white = list(w)
    return gamma, white


def rgb_curve(gamma):
    """256 entries: value v (as seen) to the LED's PWM, 255 * (v / 255) ^ gamma; above 0 stays above 0."""
    return [0] + [max(1, round(255 * (k / 255) ** gamma)) for k in range(1, 256)]


def led_kind(pin2, pin3):
    return "rgb" if pin3 is not NO_PIN else "bicolour" if pin2 is not NO_PIN else "single"


def look_word(d, key):
    """The look as written: `on` / `off` read as booleans unquoted, so take the raw text then."""
    x = d.get(key)
    return str(d.raw_of(key) if isinstance(x, bool) else x).strip().lower()


def validate_led_defaults(v, data):
    """led_defaults: { idle, active }: every LED's looks unless it sets its own. Any name of any kind of LED;
    each LED shows the nearest it can (resolve_led_default)."""
    out = {}
    if data.get("led_defaults") is None:
        return out
    d = v.mapping(data["led_defaults"], "led_defaults", data.line_of("led_defaults"), LED_DEFAULTS_KEYS)
    if d is None:
        return out
    for key in ("idle", "active"):
        if d.get(key) is None:
            continue
        w = look_word(d, key)
        if w != "on" and w not in LED_LOOK_NAMES and colour_look(w) is None:
            close = difflib.get_close_matches(w, ["on", *LED_LOOK_NAMES], 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"led_defaults.{key}", d.line_of(key), f"unknown look '{w}' ({hint}valid: on, "
                                                          f"{', '.join(LED_LOOK_NAMES)}, or a full colour: "
                                                          "\"#rrggbb\", blue 20%)")
            continue
        out[key] = w
    return out


def resolve_led_default(kind, word, full=False):
    """A led_defaults look (or a cycle step's colour) on an LED of this kind -> (pin mask, full colour or
    None): its own name if it has it, else the pins of that look it has, else (a colour it can't show) its
    first colour. A full colour ("#rrggbb", "blue 20%") shows as it is on an LED that can show any colour
    (full), elsewhere as the nearest on/off look (colour_nearest)."""
    col = colour_look(word) if word not in LED_LOOKS[kind] and word not in LED_LOOK_NAMES else None
    if col is not None:
        if full and kind == "rgb":
            return colour_mask(col), col
        return colour_nearest(kind, col), None
    if word in LED_LOOKS[kind]:
        return LED_LOOKS[kind][word], None
    want = LED_LOOK_NAMES[word]
    has = {"single": 1, "bicolour": 3, "rgb": 7}[kind]
    return (want & has) or 1, None


def validate_led_looks(v, d, p, kind, defaults, full=False):
    """((idle, active) pin masks, or None for the defaults (generated as 0); (idle, active) full colours,
    None each for none). full: the LED can show any colour (led_full_colour)."""
    looks = list(LED_DEFAULT_LOOK[kind])
    rgb = [None, None]
    given = False
    set_active = False
    for k, key in enumerate(("idle", "active")):
        if d.get(key) is not None:
            w = look_word(d, key)
            col = colour_look(w) if kind == "rgb" and w not in LED_LOOKS[kind] else None
            if col is not None:
                if not full:
                    v.err(f"{p}.{key}", d.line_of(key), f"'{w}' {COLOUR_LOOK_NEEDS}; this one takes "
                                                          f"{', '.join(LED_LOOKS[kind])}")
                    continue
                looks[k], rgb[k] = colour_mask(col), col
                given = True
                set_active = set_active or k == 1
                continue
            if w not in LED_LOOKS[kind]:
                other = [x for x in ("rgb", "bicolour") if w in LED_LOOKS[x]]
                what = {"single": "a single-colour LED (pin only)", "bicolour": "a bicolour LED (pin, pin2)",
                        "rgb": "an RGB LED (pin, pin2, pin3)"}[kind]
                if other:
                    msg = f"'{w}' is a look for {'an RGB' if other[0] == 'rgb' else 'a bicolour'} LED; {what} takes"
                else:
                    close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
                    msg = (f"unknown look '{w}' (did you mean '{close[0]}'?);" if close else f"unknown look '{w}';") + \
                          f" {what} takes"
                v.err(f"{p}.{key}", d.line_of(key), f"{msg} {', '.join(LED_LOOKS[kind])}")
                continue
            looks[k] = LED_LOOKS[kind][w]
            given = True
            set_active = set_active or k == 1
        elif key in defaults:
            looks[k], rgb[k] = resolve_led_default(kind, defaults[key], full)
            given = True
    if not given:
        return None, None
    if looks[0] == looks[1] and rgb[0] == rgb[1]:
        if not set_active:
            looks[1] = 0            # lit when idle and no active: of its own: dark when active
        else:
            v.warn(p, d.line, "idle and active look the same: the LED never shows its state")
    return tuple(looks), (tuple(rgb) if any(c is not None for c in rgb) else None)


def validate_components(v, dev, data):
    names = {}       # (kind, display name) -> path, for the duplicate-name warning
    # LEDs first (buttons refer to them)
    led_defaults = validate_led_defaults(v, data)
    leds = v.seq(data.get("leds"), "leds", data.line_of("leds"))
    for i, d in enumerate(leds):
        p = f"leds[{i}]"
        d = v.mapping(d, p, leds.line_of(i), LED_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "LED", len(dev.leds))
        name = v.display_name(d, p, ident)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "out")
        pin2 = v.pin(d["pin2"], f"{p}.pin2", d.line_of("pin2"), "out") if d.get("pin2") is not None else NO_PIN
        pin3 = v.pin(d["pin3"], f"{p}.pin3", d.line_of("pin3"), "out") if d.get("pin3") is not None else NO_PIN
        if d.get("pin3") is not None and d.get("pin2") is None:
            v.err(f"{p}.pin3", d.line_of("pin3"), "an RGB LED needs pin2 too (pin red, pin2 green, pin3 blue)")
        looks, rgb = validate_led_looks(v, d, p, led_kind(pin2 or NO_PIN, pin3 or NO_PIN), led_defaults,
                                        led_full_colour(dev, pin, pin2, pin3))
        src = validate_led_source(v, dev, d, p)
        owner = validate_owner(v, d, p)
        timeout = validate_timeout(v, d, "timeout", p, None)
        if timeout is not None and owner in (LED_OWNERS["host"], LED_OWNERS["local"]):
            v.warn(f"{p}.timeout", d.line_of("timeout"), "only used with owner: host_then_local")
        led_owner_line = d.line_of("owner") if owner == LED_OWNERS["local"] else None
        dev.leds.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, pin2=pin2 or NO_PIN,
                            pin3=pin3 or NO_PIN, looks=looks, rgb=rgb, source=src,
                            owner=owner, timeout=timeout, path=p, line=d.line, used_by=[],
                            daw=daw_role(v, dev, d, p), local_line=led_owner_line))

    buttons = v.seq(data.get("buttons"), "buttons", data.line_of("buttons"))
    for i, d in enumerate(buttons):
        p = f"buttons[{i}]"
        d = v.mapping(d, p, buttons.line_of(i), BUTTON_KEYS, renamed=RENAMED_BUTTON)
        if d is None:
            continue
        ident = v.ident(d, p, "button", len(dev.buttons))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "button", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "in")
        act, num = button_action(v, dev, d, p)
        led = -1
        if d.get("led") is not None:
            idx = v.ref(d, "led", f"{p}.led", d.line_of("led"), "LED", "LEDs")
            if idx is not None:
                led = idx
                if led > 127:
                    v.err(f"{p}.led", d.line_of("led"), "LED index over 127 can't be referenced")
                    led = -1
        if led >= 0:
            dev.leds[led].used_by.append(("button", act))
        top = 16383 if act == "sysex" and sysex_wide(dev, num) else 127
        dev.buttons.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, action=ACTION_IDS[act],
                               action_name=act, number=num, led=led, daw=daw_role(v, dev, d, p),
                               ctl=control_out(v, d, p, top), bank=button_bank(v, dev, d, p, act)))

    analogs = v.seq(data.get("analog"), "analog", data.line_of("analog"))
    for i, d in enumerate(analogs):
        p = f"analog[{i}]"
        d = v.mapping(d, p, analogs.line_of(i), ANALOG_KEYS, renamed=RENAMED_ANALOG)
        if d is None:
            continue
        ident = v.ident(d, p, "analog input", len(dev.analogs))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "analog input", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog")
        fader = mcu_name(v, dev, d, p, MCU_FADERS, "fader_1")
        mode = 0
        if "mode" in d:
            mode = enum_value(v, d, "mode", f"{p}.mode", ANALOG_MODES)
            mode = 0 if mode is None else mode
            mode_use(v, dev, ANALOG_MODES[mode], f"{p}.mode", d.line_of("mode"))
        if mode != 0 and fader is not None:
            v.err(f"{p}.mode", d.line_of("mode"), "a Mackie Control fader (mcu:) sends its own messages: "
                                                  f"drop mode: {ANALOG_MODES[mode]} or mcu:")
        if mode == 1 and pin is not None and pin.hub and dev.hubs[pin.hub - 1].type in XY_HUBS:
            v.err(f"{p}.mode", d.line_of("mode"), "pitch_bend centres on the power-up reading, and an XY "
                                                  "pad reads nothing until touched: use cc or channel_pressure")
        cc = mode_number(v, dev, d, p, ANALOG_MODES[mode], fader is None and mode == 0, ANALOG_NUMBER_KEYS)
        if fader is not None:
            for k in ("channel", "min", "max"):
                if k in d:
                    v.err(f"{p}.{k}", d.line_of(k), "a Mackie Control fader (mcu:) sends its own messages: "
                                                    f"drop {k}: or mcu:")
        ctl = control_out(v, d, p, 16383 if mode_wide(dev, ANALOG_MODES[mode], cc) else 127)
        lo = v.integer(d, "adc_min", p, 0, 4095, default=0)
        hi = v.integer(d, "adc_max", p, 0, 4095, default=4095)
        if lo == hi:
            v.err(p, d.line, f"adc_min and adc_max are both {lo}")
        pickup = validate_takeover(v, dev, d, p, fader)
        dev.analogs.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, cc=cc, mode=mode, raw_min=lo,
                               raw_max=hi, pickup=pickup, touch=validate_touch(v, d, p), mcu=fader,
                               daw=daw_role(v, dev, d, p), ctl=ctl))

    encs = v.seq(data.get("encoders"), "encoders", data.line_of("encoders"))
    accel_default = encoder_settings(v, dev, data)
    for i, d in enumerate(encs):
        p = f"encoders[{i}]"
        d = v.mapping(d, p, encs.line_of(i), ENCODER_KEYS, renamed=RENAMED_ENCODER)
        if d is None:
            continue
        ident = v.ident(d, p, "encoder", len(dev.encoders))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "encoder", name, p, d)
        enc_hubs = IN_HUBS - MUX_HUBS
        why = "encoders can't be on a mux"
        pins = {}
        for key in ("pin_a", "pin_b"):
            if key not in d and key[-1] in d:
                pins[key] = None        # format 1 a:/b:, reported as renamed
                continue
            pins[key] = v.pin(d.get(key), f"{p}.{key}", d.line_of(key), "in", hubs=enc_hubs, why=why)
            pn = pins[key]
            if pn and pn.hub and dev.hubs[pn.hub - 1].type in I2C_HUBS:
                v.warn(f"{p}.{key}", d.line_of(key), "encoder on an I2C hub is polled from the main "
                                                     "loop: fast spinning may miss steps")
        vpot = mcu_name(v, dev, d, p, MCU_VPOTS, "vpot_1")
        mode = 0
        if "mode" in d:
            mode = enum_value(v, d, "mode", f"{p}.mode", ENC_MODES)
            mode = 0 if mode is None else mode
            mode_use(v, dev, ENC_MODES[mode], f"{p}.mode", d.line_of("mode"))
        for k in ("mode", "channel", "min", "max"):
            if k in d and vpot is not None:
                v.err(f"{p}.{k}", d.line_of(k), f"a Mackie Control V-Pot (mcu:) sends its own messages: drop {k}: "
                                                "or mcu:")
        cc = mode_number(v, dev, d, p, ENC_MODES[mode], vpot is None, ENCODER_NUMBER_KEYS)
        ctl = control_out(v, d, p, 16383 if mode_wide(dev, ENC_MODES[mode], cc) else 127)
        spd = v.integer(d, "steps_per_detent", p, 1, 255, default=4)
        ring = encoder_ring(v, dev, d, p, vpot)
        center = ring_center(v, dev, d, p, vpot)
        accel = validate_acceleration(v, d, p, accel_default)[1]
        dev.encoders.append(Hub(id=ident or "", name=name, a=pins["pin_a"] or NO_PIN, b=pins["pin_b"] or NO_PIN,
                                cc=cc, steps=spd, mcu=vpot, ring=ring, ring_center=center,
                                daw=daw_role(v, dev, d, p), accel=accel, mode=mode, ctl=ctl))
    check_mcu_unique(v, dev, data)


def enc_accel(dev):
    """Any encoder with acceleration: GM_ENC_ACCEL 1 and gm_enc_accel[]; none: the feature compiles out."""
    return any(getattr(e, "accel", None) for e in dev.encoders)


def mcu_needed(v, dev, path, line):
    if dev.mcu is None:
        v.err(path, line, "Mackie Control needs an `mcu:` section at the top level")
        return False
    return True


def mcu_name(v, dev, d, p, table, example):
    """A component's `mcu:` (fader_1 / vpot_1 / ...) -> its number, or None."""
    if d.get("mcu") is None:
        return None
    raw, line = d.raw_of("mcu"), d.line_of("mcu")
    if not mcu_needed(v, dev, f"{p}.mcu", line):
        return None
    if d["mcu"] not in table:
        close = difflib.get_close_matches(raw, list(table), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{p}.mcu", line, f"unknown MCU control '{raw}' ({hint}valid: {', '.join(table)})")
        return None
    return table[d["mcu"]]


def button_mcu(v, dev, d, p, act):
    """action: mcu + mcu: <button> -> the default button.number (the MCU note)."""
    line = d.line_of("mcu") if "mcu" in d else d.line
    if act != "mcu":
        v.err(f"{p}.mcu", line, f"mcu: is for action: mcu (this button's action is {act}; leave action: out "
                                "or set it to mcu)")
        return 0
    if not mcu_needed(v, dev, f"{p}.action", d.line_of("action") if "action" in d else line):
        return 0
    for k in NUMBER_KEYS:
        if k in d:
            v.err(f"{p}.{k}", d.line_of(k), f"action mcu takes mcu: (e.g. `mcu: play`), not {k}:")
    if d.get("mcu") is None:
        v.err(p, d.line, "action mcu needs mcu: (the MCU button, e.g. `mcu: play`, `rec_1`, `f1`)")
        return 0
    if d["mcu"] not in MCU_NOTES:
        close = difflib.get_close_matches(d.raw_of("mcu"), list(MCU_NOTES), 1)
        hint = f"; did you mean '{close[0]}'?" if close else " (core/README.md \"Mackie Control\" has the list)"
        v.err(f"{p}.mcu", line, f"unknown MCU button '{d.raw_of('mcu')}'{hint}")
        return 0
    return MCU_NOTES[d["mcu"]]


def validate_takeover(v, dev, d, p, fader):
    """analog takeover: none | pickup. MCU faders default to pickup (no motors to follow the host)."""
    if d.get("takeover") is None:
        return fader is not None
    t = d["takeover"]
    if t not in TAKEOVER:
        v.err(f"{p}.takeover", d.line_of("takeover"), f"unknown takeover '{d.raw_of('takeover')}' "
                                                      f"(valid: {', '.join(TAKEOVER)})")
        return False
    if t == "pickup" and fader is None and not f1_on(dev):
        v.err(f"{p}.takeover", d.line_of("takeover"), "pickup needs the host's value: " + f1_off_why(dev))
        return False
    return t == "pickup"


def validate_touch(v, d, p):
    """analog touch: none | movement (EVENT touch while it moves) -> GET_COMPONENT's touch byte. A touch
    sensor, touch: { pin: ... }, is reserved."""
    if d.get("touch") is None:
        return 0
    t, line = d["touch"], d.line_of("touch")
    if isinstance(t, dict):
        v.err(f"{p}.touch", line, "a touch sensor isn't supported yet (use touch: movement)")
        return 0
    if t not in TOUCH:
        v.err(f"{p}.touch", line, f"unknown touch '{d.raw_of('touch')}' (valid: {', '.join(TOUCH)})")
        return 0
    return TOUCH[t]


def encoder_ring(v, dev, d, p, vpot):
    """ring: [LED ids] around an MCU V-Pot (leftmost first)."""
    if d.get("ring") is None:
        return []
    line = d.line_of("ring")
    if vpot is None and not dev.host_fb:
        if dev.mcu is not None or "mcu" not in d:
            v.err(f"{p}.ring", line, "ring: is driven by Mackie Control or a DAW script: give the encoder "
                                     "`mcu: vpot_N`, or leave feedback.script on")
        return []
    lst = v.seq(d["ring"], f"{p}.ring", line)
    if not 1 <= len(lst) <= 16:
        v.err(f"{p}.ring", line, f"{len(lst)} LEDs: a ring has 1..16")
    out = []
    for k in range(len(lst)):
        idx = v.ref(lst, k, f"{p}.ring[{k}]", lst.line_of(k), "LED", "LEDs")
        if idx is not None:
            out.append(idx)
            dev.leds[idx].used_by.append(("ring", None))
    return out


def ring_center(v, dev, d, p, vpot):
    """ring_center: the LED under an MCU V-Pot (lit by bit 6 of its ring value). -> LED index or None."""
    if d.get("ring_center") is None:
        return None
    line = d.line_of("ring_center")
    if vpot is None and not dev.host_fb:
        if dev.mcu is not None or "mcu" not in d:
            v.err(f"{p}.ring_center", line, "ring_center: is driven by Mackie Control or a DAW script: give the "
                                            "encoder `mcu: vpot_N`, or leave feedback.script on")
        return None
    idx = v.ref(d, "ring_center", f"{p}.ring_center", line, "LED", "LEDs")
    if idx is not None:
        dev.leds[idx].used_by.append(("ring", None))
    return idx


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


def f1_on(dev):
    """Plain feedback reaches the main port: enabled, and the main port isn't the MCU port."""
    return dev.feedback and not (dev.mcu is not None and dev.mcu.cable == 0)


def f1_off_why(dev):
    if not dev.feedback:
        return "feedback is off (feedback: { enabled: false })"
    return ("with mcu: on a single port (no USB DAW port), everything the host sends is read as "
            "Mackie Control")


def validate_led_source(v, dev, d, p):
    """LED feedback: { note | cc | clock | transport | mcu, channel } -> (source, num, channel) or None."""
    if d.get("feedback") is None:
        return None
    fp, line = f"{p}.feedback", d.line_of("feedback")
    f = v.mapping(d["feedback"], fp, line, [*LED_SOURCES, "channel"])
    if f is None:
        return None
    kinds = [k for k in LED_SOURCES if k in f]
    if len(kinds) != 1:
        v.err(fp, line, ("give one of " if not kinds else f"only one of {', '.join(kinds)}; one of ") +
              "note: / cc: (a number), clock: beat, transport: playing, mcu: <MCU LED>")
        return None
    k = kinds[0]
    kl = f.line_of(k)
    ch = 0
    if "channel" in f:
        if k in ("note", "cc"):
            ch = v.integer(f, "channel", fp, 1, 16, default=0)
        else:
            v.err(f"{fp}.channel", f.line_of("channel"), f"only for note: / cc: (a {k} source has no channel)")
    if k == "mcu":
        if not mcu_needed(v, dev, f"{fp}.mcu", kl):
            return None
        if f["mcu"] not in MCU_NOTES:
            close = difflib.get_close_matches(f.raw_of("mcu"), list(MCU_NOTES), 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            v.err(f"{fp}.mcu", kl, f"unknown MCU LED '{f.raw_of('mcu')}'{hint}")
            return None
        return (LED_SOURCES[k], MCU_NOTES[f["mcu"]], 0)
    if not f1_on(dev):
        v.err(f"{fp}.{k}", kl, f"{k}: is plain feedback, but " + f1_off_why(dev))
        return None
    if k in LED_SOURCE_VALUES:
        if f[k] not in LED_SOURCE_VALUES[k]:
            v.err(f"{fp}.{k}", kl, f"'{f.raw_of(k)}' isn't a {k} source (valid: {', '.join(LED_SOURCE_VALUES[k])})")
            return None
        return (LED_SOURCES[k], 0, 0)
    num = v.integer(f, k, fp, 0, 127, default=None)
    if num is None:
        return None
    return (LED_SOURCES[k], num, ch)


def unique_name(v, names, kind, name, path, d):
    """Display names label the rows of the web config table and the CLI's `param:name` keys:
    two of the same kind with one name can't be told apart."""
    if not name:
        return
    first = names.setdefault((kind, name.lower()), path)
    if first != path:
        v.warn(f"{path}.name" if "name" in d else path, d.line_of("name") if "name" in d else d.line,
               f"'{name}' is also the name of {first}: the config page and CLI can't tell them apart "
               "(set a different name:)")


def button_action(v, dev, d, p, base=None):
    """A button's action and default button.number, from action: and cc: / note: / preset: / mcu:.
    base: (action, number) of the component, for a preset's override of it: what the override leaves
    out stays as the component has it. Returns (action name, number)."""
    if "action" in d:
        act = d["action"]
        if act not in ACTIONS:
            close = difflib.get_close_matches(str(act), ACTIONS, 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.action", d.line_of("action"), f"unknown action '{d.raw_of('action')}' ({hint}valid: "
                                                       f"{', '.join(ACTIONS)})")
            return base or ("none", 0)
    else:
        act = base[0] if base else "mcu" if "mcu" in d else "none"
    if act in MSG_ACTIONS:
        if act == "sysex" and not dev.sysex:
            v.err(f"{p}.action", d.line_of("action") if "action" in d else d.line,
                  "action sysex sends one of the midi_messages.sysex strings, and there are none")
        dev.msg_used.add(MSG_ACTIONS[act])
    if act in PAD_ACTIONS:
        dev.pad_action_uses.append((f"{p}.action", d.line_of("action") if "action" in d else d.line, act))
    if act in PRESET_ACTIONS and dev.n_presets < 2:
        v.err(f"{p}.action", d.line_of("action") if "action" in d else d.line,
              f"action {act} switches presets, but this device has {dev.n_presets} preset slot: add "
              "presets: (with 2 or more slots)")
        return act, (base[1] if base else 0)
    if act == "mcu" or "mcu" in d:
        if base and act == base[0] == "mcu" and "mcu" not in d:
            return act, base[1]
        num = button_mcu(v, dev, d, p, act)
        return (act if act == "mcu" else base[0] if base else "none"), num
    return act, button_number(v, dev, d, p, act, base)


def button_number(v, dev, d, p, act, base=None):
    """The default button.number: cc: for the CC actions, note: for action note, preset: (the slot) for
    preset_select. With base (a preset override), a number left out keeps the component's, and a
    number on an action that takes none is an error (in a component it only warns)."""
    want = number_key(act)
    keep = base[1] if base else 0
    given = [k for k in NUMBER_KEYS if k in d]
    if "cc" in d and "note" in d:
        v.err(p, d.line, "give cc: or note:, not both")
    elif len(given) > 1:
        v.err(p, d.line, f"give one of {', '.join(k + ':' for k in given)}, not {len(given)}")
    if want is None:
        if given:
            k = given[0]
            if base or k in ("preset", "mmc", "sysex", "program"):
                what = "a preset number" if k == "preset" else "a CC or note"
                v.err(f"{p}.{k}", d.line_of(k), f"action {act} takes no {what}: drop {k}:")
                return keep
            v.warn(f"{p}.{k}", d.line_of(k), f"action {act} sends no CC or note; {k}: only "
                                             "sets the default button.number for when the action is "
                                             "changed in the config")
            return v.integer(d, k, p, 0, 127, default=0)
        return keep
    other = [k for k in given if k != want]
    if other:
        v.err(f"{p}.{other[0]}", d.line_of(other[0]), f"action {act} takes {want}:, not {other[0]}:")
        return v.integer(d, other[0], p, 0, 127, default=0) if other[0] in ("cc", "note") else keep
    if "number" in d:          # format 1; reported as renamed
        return 0
    lo, hi = (1, dev.n_presets) if want == "preset" else (0, 127)
    if want not in d:
        if base and act == base[0]:
            return keep
        what = {"mmc": f"an MMC command: {', '.join(MMC_COMMANDS)}, or 1..127",
                "sysex": "the id of a midi_messages.sysex string"}.get(want)
        v.err(p, d.line, f"action {act} needs {want}: (" + (what or f"{lo}..{hi}" +
              (", the preset slot" if want == "preset" else "")) + ")")
        return 0
    if want == "mmc":
        return mmc_command(v, d, p)
    if want == "sysex":
        return sysex_ref(v, dev, d, "sysex", p)
    return v.integer(d, want, p, lo, hi, default=0)


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
              ram_bytes=0, intensity=0, grids=0, fade=False, touch_min=0, touch_max=0, dma=False, raw=h)
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


def validate_xy_pad(v, hub, h, p, ctl):
    """xy_pad: a 4-wire resistive panel on four MCU pins. Reading X drives x_plus high and x_minus low,
    and reads the Y plate; reading Y the other way round. Each axis is read on the other plate's minus
    pin if it has an ADC, else its plus pin."""
    hub.n_pins = 2
    hub.sel = [ctl(k, "out") for k in ("x_plus_pin", "x_minus_pin", "y_plus_pin", "y_minus_pin")]
    hub.touch_min = v.integer(h, "touch_min", p, 0, 4095, default=XY_TOUCH[0])
    hub.touch_max = v.integer(h, "touch_max", p, 0, 4095, default=XY_TOUCH[1])
    if hub.touch_min >= hub.touch_max:
        v.err(p, h.line, f"touch_min {hub.touch_min} must be below touch_max {hub.touch_max}")

    def sense(minus, plus, axis, keys):
        for pn in (minus, plus):
            if pn.hub == 0 and pn.num >= 0 and v.plat.adc(pn.num)[0]:
                return pn
        if all(pn.hub == 0 for pn in (minus, plus)) and all(h.get(k) is not None for k in keys):
            v.err(p, h.line, f"{axis} is read on {keys[0]} or {keys[1]}: neither has an ADC "
                             f"({adc_text(v.plat)})")
        return NO_PIN

    xp, xm, yp, ym = hub.sel
    hub.data = sense(ym, yp, "X", ("y_minus_pin", "y_plus_pin"))
    hub.clock = sense(xm, xp, "Y", ("x_minus_pin", "x_plus_pin"))


def validate_decoder(v, hub, h, p, index):
    """decoder: { address: [A0, A1, A2], groups: [{ PB6: 1, PB7: 0 }, ...] }. Output n = address
    n % 2^len(address) in group n // 2^len(address); a group is the levels of the enable pins."""
    addr = h.get("address")
    if not isinstance(addr, list) or not 1 <= len(addr) <= 4:
        v.err(f"{p}.address", h.line_of("address") if "address" in h else h.line,
              "needs a list of 1-4 address pins, A0 first")
        addr = LList()
    hub.sel = [v.pin(x, f"{p}.address[{i}]", addr.line_of(i), "out", allow_hub=False, line_kind="address",
                     owner=index) or NO_PIN for i, x in enumerate(addr)]
    hub.n_sel = len(hub.sel)
    groups = h.get("groups")
    if groups is None:
        hub.group_lv = [0]
    else:
        groups = v.seq(groups, f"{p}.groups", h.line_of("groups"))
        if not 1 <= len(groups) <= 4:
            v.err(f"{p}.groups", h.line_of("groups"), f"{len(groups)} groups: 1-4 (each a mapping of "
                                                      "enable pin to level, e.g. { PB6: 1, PB7: 0 })")
        names = None
        seen = {}
        for g, grp in enumerate(groups[:4]):
            gp = f"{p}.groups[{g}]"
            if not isinstance(grp, dict) or not grp:
                v.err(gp, groups.line_of(g), "must be a mapping of enable pin to level, e.g. { PB6: 1, PB7: 0 }")
                continue
            nums = {}
            for k in grp:
                try:
                    nums[v.plat.parse_pin(k)] = k
                except ValueError as e:
                    v.err(f"{gp}.{k}", grp.line_of(k), str(e))
            if names is None:
                if len(nums) > 3:
                    v.err(gp, groups.line_of(g), f"{len(nums)} enable pins: at most 3")
                names = list(nums)[:3]
                hub.en = [v.pin(nums[n], f"{gp}.{nums[n]}", grp.line_of(nums[n]), "out", allow_hub=False,
                                line_kind="enable", owner=index) or NO_PIN for n in names]
            elif set(nums) != set(names):
                v.err(gp, groups.line_of(g), "every group must give a level for the same enable pins "
                                             f"({', '.join(v.plat.pin_name(n) for n in names)})")
                continue
            lv = 0
            for e, n in enumerate(names):
                x = grp[nums[n]]
                if isinstance(x, bool):
                    x = int(x)
                if x not in (0, 1):
                    v.err(f"{gp}.{nums[n]}", grp.line_of(nums[n]), f"level must be 0 or 1 (got {x!r})")
                    x = 0
                lv |= x << e
            if lv in seen:
                v.err(gp, groups.line_of(g), f"same enable levels as groups[{seen[lv]}]: the two can't be told apart")
            seen.setdefault(lv, g)
            hub.group_lv.append(lv)
        if not hub.group_lv:
            hub.group_lv = [0]
    hub.n_pins = len(hub.group_lv) << hub.n_sel


def matrix_rows(v, d, path, hubs, why, role="out"):
    """Rows of a key_matrix / switch_matrix: a list of pins, or { decoder: <id>, first: n, count: n }
    for consecutive outputs of a decoder hub."""
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
    return [v.pin(x, f"{p}[{i}]", rows.line_of(i), role, hubs=hubs, why=why) or NO_PIN
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


def resolve_switch_matrices(v, dev):
    for hub in dev.hubs:
        if hub.type != "switch_matrix" or not isinstance(hub.raw, dict):
            continue
        why = "switch_matrix rows are MCU pins or decoder outputs"
        hub.rows = matrix_rows(v, hub.raw, hub.path, ROW_HUBS, why)
        hub.cols = matrix_columns(v, hub.raw, hub.path, owner=f"hub{hub.index}")


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


# key_matrix rows and columns: the core scans at most 32 rows (GM_MATRIX_ROWS) and keeps each row's
# columns in a uint32_t (core/src/controller.c), until it is widened for larger devices.
MATRIX_MAX = 32


def validate_matrix(v, dev, d, line):
    p = "key_matrix"
    d = v.mapping(d, p, line, MATRIX_KEYS, renamed=RENAMED_MATRIX)
    if d is None:
        return
    kind = d.get("type")
    if kind not in ("switch", "velocity"):
        if not (kind is None and "kind" in d):
            v.err(f"{p}.type", d.line_of("type"), f"{'is required' if kind is None else repr(kind) + ' is unknown'} "
                                                   "(switch: one contact per key, velocity: two contacts per key)")
        kind = "switch"
    why = ("the matrix is scanned from the timer interrupt, so only MCU pins, sn74hc595 or decoder rows and "
           "sn74hc165 columns")
    pins = {"rows": matrix_rows(v, d, p, {"sn74hc595"} | ROW_HUBS, why)}
    lst = d.get("columns")
    if not isinstance(lst, list) or not lst:
        if "cols" not in d:
            v.err(f"{p}.columns", d.line_of("columns") if "columns" in d else d.line, "needs a list of pins")
        lst = LList()
    if len(lst) > MATRIX_MAX:
        v.err(f"{p}.columns", d.line_of("columns"), f"{len(lst)} columns: at most {MATRIX_MAX} (the core "
                                                    "keeps a row's columns in 32 bits)")
    # MCU columns are a "column" line, so a switch_matrix can share them (allow_other_uses)
    pins["columns"] = [v.pin(x, f"{p}.columns[{i}]", lst.line_of(i), "in", hubs={"sn74hc165"}, why=why,
                             line_kind=None if isinstance(x, dict) and any(k in x for k in PIN_KEY_HUB)
                             else "column", owner="key_matrix") or NO_PIN
                       for i, x in enumerate(lst)]
    nr, nc = len(pins["rows"]), len(pins["columns"])
    if nr > MATRIX_MAX:
        v.err(f"{p}.rows", d.line_of("rows"), f"{nr} rows: at most {MATRIX_MAX} (the core scans up to "
                                              f"{MATRIX_MAX} rows; {MATRIX_MAX} x {MATRIX_MAX} is enough for 88 keys)")
    contacts = d.get("contacts", "columns")
    if contacts not in ("columns", "rows"):
        v.err(f"{p}.contacts", d.line_of("contacts"), f"{contacts!r} is unknown (columns: a key's two contacts "
                                                      "are columns 2p, 2p+1; rows: rows 2p, 2p+1)")
        contacts = "columns"
    if "contacts" in d and kind != "velocity":
        v.err(f"{p}.contacts", d.line_of("contacts"), "only for type: velocity (two contacts per key)")
        contacts = "columns"
    by_rows = kind == "velocity" and contacts == "rows"
    if kind == "switch":
        lrows, lcols = nr, nc
    elif by_rows:
        lrows, lcols = nr // 2, nc
        if nr % 2:
            v.err(f"{p}.rows", d.line_of("rows"), f"contacts: rows needs an even number of rows "
                                                  f"(two contacts per key), got {nr}")
    else:
        lrows, lcols = nr, nc // 2
        if nc % 2:
            v.err(f"{p}.columns", d.line_of("columns"), f"type velocity needs an even number of columns "
                                                        f"(two contacts per key), got {nc}")
    cap = lrows * lcols
    keys = v.integer(d, "keys", p, 1, 128, default=min(cap, 128) or 1)
    first = v.integer(d, "first_note", p, 0, 127, default=48)
    if keys is not None and nr and nc and keys > cap and d.get("key_map") is None:
        v.err(f"{p}.keys", d.line_of("keys"), f"{keys} keys don't fit a {nr} x {nc} {kind} matrix "
                                              f"(at most {cap})")
    if keys is not None and first is not None and first + keys > 128:
        v.err(f"{p}.keys", d.line_of("keys"), f"first_note {first} + {keys} keys goes past note 127")
    settle = v.period(d, "settle", p, 0, 255, default=1, example="1us")
    vel_fast, vel_slow = validate_velocity(v, d, p, kind)
    key_map = validate_key_map(v, d, p, lrows, lcols, keys or 0, by_rows, kind)
    dev.matrix = Hub(kind=kind, rows=pins["rows"], cols=pins["columns"], keys=keys or 0,
                     first_note=first or 0, settle_us=settle, vel_fast=vel_fast, vel_slow=vel_slow,
                     daw=daw_role(v, dev, d, p), contacts=contacts, key_map=key_map)


def validate_key_map(v, d, p, lrows, lcols, keys, by_rows, kind):
    """key_map: one list per matrix row (per row pair with contacts: rows), one key number per column
    (per column pair with contacts: columns), or null where there is no key. -> flat list, row-major,
    255 = none; or None when not given."""
    if d.get("key_map") is None:
        return None
    p = f"{p}.key_map"
    km = v.seq(d["key_map"], p, d.line_of("key_map"))
    what_r = "row pair" if by_rows else "row"
    what_c = "column pair" if kind == "velocity" and not by_rows else "column"
    if len(km) != lrows:
        v.err(p, d.line_of("key_map"), f"needs one list per {what_r}: {lrows}, got {len(km)}")
    flat, where = [], {}
    for r in range(lrows):
        row = km[r] if r < len(km) else LList()
        rp = f"{p}[{r}]"
        if not isinstance(row, list):
            v.err(rp, km.line_of(r), f"must be a list of {lcols} key numbers (null = no key)")
            row = LList()
        elif len(row) != lcols:
            v.err(rp, km.line_of(r), f"needs {lcols} entries, one per {what_c} (null = no key), got {len(row)}")
        for c in range(lcols):
            x = row[c] if c < len(row) else None
            if x is None:
                flat.append(255)
                continue
            if isinstance(x, bool) or not isinstance(x, int) or not 0 <= x < keys:
                v.err(f"{rp}[{c}]", row.line_of(c) if isinstance(row, LList) else km.line_of(r),
                      f"must be a key number 0..{keys - 1} or null (got {x!r})")
                flat.append(255)
                continue
            if x in where:
                v.err(f"{rp}[{c}]", row.line_of(c), f"key {x} is already at key_map{where[x]}")
            where.setdefault(x, f"[{r}][{c}]")
            flat.append(x)
    missing = [k for k in range(keys) if k not in where]
    if missing and len(km) == lrows:
        v.warn(p, d.line_of("key_map"), f"keys not in the map never play: {', '.join(map(str, missing[:8]))}"
                                        f"{' ...' if len(missing) > 8 else ''}")
    return flat


# Default key times for velocity 127 and 1, in 0.1 ms (the velocity.fast /
# velocity.slow params; their ranges are in core/src/settings.c).
VEL_FAST_RANGE = (1, 500)
VEL_SLOW_RANGE = (20, 2000)


def validate_velocity(v, d, path, kind):
    """key_matrix.velocity: { fast, slow } (time periods) -> (fast, slow) in 0.1 ms, None where not given."""
    p = f"{path}.velocity"
    vd = v.mapping(d.get("velocity"), p, d.line_of("velocity"), VELOCITY_KEYS,
                   renamed=RENAMED_VELOCITY) if "velocity" in d else None
    if vd is None:
        return None, None
    if kind != "velocity":
        v.err(p, d.line_of("velocity"), "only for type: velocity (a switch matrix has no key times)")
        return None, None
    fast = v.period(vd, "fast", p, VEL_FAST_RANGE[0] * 100, VEL_FAST_RANGE[1] * 100, step_us=100,
                    example="1.6ms")
    slow = v.period(vd, "slow", p, VEL_SLOW_RANGE[0] * 100, VEL_SLOW_RANGE[1] * 100, step_us=100,
                    example="120ms")
    if (fast or 12) >= (slow or 1200):
        v.err(p, d.line_of("velocity"), f"fast ({(fast or 12) / 10:g}ms) must be shorter than slow "
                                        f"({(slow or 1200) / 10:g}ms)")
        return None, None
    return fast, slow


# ---------------------------------------------------------------------------------------------
# encoder acceleration (core/README.md "Encoder acceleration")

# A turn whose detents come at most `slow` apart moves more than one step per detent: up to `max` steps
# at `fast` apart or less, along `curve` in between. Values are (slow ms, fast ms, max, curve index);
# None is off (1 step per detent, the C table's { 0, 0, 1, linear }).
ACCEL_CURVES = ["linear", "quadratic", "cubic"]     # gm_device.h GM_ACCEL_*
ACCEL_PRESETS = {"off": None,
                 "low": (60, 15, 4, 0),
                 "medium": (80, 10, 8, 1),
                 "high": (100, 8, 16, 1)}
ACCEL_KEYS = ["slow", "fast", "max", "curve"]
ACCEL_SLOW_MS = (2, 1000)
ACCEL_FAST_MS = (1, 999)
ACCEL_MAX = (1, 32)
ENCODER_SETTINGS_KEYS = ["acceleration"]


def accel_text(a):
    """(slow, fast, max, curve) -> the preset's name, or 'slow 80ms, fast 10ms, max 8, quadratic'."""
    for name, p in ACCEL_PRESETS.items():
        if p == a:
            return name
    return f"slow {a[0]}ms, fast {a[1]}ms, max {a[2]}, {ACCEL_CURVES[a[3]]}"


def validate_acceleration(v, d, path, base):
    """`acceleration:` in d (an encoder, or encoder_settings:): off / low / medium / high, or a mapping
    { slow, fast, max, curve } whose missing keys come from base (a preset tuple; medium when base is off).
    Returns (given, value): value is a tuple or None (off); given False when d has no acceleration:."""
    if d.get("acceleration") is None:
        return False, base
    x, p, line = d["acceleration"], f"{path}.acceleration" if path else "acceleration", d.line_of("acceleration")
    names = list(ACCEL_PRESETS)
    if x is False:                  # bare `off`: YAML 1.1 reads it as false
        return True, None
    if isinstance(x, str):
        if x in ACCEL_PRESETS:
            return True, ACCEL_PRESETS[x]
        close = difflib.get_close_matches(x, names, 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(p, line, f"unknown acceleration '{x}' ({hint}valid: {', '.join(names)}, or a mapping "
                       "{ slow, fast, max, curve })")
        return True, base
    if not isinstance(x, dict):
        v.err(p, line, f"must be {', '.join(names)} or a mapping {{ slow, fast, max, curve }} (got "
                       f"{type_name(x)} {d.raw_of('acceleration')!r})")
        return True, base
    m = v.mapping(x, p, line, ACCEL_KEYS)
    slow, fast, mx, curve = base or ACCEL_PRESETS["medium"]
    slow = v.period(m, "slow", p, ACCEL_SLOW_MS[0] * 1000, ACCEL_SLOW_MS[1] * 1000, default=slow,
                    step_us=1000, example="80ms")
    fast = v.period(m, "fast", p, ACCEL_FAST_MS[0] * 1000, ACCEL_FAST_MS[1] * 1000, default=fast,
                    step_us=1000, example="10ms")
    mx = v.integer(m, "max", p, *ACCEL_MAX, default=mx)
    if m.get("curve") is not None:
        c = enum_value(v, m, "curve", f"{p}.curve", ACCEL_CURVES)
        curve = curve if c is None else c
    if fast >= slow:
        v.err(p, line, f"fast ({fast}ms) must be shorter than slow ({slow}ms): detents fast apart get max "
                       "steps, slow apart 1")
        return True, base
    if mx == 1:
        v.warn(p, m.line_of("max"), "max 1: one step per detent at any speed (the same as off)")
        return True, None
    return True, (slow, fast, mx, curve)


def encoder_settings(v, dev, data):
    """encoder_settings: what all encoders share unless they set their own: acceleration (off by default)."""
    if data.get("encoder_settings") is None:
        return None
    p, line = "encoder_settings", data.line_of("encoder_settings")
    s = v.mapping(data["encoder_settings"], p, line, ENCODER_SETTINGS_KEYS)
    if s is None:
        return None
    if not data.get("encoders"):
        v.warn(p, line, "there are no encoders: ignored")
    return validate_acceleration(v, s, p, None)[1]


# ---------------------------------------------------------------------------------------------
# pads (core/README.md "Pads")

# pad_settings: the pad.* param defaults (core/src/settings.c GM_PAD_*_DEFAULT). Readings are 0..4095,
# after inverted:; window and retrigger in ms.
PAD_DEFAULTS = {"channel": 0, "curve": 0, "aftertouch": 0, "threshold": 200, "release": 100, "full": 3000,
                "window": 3, "retrigger": 20}
PAD_KEYS = ["id", "name", "pin", "note", "led"]
PAD_SETTINGS_KEYS = list(PAD_DEFAULTS)
MAX_PADS = 128


def validate_pads(v, dev, data):
    """pads: one entry per pad, an analog source (ADC pin or mux channel) and its default note.
    pad_settings: what all pads share: the sensing (global) and channel / curve / aftertouch (per preset)."""
    names = {}
    pads = v.seq(data.get("pads"), "pads", data.line_of("pads"))
    for i, d in enumerate(pads):
        p = f"pads[{i}]"
        d = v.mapping(d, p, pads.line_of(i), PAD_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "pad", len(dev.pads))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "pad", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog", hubs=MUX_HUBS,
                    why="a pad is read with the ADC")
        note = v.integer(d, "note", p, 0, 127, default=0, required=True)
        led = -1
        if d.get("led") is not None:           # lit while the pad is held
            idx = v.ref(d, "led", f"{p}.led", d.line_of("led"), "LED", "LEDs")
            if idx is not None and idx > 127:
                v.err(f"{p}.led", d.line_of("led"), "LED index over 127 can't be referenced")
            elif idx is not None:
                led = idx
                dev.leds[led].used_by.append(("pad", None))
        dev.pads.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, note=note, led=led, path=p,
                            line=d.line_of("note") if "note" in d else d.line))
    if len(dev.pads) > MAX_PADS:
        v.err("pads", data.line_of("pads"), f"{len(dev.pads)} pads: at most {MAX_PADS}")
    seen = {}
    for x in dev.pads:
        if x.note in seen:
            v.warn(f"{x.path}.note", x.line, f"note {x.note} is also the note of {seen[x.note]}: both pads "
                                              "play the same note")
        seen.setdefault(x.note, x.path)

    if data.get("pad_settings") is None:
        return
    p, line = "pad_settings", data.line_of("pad_settings")
    s = v.mapping(data["pad_settings"], p, line, PAD_SETTINGS_KEYS)
    if s is None:
        return
    if not dev.pads:
        v.warn(p, line, "there are no pads: ignored")
    ps = dev.pad_set

    def put(key, val):
        if val is not None:
            ps[key] = val

    put("channel", v.integer(s, "channel", p, 0, 16))
    if "curve" in s:
        put("curve", enum_value(v, s, "curve", f"{p}.curve", CURVES))
    if "aftertouch" in s:
        put("aftertouch", int(v.boolean(s, "aftertouch", p)))
    put("threshold", v.integer(s, "threshold", p, 1, 4095))
    put("release", v.integer(s, "release", p, 0, 4094))
    put("full", v.integer(s, "full", p, 2, 4095))
    put("window", v.period(s, "window", p, 1000, 50_000, step_us=1000, example="3ms"))
    put("retrigger", v.period(s, "retrigger", p, 0, 500_000, step_us=1000, example="20ms"))
    if not ps["release"] < ps["threshold"] < ps["full"]:
        v.err(p, line, f"needs release < threshold < full (got release {ps['release']}, threshold "
                       f"{ps['threshold']}, full {ps['full']}; readings 0..4095, with the defaults "
                       f"{PAD_DEFAULTS['release']}, {PAD_DEFAULTS['threshold']}, {PAD_DEFAULTS['full']} "
                       "for those not given)")



DISPLAY_KEYS = ["id", "type", "chars", "segment_step", "segment_order", "shows", "daw",
                "ssd1306", "line", "x", "scale", "length"]
TEXT_KEYS = {"ssd1306", "line", "x", "scale", "length"}         # type text only
SEGMENT_KEYS = {"chars", "segment_step", "segment_order"}      # segment types only
# gm_display.h GM_DISPLAY_*: the C name, and (pins per character, what they are)
DISPLAY_TYPES = {"segment14": 1, "segment7": 2, "text": 3}
DISPLAY_C = {1: "GM_DISPLAY_SEG14", 2: "GM_DISPLAY_SEG7", 3: "GM_DISPLAY_TEXT"}
DISPLAY_TEXT = 3
TEXT_W, TEXT_H = 6, 8                # a text display's character cell, in pixels (x scale)
DISPLAY_SEGS = {1: (15, "segments a..m and the dot"), 2: (8, "segments a..g and the point")}
SEG14_PINS = 15                      # segments a..m, then the dot
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


def validate_displays(v, dev, data):
    """displays: segment displays on hub output pins. Each item of chars: is a character, left to
    right: its first pin (segment a; the others are the pins after it, or every segment_step pins),
    or the list of its segments' pins, in segment order."""
    lst = v.seq(data.get("displays"), "displays", data.line_of("displays"))
    for i, d in enumerate(lst):
        p = f"displays[{i}]"
        d = v.mapping(d, p, lst.line_of(i), DISPLAY_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "display", len(dev.displays))
        typ = d.get("type", "text" if d.get("ssd1306") is not None else "segment14")
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
                                                      "ssd1306:, line:, x: and scale:)" if t == DISPLAY_TEXT
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
        step = v.integer(d, "segment_step", p, 1, 64, default=1)
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
                v.err(cp, line, f"must be a hub pin, e.g. {{ tm1628: leds_a, number: 0 }}: the {nseg} pins "
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
    """A text display (type: text): one row of 5x7 characters on an ssd1306 hub, from pixel x of text
    line `line` (8 pixels a line, from the top), each character 6 x scale pixels wide and `scale` lines
    tall. Returns the display's fields, or None after an error."""
    if d.get("ssd1306") is None:
        v.err(p, d.line, "a text display needs ssd1306: the id of the OLED (an ssd1306: hub) it is on")
        return None
    hid = d["ssd1306"]
    hub = v.hub_by_id.get(hid) if isinstance(hid, str) else None
    if hub is None or hub.type != "ssd1306":
        known = [x.id for x in dev.hubs if x.type == "ssd1306"]
        what = f"'{hid}' is a {hub.type} hub" if hub is not None else f"no ssd1306 hub with id '{hid}'"
        v.err(f"{p}.ssd1306", d.line_of("ssd1306"), f"{what} (ssd1306: {', '.join(known) or 'none'})")
        return None
    hub.used = True
    lines = hub.height // TEXT_H
    scale = v.integer(d, "scale", p, 1, 4, default=1)
    line = v.integer(d, "line", p, 0, lines - 1, default=0)
    x = v.integer(d, "x", p, 0, hub.width - 1, default=0)
    if line + scale > lines:
        v.err(f"{p}.line", d.line_of("line") or d.line,
              f"line {line} at scale {scale} is {scale} line{'s' if scale > 1 else ''} tall: "
              f"'{hub.id}' has lines 0..{lines - 1} ({hub.height} pixels, 8 a line)")
        return None
    room = (hub.width - x) // (TEXT_W * scale)
    if room < 1:
        v.err(f"{p}.x", d.line_of("x"), f"no room for a character from x {x} at scale {scale} "
                                        f"({TEXT_W * scale} pixels; '{hub.id}' is {hub.width} wide)")
        return None
    n = v.integer(d, "length", p, 1, MAX_TEXT_CHARS, default=min(room, MAX_TEXT_CHARS))
    if n > room:
        v.err(f"{p}.length", d.line_of("length"), f"{n} characters at scale {scale} from x {x} are "
                                                  f"{n * TEXT_W * scale} pixels: '{hub.id}' has room for {room}")
        return None
    rect = (x, line, x + n * TEXT_W * scale, line + scale)
    for o in dev.displays:
        if o.type == DISPLAY_TEXT and o.hub_index == hub.index and \
                rect[0] < o.rect[2] and o.rect[0] < rect[2] and rect[1] < o.rect[3] and o.rect[1] < rect[3]:
            v.err(p, d.line, f"overlaps display '{o.id}' on '{hub.id}' (lines {o.rect[1]}..{o.rect[3] - 1}, "
                             f"pixels {o.rect[0]}..{o.rect[2] - 1})")
            return None
    at = Pin(hub.index + 1, 0, line * hub.width + x, f"{hub.id} line {line} x {x}")
    return dict(chars=[at], segs=None, n=n, scale=scale, hub_index=hub.index, rect=rect)


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
                c += [f"    {pn.c()}, /* {_comment(pn.desc)} */" for pn in run]
            c.append("};")
        else:
            c.append(f"static const gm_pin_t display_{i}_chars[{len(x.chars)}] = {{")
            c += [f"    {pn.c()}, /* {_comment(pn.desc)} */" for pn in x.chars]
            c.append("};")
    c.append("const gm_display_t gm_displays[GM_N_DISPLAYS] = {")
    for i, x in enumerate(dev.displays):
        where = f"0, display_{i}_map" if x.segs is not None else f"display_{i}_chars, 0"
        c.append(f"    {{ {DISPLAY_C[x.type]}, {x.n}, {where}, {DISPLAY_SHOWS_C[x.shows]}, {x.scale} }}, "
                 f"/* {_comment(x.id)} */")
    c += ["};", ""]
    return c

# ---------------------------------------------------------------------------------------------
# layout: where the controls are, for drawing the device (sysex.md 4.7, core/README.md "Layout")

LAYOUT_VERSION = 1
LAYOUT_KEYS = ["size", "keys", "controls"]
LAYOUT_ITEM_KEYS = ["x", "y", "w", "h", "shape"]
LAYOUT_MAX = 255                     # units: the blob's u8 fields
LAYOUT_SHAPES = {"button": 0, "round_button": 1, "knob": 2, "encoder": 3, "fader": 4, "wheel": 5, "pad": 6,
                 "display": 7, "keys": 8, "xy": 9}
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
        got = [v.integer(d, k, p, 0, LAYOUT_MAX, required=True) for k in "xy"]
        got += [v.integer(d, k, p, 1, LAYOUT_MAX, default=dflt) for k, dflt in zip("wh", default_wh)]
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
            v.err(p, line, f"'{cid}' is {a_(target[0])}: layout: places buttons, analog inputs, encoders, pads "
                           f"and displays{hint}")
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
        v.err(f"{p}.shape", line, f"{a_(kind)} can't be drawn as '{shape}' (use {' | '.join(allowed)})")
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
    c += [f"    {', '.join(map(str, layout_item_bytes(it)))}, /* {_comment(it.id)}: {names[it.shape]} */"
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
    revision, from presets.slots (default: the number of listed presets, at least 1)."""
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
    # An invalid slots: (reported above) allows every slot, so a preset: on a button or a mode isn't
    # reported as well; the build stops on the error anyway.
    dev.slots_invalid = ps.get("slots") is not None and slots is None
    dev.n_presets = MAX_PRESETS if dev.slots_invalid else min(slots or max(n_list, 1), MAX_PRESETS)
    dev.presets_revision = v.integer(ps, "revision", "presets", 0, PRESET_REVISION_MAX, default=0)


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
        program = v.integer(d, "program", p, 0, 127, default=-1)
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
        g = v.mapping(d[sec], sp, sl, list(keys), renamed=PRESET_NOT_HERE.get(sec))
        if g is None:
            continue
        for k, (param, default, kind, lo, hi) in keys.items():
            if k not in g:
                continue
            kp, kl = f"{sp}.{k}", g.line_of(k)
            if not any(n == param and c(dev) for n, _i, _s, c in PARAMS):
                why = ("it needs a velocity key matrix (key_matrix: { type: velocity })"
                       if param == "velocity.curve" else "the device has no pads"
                       if param.startswith("pad.") else "the device has no buttons")
                v.err(kp, kl, f"{param} doesn't exist on this device: {why}")
                continue
            if callable(default):
                default = default(dev)
            if kind == "int":
                val, shown = v.integer(g, k, sp, lo, hi), None
            elif kind == "bool":
                val = v.boolean(g, k, sp, default=None)
                val, shown = (None, None) if val is None else (int(val), str(val).lower())
            elif kind == "ms":
                val = v.period(g, k, sp, lo * 1000, hi * 1000, step_us=1000, example="80ms")
                shown = f"{val} ms"
            else:
                val = enum_value(v, g, k, kp, kind)
                shown = None if val is None else kind[val]
            put(param, 0, val, default, kp, kl, param, shown)

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
                if "action" in o or act != b.action_name:
                    put("button.action", idx, ACTION_IDS[act], b.action, f"{cp}.action", line("action"),
                        f"button.action[{cid}]", act)
                nk = next((k for k in (*NUMBER_KEYS, "mcu") if k in o), None)
                if nk is not None or num != b.number:
                    put("button.number", idx, num, b.number, f"{cp}.{nk or 'action'}", line(nk or "action"),
                        f"button.number[{cid}]")
            elif sec == "analog":
                a = dev.analogs[idx]
                m = a.mode
                if "mode" in o:
                    m = enum_value(v, o, "mode", f"{cp}.mode", ANALOG_MODES)
                    if m is not None:
                        mode_use(v, dev, ANALOG_MODES[m], f"{cp}.mode", line("mode"))
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
                    m = enum_value(v, o, "mode", f"{cp}.mode", ENC_MODES)
                    if m is not None:
                        mode_use(v, dev, ENC_MODES[m], f"{cp}.mode", line("mode"))
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

ANALOG_NUMBER_KEYS = ("cc", "nrpn", "rpn", "sysex")     # the keys that give analog.cc, by mode
ENCODER_NUMBER_KEYS = ("cc", "nrpn", "rpn")
MIDI_MESSAGES_KEYS = ["enable", "null_rpn", "mmc_device_id", "sysex"]
SYSEX_KEYS = ["id", "data"]
BANK_KEYS = ["msb", "lsb"]


def validate_midi_messages(v, dev, data):
    """midi_messages: { enable, null_rpn, mmc_device_id, sysex } (before the components: they refer to the
    SysEx strings by id)."""
    if data.get("midi_messages") is None:
        return
    mm = v.mapping(data["midi_messages"], "midi_messages", data.line_of("midi_messages"), MIDI_MESSAGES_KEYS)
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
        dev.null_rpn = bool(v.boolean(mm, "null_rpn", "midi_messages"))
        dev.null_rpn_line = mm.line_of("null_rpn")
    if "mmc_device_id" in mm:
        x = v.integer(mm, "mmc_device_id", "midi_messages", 0, 0x7F, default=0x7F)
        dev.mmc_id = 0x7F if x is None else x
        dev.mmc_line = mm.line_of("mmc_device_id")
    if mm.get("sysex") is not None:
        lst = v.seq(mm["sysex"], "midi_messages.sysex", mm.line_of("sysex"))
        for i, d in enumerate(lst):
            p = f"midi_messages.sysex[{i}]"
            d = v.mapping(d, p, lst.line_of(i), SYSEX_KEYS)
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


def sysex_wide(dev, k):
    return 0 <= k < len(dev.sysex) and bool(dev.sysex[k].flags & 1)


def mmc_command(v, d, path):
    """mmc: play | stop | ... | 1..127 -> the MMC command byte (button.number)."""
    x = d["mmc"]
    if isinstance(x, int) and not isinstance(x, bool):
        if 1 <= x <= 127:
            return x
        v.err(f"{path}.mmc", d.line_of("mmc"), f"{x} is out of range (1..127)")
        return 0
    if isinstance(x, str) and x in MMC_COMMANDS:
        return MMC_COMMANDS[x]
    close = difflib.get_close_matches(str(d.raw_of("mmc")), list(MMC_COMMANDS), 1)
    hint = f"did you mean '{close[0]}'? " if close else ""
    v.err(f"{path}.mmc", d.line_of("mmc"), f"unknown MMC command '{d.raw_of('mmc')}' ({hint}valid: "
                                           f"{', '.join(MMC_COMMANDS)}, or a number 1..127)")
    return 0


def mode_use(v, dev, mname, path, line):
    """An analog / encoder mode that needs a message kind: it is compiled in."""
    f = MODE_FEATURE.get(mname)
    if f is None:
        return
    if f == "sysex" and not dev.sysex:
        v.err(path, line, "mode sysex sends one of the midi_messages.sysex strings, and there are none")
    dev.msg_used.add(f)


def mode_wide(dev, mname, num):
    return mname in WIDE_MODES or (mname == "sysex" and sysex_wide(dev, num))


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


def button_bank(v, dev, d, p, act):
    """bank: 0..16383 (or { msb, lsb }) on a program action -> its Bank Select, or None."""
    if "bank" not in d:
        return None
    if act not in PROGRAM_ACTIONS:
        v.err(f"{p}.bank", d.line_of("bank"), f"bank: goes with the actions {', '.join(sorted(PROGRAM_ACTIONS))}, "
                                             f"not {act}")
        return None
    bank = bank_value(v, d, p)
    if bank is not None:
        dev.msg_used.add("program")
    return bank


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
    if dev.n_presets < 2:
        v.err(f"{p}.bank", d.line_of("bank"), "preset.bank needs 2 or more preset slots")
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
            rows.append(f"    {{ {lo}, {'GM_CTL_TOP' if hi == CTL_TOP else hi}, {ch} }}, /* {_comment(x.id)} */")
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
        c += [f"    {'GM_BANK_NONE' if getattr(b, 'bank', None) is None else b.bank}, /* {_comment(b.id)} */"
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
            c.append(f"static const uint8_t sysex_{k}[{len(x.data)}] = {{ {body} }}; /* {_comment(x.id)}: "
                     f"{_comment(x.text)} */")
        c.append("const gm_sysex_t gm_sysex[GM_N_SYSEX] = {")
        c += [f"    {{ sysex_{k}, {len(x.data)}, {x.flags} }}," for k, x in enumerate(dev.sysex)]
        c += ["};", ""]
    return c


# ---------------------------------------------------------------------------------------------
# write_to_flash: what the device stores on its own (core/README.md "Writes the device makes on its own")

# item -> its default (all off). A new item goes here, in WTF_KEYS, and in write_to_flash_defines.
WTF_ITEMS = {"last_preset": False}
WTF_KEYS = [*WTF_ITEMS, "delay", "idle"]
WTF_DELAY = (10_000, 1_000, 3_600_000, "1s..1h")    # ms: default, min, max, range text
WTF_IDLE = (3_000, 100, 600_000, "100ms..10min")


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
        if v.mapping(x, "write_to_flash", line, WTF_KEYS) is None:
            return
        for k, dflt in WTF_ITEMS.items():
            w[k] = v.boolean(x, k, "write_to_flash", default=dflt)
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
    if w["last_preset"] and dev.n_presets < 2:
        v.warn("write_to_flash.last_preset", line, "the device has one preset: nothing to remember (ignored)")
        w["last_preset"] = False


def write_to_flash_defines(dev):
    """GM_WRITE_*: what the device stores on its own (0: nothing; the core leaves that code out)."""
    w = getattr(dev, "write_to_flash", None) or dict(WTF_ITEMS, delay_ms=WTF_DELAY[0], idle_ms=WTF_IDLE[0])
    return ["", "/* write_to_flash: writes the device makes on its own (host saves always work) */",
            f"#define GM_WRITE_LAST_PRESET {int(w['last_preset'])} /* write_to_flash.last_preset */",
            f"#define GM_WRITE_DELAY_MS {w['delay_ms']}u /* write_to_flash.delay: after the last change */",
            f"#define GM_WRITE_IDLE_MS {w['idle_ms']}u /* write_to_flash.idle: and nothing played for this long */"]


def enum_value(v, d, key, path, options):
    x = d[key]
    if x in options:
        return options.index(x)
    close = difflib.get_close_matches(str(d.raw_of(key)), options, 1)
    hint = f"did you mean '{close[0]}'? " if close else ""
    v.err(path, d.line_of(key), f"unknown {key} '{d.raw_of(key)}' ({hint}valid: {', '.join(options)})")
    return None


def preset_ref(v, cid, path, line, kind, plural):
    """A component id used as a key in a preset -> its index, or None after saying why."""
    target = v.ids.get(cid) if isinstance(cid, str) else None
    if target is not None and target[0] == kind:
        return target[3]
    if target is not None:
        v.err(path, line, f"'{cid}' is the id of {target[1]} ({a_(target[0])}), not of {a_(kind)}")
        return None
    known = [i for i, t in v.ids.items() if t[0] == kind]
    close = difflib.get_close_matches(str(cid), known, 1)
    hint = f"; did you mean '{close[0]}'?" if close else ""
    v.err(path, line, f"no {kind} with id '{cid}' ({plural}: {', '.join(known) or 'none'}{hint})")
    return None


# ---------------------------------------------------------------------------------------------
# modes (research/core/presets-design.md §6, core/README.md "Modes")

# What a key can do in a mode: YAML action -> (gm_device.h GM_MK_* op, the key giving its number or None).
MODE_ACTIONS = {"preset_select": (1, "preset"), "preset_next": (2, None), "preset_prev": (3, None),
                "octave_down": (4, None), "octave_up": (5, None), "octave_reset": (6, None),
                "transpose_down": (7, None), "transpose_up": (8, None), "transpose_reset": (9, None),
                "channel": (10, "channel"), "velocity_curve": (11, "curve"),
                "channel_up": (13, None), "channel_down": (14, None)}
MK_MODE = 12                   # GM_MK_MODE: go to a sub-mode
MODE_KEYS = ["id", "toggle", "hold", "parent", "cycle", "led", "preset_blink", "keys", "buttons", "layer", "steps"]
STEP_KEYS = ["id", "name", "colour", "enabled", "layer"]     # cycle: steps (core/README.md "Cycles")
MAX_STEPS = 8
MODE_NAME_LEN = 16             # a step's name: on the display when it is switched to
# modes[].layer.<button, pad or encoder id>: what its twin is (core/README.md "Button layers")
LAYER_KEYS = ["name", "action", "cc", "note", "preset", "program", "mmc", "sysex", "daw", "idle", "active"]
LAYER_PAD_KEYS = ["name", "daw", "idle", "active"]
LAYER_ENCODER_KEYS = ["name", "cc", "daw"]
RANGE_KEYS = ("white_from", "from", "action", "count")      # the range form inside keys:
RANGE_ACTIONS = ("preset_select", "channel")
MAX_MODES = 64
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
NOTE_RE = re.compile(r"([A-Ga-g])([#b]?)(-?\d+)")
WHITE = {0, 2, 4, 5, 7, 9, 11}


def note_text(n):
    """MIDI note -> name, C4 = 60 (scientific pitch)."""
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def parse_note(x):
    """`E3`, `F#3`, `Db4` (C4 = 60) or a MIDI note number -> note, or None."""
    if isinstance(x, int) and not isinstance(x, bool):
        return x if 0 <= x <= 127 else None
    m = NOTE_RE.fullmatch(x.strip()) if isinstance(x, str) else None
    if not m:
        return None
    n = (int(m.group(3)) + 1) * 12 + [0, 2, 4, 5, 7, 9, 11]["CDEFGAB".index(m.group(1).upper())] + \
        {"#": 1, "b": -1, "": 0}[m.group(2)]
    return n if 0 <= n <= 127 else None


def mode_key_index(v, dev, x, path, line):
    """A key named in a mode (its note at octave 0, no transpose: the physical key) -> its matrix index."""
    m = dev.matrix
    lo, hi = m.first_note, m.first_note + m.keys - 1
    n = parse_note(x)
    if n is None:
        v.err(path, line, f"'{x}' isn't a key: name it by its note at octave 0 (C4 = 60), e.g. E3, F#3, Db4, "
                          f"or give the MIDI note number (this keyboard: {note_text(lo)}..{note_text(hi)})")
        return None
    if not lo <= n <= hi:
        v.err(path, line, f"{note_text(n)} (note {n}) isn't on the keyboard: its keys are "
                          f"{note_text(lo)}..{note_text(hi)} (notes {lo}..{hi} at octave 0, from "
                          "key_matrix.first_note)")
        return None
    return n - lo


def mode_action(v, dev, name, d, path, line, arg=None):
    """An action name (+ its number from d, or arg for a key of a range) -> (op, arg, text), or None."""
    if name not in MODE_ACTIONS:
        close = difflib.get_close_matches(str(name), list(MODE_ACTIONS), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(path, line, f"unknown action '{name}' ({hint}valid in a mode: {', '.join(MODE_ACTIONS)}; or "
                          "`{ mode: <id> }` to go to a sub-mode)")
        return None
    op, argkey = MODE_ACTIONS[name]
    if name.startswith("preset") and dev.n_presets < 2:
        v.err(path, line, f"action {name} switches presets, but this device has {dev.n_presets} preset slot: "
                          "add presets: (with 2 or more slots)")
        return None
    if name == "velocity_curve" and _vel(dev) == 0:
        v.err(path, line, "velocity_curve needs a velocity key matrix (key_matrix: { type: velocity })")
        return None
    extra = [k for k in (d or {}) if k not in ("action", argkey)]
    if extra:
        v.err(f"{path}.{extra[0]}", d.line_of(extra[0]), f"action {name} takes " +
              (f"{argkey}:" if argkey else "nothing else") + f", not {extra[0]}:")
        return None
    if argkey is None:
        return op, 0, name
    if arg is not None:
        return op, arg, f"{name} {arg}"
    if d is None or argkey not in d:
        need = {"preset": f"preset: (the slot, 1..{dev.n_presets})", "channel": "channel: (1..16)",
                "curve": f"curve: ({' | '.join(CURVES)})"}[argkey]
        example = {"preset": 2, "channel": 10, "curve": "hard"}[argkey]
        v.err(path, line, f"action {name} needs {need}, e.g. `{{ action: {name}, {argkey}: {example} }}`")
        return None
    if argkey == "curve":
        x = enum_value(v, d, "curve", f"{path}.curve", CURVES)
        return None if x is None else (op, x, f"{name} {CURVES[x]}")
    x = v.integer(d, argkey, path, 1, dev.n_presets if argkey == "preset" else 16)
    return None if x is None else (op, x, f"{name} {x}")


def mode_led_ref(v, d, path):
    """A mode's led: an LED id (lit while the mode is active) or { <LED id>: blink } -> (index, blink)."""
    x, line = d["led"], d.line_of("led")
    if isinstance(x, dict):
        if len(x) != 1:
            v.err(path, line, "must be an LED id (lit while the mode is active) or `{ <LED id>: blink }`")
            return None
        lid, how = next(iter(x.items()))
        if how not in ("blink", True):
            v.err(f"{path}.{lid}", x.line_of(lid), f"must be blink or on (got {x.raw_of(lid)!r})")
            return None
        holder = LDict(led=lid)
        holder.line = line
        idx = v.ref(holder, "led", path, line, "LED", "LEDs")
        return None if idx is None else (idx, how == "blink")
    idx = v.ref(d, "led", path, line, "LED", "LEDs")
    return None if idx is None else (idx, False)


# ---- safety (core/README.md "Factory-reset combo", "Physical unlock", "Status LED") ----
RESET_HOLD_DEFAULT_MS = 5000
UNLOCK_HOLD_DEFAULT_MS = 2000
COMBO_MAX = 8            # buttons in a combo (sysex.md 4.9 UNLOCK `n`)


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


def combo_and_hold(v, dev, data, key, lo_ms, hi_ms, default_ms, example):
    """`key: [ids]` or `key: { combo: [ids], hold: 2s }` -> (indices or None if not given, hold ms,
    combo given). combo may be left out of the mapping (None then)."""
    x, line = data[key], data.line_of(key)
    if isinstance(x, list):
        return combo_buttons(v, dev, x, key, line), default_ms, True
    d = v.mapping(x, key, line, COMBO_KEYS)
    if d is None:
        return [], default_ms, True
    hold = v.period(d, "hold", key, lo_ms * 1000, hi_ms * 1000, step_us=1000, example=example)
    hold = default_ms if hold is None else hold
    if d.get("combo") is None:
        return None, hold, False
    return combo_buttons(v, dev, d["combo"], f"{key}.combo", d.line_of("combo")), hold, True


def validate_safety(v, dev, data):
    """factory_reset_combo:, unlock: and status_led: (sysex.md 4.9; core/README.md)."""
    boot = set(dev.boot_combo)
    if data.get("factory_reset_combo") is not None:
        line = data.line_of("factory_reset_combo")
        combo, dev.reset_hold_ms, given = combo_and_hold(v, dev, data, "factory_reset_combo", 1000, 30000,
                                                          RESET_HOLD_DEFAULT_MS, "5s")
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
            combo, dev.unlock_hold_ms, given = combo_and_hold(v, dev, data, "unlock", 0, 10000,
                                                             UNLOCK_HOLD_DEFAULT_MS, "2s")
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


def combo_mask(ix):
    """Button indices -> a C mask constant, 32-bit when it fits."""
    m = sum(1 << i for i in ix)
    return f"0x{m:X}u" if m < (1 << 32) else f"0x{m:X}ull"


def validate_modes(v, dev, data):
    """modes: -> dev.modes, one Hub per mode: toggle (button index) or parent (mode index), root, led,
    blink, show (preset_blink LEDs) and keys = sorted [(key index, op, arg, comment)]."""
    if data.get("modes") is None:
        return
    lst = v.seq(data["modes"], "modes", data.line_of("modes"))
    if not lst:
        return
    if len(lst) > MAX_MODES:
        v.err("modes", data.line_of("modes"), f"{len(lst)} modes: at most {MAX_MODES}")
        return
    n_errors = len(errors(v.diags))
    # pass 1: ids, toggles and parents; a cycle: becomes its steps, one root mode each
    modes, by_id, cycles, seen = [], {}, [], {}

    def new_id(d, p, what):
        if "id" not in d:
            v.err(p, d.line, f"needs id: (e.g. `id: {what}`)")
            return None
        mid = d["id"]
        why = id_problem(mid, d.raw_of("id"))
        if why:
            v.err(f"{p}.id", d.line_of("id"), why)
            return None
        if mid in seen:
            v.err(f"{p}.id", d.line_of("id"), f"mode '{mid}' is already {seen[mid]}: mode ids (and cycle "
                                              "and step ids) are unique")
            return None
        seen[mid] = p
        return mid

    def new_mode(mid, p, d):
        m = Hub(id=mid, path=p, line=d.line, d=d, index=len(modes), toggle=None, hold=False, parent=None,
                root=None, led=None, blink=False, show=None, keys=[], buttons=[], with_=None, twins=0,
                cycle=0, colour=0, rgb=None, name=None, enabled=True, step=False, modifier=False)
        modes.append(m)
        by_id[mid] = m
        return m

    for i, d in enumerate(lst):
        p = f"modes[{i}]"
        d = v.mapping(d, p, lst.line_of(i), MODE_KEYS)
        if d is None:
            continue
        mid = new_id(d, p, "edit; keys go to a sub-mode by its id")
        if mid is None:
            continue
        given = [k for k in ("toggle", "hold", "parent", "cycle") if d.get(k) is not None]
        if "cycle" in given or d.get("steps") is not None:
            mode_cycle(v, dev, d, p, mid, given, cycles, new_id, new_mode)
            continue
        m = new_mode(mid, p, d)
        if len(given) != 1:
            v.err(p, d.line, f"mode '{mid}' needs " + (
                f"one of toggle:, hold: or parent:, not {' and '.join(given)}" if given else
                "toggle: { button: <id> } (the button that enters and leaves it), hold: { button: <id> } "
                "(active while that button is held), cycle: { button: <id> } with steps: (layers in turn) "
                "or parent: <mode id> (a sub-mode, entered from a key of another mode)"))
        elif given[0] != "parent":
            m.hold = given[0] == "hold"
            mode_toggle(v, dev, m, d, p, given[0])
    dev.n_cycles = len(cycles)
    for m in modes:
        d = m.d
        if d.get("parent") is None or d.get("toggle") is not None or d.get("hold") is not None:
            continue
        pid, pl = d["parent"], d.line_of("parent")
        if pid not in by_id:
            close = difflib.get_close_matches(str(pid), list(by_id), 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            v.err(f"{m.path}.parent", pl, f"no mode with id '{d.raw_of('parent')}' (modes: "
                                          f"{', '.join(by_id)}{hint})")
        elif pid == m.id:
            v.err(f"{m.path}.parent", pl, "a mode can't be its own parent")
        else:
            m.parent = by_id[pid].index
    for m in modes:                  # the root: follow the parents (and catch loops)
        seen, x = [m.index], m
        while x.parent is not None and x.parent not in seen:
            x = modes[x.parent]
            seen.append(x.index)
        if x.parent is not None:
            v.err(f"{m.path}.parent", m.d.line_of("parent"), "the parents go round in a loop: " +
                  " -> ".join(modes[i].id for i in seen + [x.parent]))
        m.root = x.index
        if x.parent is None and x.hold and m is not x:
            v.err(f"{m.path}.parent", m.d.line_of("parent"), f"mode '{x.id}' is a hold mode (active while its "
                                                            "button is held): it can't have sub-modes")
    # pass 2: LEDs, keys and buttons (a key may name a sub-mode further down the file)
    for m in modes:
        d, p = m.d, m.path
        if d.get("led") is not None:
            r = mode_led_ref(v, d, f"{p}.led")
            if r:
                m.led, m.blink = r
        if d.get("preset_blink") is not None:
            mode_show(v, dev, m, d, p)
        keys = d.get("keys")
        if d.get("buttons") is not None:
            mode_buttons(v, dev, m, d["buttons"], f"{p}.buttons", d.line_of("buttons"))
        if d.get("layer") is not None:
            mode_layer(v, dev, m, d["layer"], f"{p}.layer", d.line_of("layer"))
        if keys is None:
            if d.get("buttons") is None and d.get("layer") is None and not m.step:
                v.warn(p, d.line, f"mode '{m.id}' has no keys, buttons or layer: it does nothing")
        elif not isinstance(keys, dict):
            v.err(f"{p}.keys", d.line_of("keys"), "must be a mapping of keys to what they do, e.g. "
                                                   "`keys: { C3: octave_reset, E3: { mode: pick } }`")
        elif dev.matrix is None:
            v.err(f"{p}.keys", d.line_of("keys"), "keys: turns the keyboard's keys into function keys, and "
                                                   "this device has no key_matrix: bind buttons instead "
                                                   "(`buttons: { up: preset_next }`), or give the buttons a "
                                                   "layer:")
        else:
            mode_keys(v, dev, m, keys, f"{p}.keys", by_id)
    targets = {k[2] for m in modes for k in m.keys if k[1] == MK_MODE}
    for m in modes:
        if m.parent is not None and m.index not in targets:
            if dev.matrix is None:
                v.err(f"{m.path}.parent", m.d.line_of("parent"), f"mode '{m.id}' is a sub-mode, entered from a "
                      "key of its parent's keys:, and this device has no key_matrix: make it a root mode "
                      "(toggle:, hold: or cycle:)")
            else:
                v.err(m.path, m.line, f"mode '{m.id}' can't be reached: no key goes to it (add e.g. "
                                      f"`E3: {{ mode: {m.id} }}` to the keys of mode '{modes[m.parent].id}')")
    if len(errors(v.diags)) > n_errors:
        return
    # defaults: a root lights its toggle button's LED, a sub-mode blinks its parent's; preset_blink is
    # inherited from the parent
    for m in modes:
        x = m
        while m.parent is not None and x.parent is not None and (m.led is None or m.show is None):
            x = modes[x.parent]
            if m.led is None and x.led is not None:
                m.led, m.blink = x.led, True
            if m.show is None:
                m.show = x.show
        root = modes[m.root]
        if m.led is None and root.toggle is not None and dev.buttons[root.toggle].led >= 0:
            m.led, m.blink = dev.buttons[root.toggle].led, m is not root
        m.keys.sort()
        m.buttons.sort()
    dev.modes = modes


def mode_cycle(v, dev, d, p, cid, given, cycles, new_id, new_mode):
    """cycle: { button, with } + steps: (core/README.md "Cycles"): each press moves to the next enabled
    step. Each step is a root mode of its own (its own index, layer and LED colour), all with the cycle's
    button as toggle / with and cycle = the group number + 1."""
    if given != ["cycle"] or d.get("steps") is None:
        others = [k for k in given if k != "cycle"]
        v.err(p, d.line, f"cycle '{cid}' needs " + (
            f"cycle: and steps: only, not {' and '.join(others)}:" if others else
            "cycle: { button: <id>, with: <id> } (the button that moves to the next step)" if "cycle" not in given
            else "steps: (2 to 8 of { id, name, colour, enabled, layer })"))
        return
    for k in ("hold", "parent", "keys", "buttons", "layer", "preset_blink"):
        if d.get(k) is not None:
            v.err(f"{p}.{k}", d.line_of(k), f"a cycle has no {k}: of its own: give each step its layer:")
            return
    cyc = Hub(id=cid, path=p, toggle=None, with_=None)
    mode_toggle(v, dev, cyc, d, p, "cycle")
    led = None
    if d.get("led") is not None:
        r = mode_led_ref(v, d, f"{p}.led")
        if r:
            led = r[0]
    elif cyc.toggle is not None and dev.buttons[cyc.toggle].led >= 0:
        led = dev.buttons[cyc.toggle].led
    steps, sl = d["steps"], d.line_of("steps")
    lst = v.seq(steps, f"{p}.steps", sl)
    if isinstance(steps, list) and not 2 <= len(lst) <= MAX_STEPS:
        v.err(f"{p}.steps", sl, f"{len(lst)} step{'s' if len(lst) != 1 else ''}: a cycle has 2 to {MAX_STEPS}")
        return
    group = len(cycles) + 1
    out = []
    for j, s in enumerate(lst):
        sp = f"{p}.steps[{j}]"
        s = v.mapping(s, sp, lst.line_of(j), STEP_KEYS)
        if s is None:
            continue
        sid = new_id(s, sp, "enc_midi")
        if sid is None:
            continue
        m = new_mode(sid, sp, s)
        m.step, m.cycle, m.root = True, group, m.index
        m.toggle, m.with_, m.led = cyc.toggle, cyc.with_, led
        m.enabled = v.boolean(s, "enabled", sp, default=True)
        name = v.string(s, "name", sp)
        if name is not None and (len(name) > MODE_NAME_LEN or any(not 32 <= ord(c) <= 126 for c in name)):
            v.err(f"{sp}.name", s.line_of("name"), f"'{name}': at most {MODE_NAME_LEN} ASCII characters (the display "
                                                   "shows as much as fits)")
        m.name = name
        if s.get("colour") is not None:
            m.colour, m.rgb = step_colour(v, dev, s, sp, led)
            if m.rgb is not None and m.index >= MODE_RGB_MAX:
                v.warn(f"{sp}.colour", s.line_of("colour"), f"mode {m.index}: only the first {MODE_RGB_MAX} modes "
                                                            "show a step's full colour; this one shows the nearest "
                                                            "on/off colour")
                m.rgb = None
        out.append(m)
    if out and not any(m.enabled for m in out):
        v.err(f"{p}.steps", sl, f"cycle '{cid}': every step has enabled: false; at least one must be enabled")
    cycles.append(cyc)


def step_colour(v, dev, s, sp, led):
    """A step's colour: an LED look for the cycle's LED, resolved for its kind like led_defaults ->
    (0x80 | pin mask (gm_mode_t.colour), 0 after an error; the full colour (gm_mode_rgb[]) or None). A full
    colour ("#rrggbb", "blue 20%") shows as it is on an LED that can show any colour, elsewhere as the
    nearest on/off colour."""
    cp, cl = f"{sp}.colour", s.line_of("colour")
    if led is None:
        v.err(cp, cl, "the cycle has no LED to show it: add led: <LED id> to the cycle (or give its button an LED)")
        return 0, None
    w = look_word(s, "colour")
    x = dev.leds[led]
    kind = led_kind(x.pin2, x.pin3)
    full = led_full_colour(dev, x.pin, x.pin2, x.pin3)
    col = colour_look(w) if w not in LED_LOOKS[kind] and w not in LED_LOOK_NAMES else None
    if w not in LED_LOOKS[kind] and w not in LED_LOOK_NAMES and col is None:
        close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(cp, cl, f"unknown colour '{w}' for LED '{x.id}' ({hint}valid: {', '.join(LED_LOOKS[kind])}, "
                      "or a full colour: \"#rrggbb\", blue 20%)")
        return 0, None
    if col is not None and not (full and kind == "rgb") and not colour_exact(kind, col):
        v.warn(cp, cl, f"LED '{x.id}' ({kind}, on/off) can't show {w}: it shows the nearest it can (full colours "
                       f"need an LED that can show any colour: one LED of a {' / '.join(FULL_COLOUR_HUBS)} hub)")
    elif col is None and w not in LED_LOOKS[kind]:
        v.warn(cp, cl, f"LED '{x.id}' ({kind}) can't show {w}: it shows the nearest it can (valid: "
                       f"{', '.join(LED_LOOKS[kind])})")
    mask, rgb = resolve_led_default(kind, w, full)
    return 0x80 | mask, rgb


def mode_toggle(v, dev, m, d, p, key="toggle"):
    """toggle: { button: <id> } (or toggle: <id>): the button that enters and leaves a root mode;
    hold: the same form, the button that keeps a hold mode active while it is held; cycle: the same as
    toggle:, the button that moves a cycle to its next step."""
    t, tl = d[key], d.line_of(key)
    if isinstance(t, dict):
        if v.mapping(t, f"{p}.{key}", tl, ["button", "with"] if key in ("toggle", "cycle") else ["button"]) is None:
            return
        if "button" not in t:
            v.err(f"{p}.{key}", tl, f"needs button: (e.g. `{key}: {{ button: edit }}`)")
            return
        holder = t
    else:
        holder = LDict(button=t)
        holder.line = tl
    idx = v.ref(holder, "button", f"{p}.{key}.button", holder.line_of("button"), "button", "buttons")
    if idx is None:
        return
    b = dev.buttons[idx]
    verb = {"toggle": "toggles", "hold": "holds", "cycle": "cycles"}[key]
    if holder.get("with") is not None:
        # toggle: { button, with }: the mode only with `with` held; a plain press keeps the button's action
        w = v.ref(holder, "with", f"{p}.{key}.with", holder.line_of("with"), "button", "buttons")
        if w is None:
            return
        if w == idx:
            v.err(f"{p}.{key}.with", holder.line_of("with"), "the button can't be held with itself")
        elif idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers:
            v.err(f"{p}.{key}", tl, f"button '{b.id}' already enters mode "
                                    f"'{v.mode_toggles.get(idx) or v.mode_with_toggles.get(idx) or v.mode_modifiers[idx]}'")
        elif w in v.mode_toggles:
            v.err(f"{p}.{key}.with", holder.line_of("with"), f"button '{dev.buttons[w].id}' enters mode "
                                                             f"'{v.mode_toggles[w]}' itself")
        else:
            v.mode_with_toggles[idx] = m.id
            v.mode_with_of[idx] = w
            v.mode_with_buttons.setdefault(w, m.id)
            m.toggle, m.with_ = idx, w
        return
    if idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers:
        v.err(f"{p}.{key}", tl, f"button '{b.id}' already enters mode "
                                f"'{v.mode_toggles.get(idx) or v.mode_with_toggles.get(idx) or v.mode_modifiers[idx]}'")
    elif key == "hold" and d.get("layer") is not None and d.get("keys") is None and d.get("buttons") is None:
        # a hold mode with only a layer: is a modifier (Shift): its button keeps its own action, and
        # while it is held the layer's twins take their buttons' presses (core/README.md "Button layers")
        v.mode_modifiers[idx] = m.id
        m.toggle, m.modifier = idx, True
    elif b.action_name != "none":
        v.err(f"{p}.{key}", tl, f"button '{b.id}' {verb} this mode, so its own action would never run: remove "
                                f"`action: {b.action_name}` (and its cc:/note:) from the button")
    else:
        v.mode_toggles[idx] = m.id
        m.toggle = idx


def mode_buttons(v, dev, m, btns, bp, bl):
    """buttons: { <button id>: <action> }: what buttons do instead of their own action while the mode is
    active (the actions of keys:, but no sub-modes or ranges) -> m.buttons."""
    if not isinstance(btns, dict) or not btns:
        v.err(bp, bl, "must be a mapping of button ids to what they do in the mode, e.g. "
                      "`buttons: { up: channel_up, down: channel_down }`")
        return
    for bid in btns:
        path, line = f"{bp}.{bid}", btns.line_of(bid)
        holder = LDict(button=bid)
        holder.line = line
        idx = v.ref(holder, "button", path, line, "button", "buttons")
        if idx is None:
            continue
        if idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers or idx == m.toggle:
            v.err(path, line, f"button '{bid}' enters a mode (toggle:/hold:): it can't also be bound in one")
            continue
        b = btns[bid]
        r = None
        if isinstance(b, str):
            r = mode_action(v, dev, b, None, path, line)
        elif isinstance(b, dict) and "action" in b:
            r = mode_action(v, dev, b["action"], b, path, line)
        else:
            v.err(path, line, "must be an action (`channel_up`, `preset_next`, `{ action: channel, channel: 10 }`, "
                              "...); buttons can't go to a sub-mode")
        if r:
            m.buttons.append((idx, r[0], r[1], f"{dev.buttons[idx].id}: {r[2]}"))


def mode_layer(v, dev, m, layer, lp, ll):
    """layer: { <button, pad or encoder id>: { ... } } (core/README.md "Button layers"): while the mode is
    active those are their twins. A button twin (action/cc/note/preset, daw, name, idle, active) is appended
    to dev.buttons, a pad twin (daw, name, idle, active) to dev.pad_twins, an encoder twin (cc, daw, name) to
    dev.encoders; a button's or pad's LED gets a twin LED in dev.leds: its pins with the twin's looks.
    What a twin leaves out is its base's."""
    if m.parent is not None:
        v.err(lp, ll, "only a root mode (toggle: or hold:) has a layer: a sub-mode lasts one action")
        return
    if not isinstance(layer, dict) or not layer:
        v.err(lp, ll, "must be a mapping of button, pad or encoder ids to what they are in the layer, e.g. "
                      "`layer: { b2: { cc: 52, daw: arm_1, active: red } }`")
        return
    bound = {k[0] for k in m.buttons}
    for bid in layer:
        path, line = f"{lp}.{bid}", layer.line_of(bid)
        target = v.ids.get(bid) if isinstance(bid, str) else None
        kind = target[0] if target else None
        if kind in ("pad twin",) or (kind in ("button", "encoder") and
                                     getattr((dev.buttons if kind == "button" else dev.encoders)[target[3]],
                                             "layer", 0)):
            v.err(path, line, f"'{bid}' is a layer twin: give the layer the {kind.split()[0]} itself")
            continue
        if kind not in ("button", "pad", "encoder"):
            known = [i for i, t in v.ids.items() if t[0] in ("button", "pad", "encoder")]
            close = difflib.get_close_matches(str(bid), known, 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            what = f"is the id of {target[1]} ({a_(kind)})" if target else "matches nothing"
            v.err(path, line, f"no button, pad or encoder with id '{bid}': it {what}{hint}")
            continue
        idx = target[3]
        e = layer[bid]
        if e is None:
            e = LDict()
            e.line = line
        e = v.mapping(e, path, line, {"button": LAYER_KEYS, "pad": LAYER_PAD_KEYS,
                                      "encoder": LAYER_ENCODER_KEYS}[kind],
                      renamed={"acceleration": "acceleration: is the physical encoder's, and its twins share it: "
                                               "set it on the encoder"} if kind == "encoder" else None)
        if e is None:
            continue
        if kind == "pad":
            layer_pad(v, dev, m, idx, e, path, line)
        elif kind == "encoder":
            layer_encoder(v, dev, m, idx, e, path, line)
        else:
            b = dev.buttons[idx]
            if (idx == m.with_ or idx in v.mode_toggles or idx in v.mode_with_buttons or
                    idx in v.mode_modifiers or (idx == m.toggle and m.with_ is None)):
                v.err(path, line, f"button '{bid}' enters a mode (or is held for one): it can't have a twin")
                continue
            if m.modifier and v.mode_with_of.get(idx) == m.toggle:
                v.err(path, line, f"{dev.buttons[m.toggle].id} + {bid} already toggles mode "
                                  f"'{v.mode_with_toggles[idx]}', so this twin's action would never run")
                continue
            if idx in bound:
                v.err(path, line, f"button '{bid}' is also in this mode's buttons:; give it one of the two")
                continue
            act, num = button_action(v, dev, e, path, base=(b.action_name, b.number))
            tid = v.register(f"{b.id}_{m.id}", path, line, "button", len(dev.buttons))
            if tid is None:
                continue
            led = layer_led(v, dev, m, b.led, e, path, line, ("button", act), f"button '{bid}'")
            if led is None:
                continue
            name = v.string(e, "name", path)
            dev.buttons.append(Hub(id=tid, name=name if name is not None else f"{b.name} {m.id}", pin=b.pin,
                                   action=ACTION_IDS[act], action_name=act, number=num, led=led,
                                   daw=daw_role(v, dev, e, path), layer=m.index + 1, base=idx,
                                   ctl=b.ctl, bank=b.bank))
        m.twins += 1


def layer_led(v, dev, m, base_led, e, path, line, use, what):
    """The twin LED of LED base_led (-1: none) for a layer twin -> its index, -1 for none, None after an
    error."""
    if base_led < 0:
        if e.get("idle") is not None or e.get("active") is not None:
            v.warn(path, line, f"{what} has no LED (led:): idle: / active: do nothing")
        return -1
    bl = dev.leds[base_led]
    led = len(dev.leds)
    if led > 127:
        v.err(path, line, "LED index over 127 can't be referenced: too many LEDs for another twin")
        return None
    looks, rgb = layer_led_looks(v, e, path, led_kind(bl.pin2, bl.pin3), bl.looks, getattr(bl, "rgb", None),
                                 led_full_colour(dev, bl.pin, bl.pin2, bl.pin3))
    lid = v.register(f"{bl.id}_{m.id}", path, line, "LED", led) or f"{bl.id}_{m.id}"
    dev.leds.append(Hub(id=lid, name=f"{bl.name} {m.id}", pin=bl.pin, pin2=bl.pin2, pin3=bl.pin3,
                        looks=looks, rgb=rgb, source=None, owner=bl.owner, timeout=bl.timeout, path=path,
                        line=line, used_by=[use], daw=None, local_line=None,
                        layer=m.index + 1, base=base_led))
    return led


def layer_pad(v, dev, m, idx, e, path, line):
    """A pad's twin: hits go to it (an EVENT, no note) while the layer is active."""
    x = dev.pads[idx]
    tid = v.register(f"{x.id}_{m.id}", path, line, "pad twin", len(dev.pad_twins))
    if tid is None:
        return
    led = layer_led(v, dev, m, x.led, e, path, line, ("pad", None), f"pad '{x.id}'")
    if led is None:
        return
    name = v.string(e, "name", path)
    dev.pad_twins.append(Hub(id=tid, name=name if name is not None else f"{x.name} {m.id}", led=led,
                             daw=daw_role(v, dev, e, path), layer=m.index + 1, base=idx))


def layer_encoder(v, dev, m, idx, e, path, line):
    """An encoder's twin: turns go to it (its CC, its EVENT index) while the layer is active. Same pins, not
    scanned again; layer.encoder.mode starts as absolute. cc: none sends no CC, only EVENTs."""
    x = dev.encoders[idx]
    if x.ring or x.ring_center is not None:
        v.err(path, line, "a ring encoder can't have a layer twin yet")
        return
    if isinstance(e.get("cc"), str) and e["cc"].strip().lower() == "none":
        cc = -1                  # cc: none: only EVENTs (layer.encoder.cc -1)
    else:
        cc = v.integer(e, "cc", path, 0, 127, default=x.cc)
    tid = v.register(f"{x.id}_{m.id}", path, line, "encoder", len(dev.encoders))
    if tid is None:
        return
    name = v.string(e, "name", path)
    dev.encoders.append(Hub(id=tid, name=name if name is not None else f"{x.name} {m.id}", a=x.a, b=x.b,
                            cc=cc, steps=x.steps, mcu=None, ring=[], ring_center=None,
                            daw=daw_role(v, dev, e, path), layer=m.index + 1, base=idx, mode=0, ctl=x.ctl))


def layer_led_looks(v, e, p, kind, base_looks, base_rgb=None, full=False):
    """A twin LED's ((idle, active), full colours): the button LED's, with the twin's idle: / active: in
    their place."""
    looks = list(base_looks or LED_DEFAULT_LOOK[kind])
    rgb = list(base_rgb or (None, None))
    given = False
    for k, key in enumerate(("idle", "active")):
        if e.get(key) is None:
            continue
        w = look_word(e, key)
        col = colour_look(w) if kind == "rgb" and w not in LED_LOOKS[kind] else None
        if col is not None:
            if not full:
                v.err(f"{p}.{key}", e.line_of(key), f"'{w}' {COLOUR_LOOK_NEEDS}")
                continue
            looks[k], rgb[k] = colour_mask(col), col
            given = True
            continue
        if w not in LED_LOOKS[kind]:
            close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.{key}", e.line_of(key), f"unknown look '{w}' for this LED ({hint}valid: "
                                                f"{', '.join(LED_LOOKS[kind])})")
            continue
        looks[k], rgb[k] = LED_LOOKS[kind][w], None
        given = True
    if not given:
        return base_looks, base_rgb
    if looks[0] == looks[1] and rgb[0] == rgb[1]:
        v.warn(p, e.line, "idle and active look the same: the LED never shows its state")
    return tuple(looks), (tuple(rgb) if any(c is not None for c in rgb) else None)


def mode_show(v, dev, m, d, p):
    """preset_blink: [count LED, done LED]: after a preset switch from the mode, the first blinks the
    slot number, then both flash once."""
    pb, pbl = d["preset_blink"], d.line_of("preset_blink")
    items = v.seq(pb, f"{p}.preset_blink", pbl)
    if not 1 <= len(items) <= 2:
        if isinstance(pb, list):
            v.err(f"{p}.preset_blink", pbl, "needs one or two LED ids: the first blinks the preset number, then "
                                            "both flash once (e.g. [oct_down_led, oct_up_led])")
        return
    idx = [v.ref(items, k, f"{p}.preset_blink[{k}]", items.line_of(k), "LED", "LEDs") for k in range(len(items))]
    if None in idx:
        return
    m.show = (idx[0], idx[-1] if len(idx) > 1 else -1)
    if dev.n_presets < 2:
        v.warn(f"{p}.preset_blink", pbl, "only shown after a preset switch, and this device has 1 preset slot")


def mode_keys(v, dev, m, keys, kp, by_id):
    """The keys: mapping of one mode -> m.keys."""
    first, top = dev.matrix.first_note, dev.matrix.first_note + dev.matrix.keys - 1
    bound = {}

    def bind(k, op, arg, text, line):
        name = note_text(first + k)
        if k in bound:
            v.err(kp, line, f"{name} is bound twice in this mode ({bound[k]}, and {text})")
            return
        bound[k] = f"{text} at line {line}"
        m.keys.append((k, op, arg, f"{name}: {text}"))

    if any(k in keys for k in RANGE_KEYS):
        mode_range(v, dev, keys, kp, bind, first, top)
    for name in keys:
        if name in RANGE_KEYS:
            continue
        path, line = f"{kp}.{name}", keys.line_of(name)
        k = mode_key_index(v, dev, name, path, line)
        if k is None:
            continue
        b = keys[name]
        r = None
        if isinstance(b, str):
            r = mode_action(v, dev, b, None, path, line)
        elif isinstance(b, dict) and "mode" in b:
            target = by_id.get(b["mode"])
            extra = [x for x in b if x != "mode"]
            if extra:
                v.err(f"{path}.{extra[0]}", b.line_of(extra[0]), "a key that goes to a sub-mode does nothing "
                                                                 "else (mode: only)")
            elif target is None:
                close = difflib.get_close_matches(str(b["mode"]), list(by_id), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(f"{path}.mode", b.line_of("mode"), f"no mode with id '{b.raw_of('mode')}' (modes: "
                                                         f"{', '.join(by_id)}{hint})")
            elif target.parent is None:
                v.err(f"{path}.mode", b.line_of("mode"), f"mode '{target.id}' has no parent: a key can only go to "
                                                         "a sub-mode (one with parent:)")
            else:
                r = (MK_MODE, target.index, f"mode {target.id}")
        elif isinstance(b, dict) and "action" in b:
            r = mode_action(v, dev, b["action"], b, path, line)
        else:
            v.err(path, line, "must be an action (`octave_reset`, `{ action: preset_select, preset: 2 }`, "
                              "`{ action: channel, channel: 10 }`, ...) or `{ mode: <sub-mode id> }`")
        if r:
            bind(k, *r, line)


def mode_range(v, dev, keys, kp, bind, first, top):
    """keys: { white_from: D4 | from: C4, action: preset_select | channel, count: N }: consecutive (white)
    keys numbered 1..count (default: every slot / 16 channels)."""
    start = [k for k in ("white_from", "from") if k in keys]
    if len(start) != 1 or "action" not in keys:
        v.err(kp, keys.line, "a range of keys needs white_from: (white keys) or from: (every key), and "
                             "action: (preset_select | channel), e.g. `{ white_from: D4, action: preset_select }`")
        return
    act, sk = keys["action"], start[0]
    if act not in RANGE_ACTIONS:
        v.err(f"{kp}.action", keys.line_of("action"), f"a range numbers its keys 1, 2, ...: action is one of "
                                                      f"{', '.join(RANGE_ACTIONS)} (got {keys.raw_of('action')!r})")
        return
    if mode_action(v, dev, act, None, f"{kp}.action", keys.line_of("action"), arg=1) is None:
        return
    count = v.integer(keys, "count", kp, 1, dev.n_presets if act == "preset_select" else 16,
                      default=dev.n_presets if act == "preset_select" else 16)
    k0 = mode_key_index(v, dev, keys[sk], f"{kp}.{sk}", keys.line_of(sk))
    if count is None or k0 is None:
        return
    white = sk == "white_from"
    if white and (first + k0) % 12 not in WHITE:
        v.err(f"{kp}.{sk}", keys.line_of(sk), f"{note_text(first + k0)} is a black key")
        return
    ks = [n - first for n in range(first + k0, top + 1) if not white or n % 12 in WHITE][:count]
    if len(ks) < count:
        v.err(f"{kp}.{sk}", keys.line_of(sk), f"{count} {'white ' if white else ''}keys from "
                                              f"{note_text(first + k0)} go past the top key ({note_text(top)}): "
                                              f"{len(ks)} fit (set count: {len(ks)}, or start lower)")
        return
    op = MODE_ACTIONS[act][0]
    for j, k in enumerate(ks, 1):
        bind(k, op, j, f"{act} {j}", keys.line_of(sk))


# controller.c: a step's full colour goes in its LED's level as the mode index + 1, in 7 bits
MODE_RGB_MAX = 127


def mode_rgb(dev):
    """gm_mode_rgb[] (GM_MODE_RGB): each mode's full step colour, None when no step has one."""
    rgb = [getattr(m, "rgb", None) for m in dev.modes]
    return rgb if any(c is not None for c in rgb) else None


def mode_tables(dev):
    """gm_modes[] (gm_device.h gm_mode_t), with each mode's key bindings sorted by key; gm_mode_rgb[] when a
    step has a full colour."""
    if not dev.modes:
        return []
    c = ["/* modes: key bindings { key index, op (gm_device.h GM_MK_*), arg } */"]
    rows = []
    for m in dev.modes:
        arr = "0"
        if m.keys:
            arr = f"mode_{m.index + 1}_keys"
            c.append(f"static const gm_mode_key_t {arr}[{len(m.keys)}] = {{ /* {_comment(m.id)} */")
            c += [f"    {{ {k}, {op}, {arg} }}, /* {_comment(text)} */" for k, op, arg, text in m.keys]
            c.append("};")
        barr = "0"
        if m.buttons:
            barr = f"mode_{m.index + 1}_buttons"
            c.append(f"static const gm_mode_key_t {barr}[{len(m.buttons)}] = {{ /* {_comment(m.id)}: buttons */")
            c += [f"    {{ {k}, {op}, {arg} }}, /* {_comment(text)} */" for k, op, arg, text in m.buttons]
            c.append("};")
        show = m.show or (-1, -1)
        toggle = "GM_MODE_NONE" if m.toggle is None else m.toggle
        parent = "GM_MODE_NONE" if m.parent is None else m.parent
        rows.append(f"    {{ {toggle}, {parent}, {m.root}, {-1 if m.led is None else m.led}, {int(m.blink)}, "
                    f"{show[0]}, {show[1]}, {len(m.keys)}, {arr}, {int(m.hold)}, {len(m.buttons)}, {barr}"
                    f", {0 if m.with_ is None else m.with_ + 1}, {m.cycle}, 0x{m.colour:02X}, "
                    f"{c_str(m.name if m.name is not None else m.id)}, {int(m.modifier)} }}, "
                    f"/* {_comment(m.id)} */")
    c += ["/* { toggle/hold/cycle button, parent, root, led, led_blink, blink_count, blink_done, n_keys, keys, "
          "hold, n_buttons, buttons, with, cycle, colour, name, modifier } */",
          "const gm_mode_t gm_modes[GM_N_MODES] = {", *rows, "};", ""]
    rgb = mode_rgb(dev)
    if rgb:
        c += ["/* the steps' full colours (cycle step colour: \"#rrggbb\" on an LED that can show any colour), "
              "0 = gm_modes[].colour */",
              "const uint32_t gm_mode_rgb[GM_N_MODES] = {",
              *[f"    {'0' if x is None else f'GM_LED_RGB(0x{x:06X})'}, /* {_comment(m.id)} */"
                for m, x in zip(dev.modes, rgb)], "};", ""]
    return c


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

def c_str(s):
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if c in '"\\':
            out.append("\\" + c)
        elif 32 <= b < 127 and c != "?":   # '?' escaped: no trigraphs
            out.append(c)
        else:
            out.append(f"\\{b:03o}")
    return '"' + "".join(out) + '"'


def _comment(s):
    return s.replace("*/", "* /")


def generate(dev, source="device.yaml"):
    """-> {"gm_device_config.h": str, "gm_device_config.c": str}"""
    src = _comment(source)
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
         "vpin.c links only their drivers */",
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
    h += matrix_defines(dev)
    hub_ram = sum(x.ram_bytes for x in dev.hubs if x.type in RAM_HUBS)
    if hub_ram:
        h.append(f"#define GM_HUB_RAM {hub_ram} /* hub RAM bytes (vpin.c hub_ram): its tm1628 and LED hubs' */")
    strips = sum(1 for x in dev.hubs if x.type == "ws2812")
    if strips:
        h.append(f"#define GM_LEDSTRIPS {strips} /* ws2812 hubs (the platform's hal_ledstrip_*) */")
    if dev.platform.name == "stm32f1":
        for x in dev.hubs:
            spi = Stm32F1Platform.LEDSTRIP_SPI.get(x.data.num) if x.type == "ws2812" and x.data.hub == 0 else None
            if spi:
                h.append(f"#define GM_LEDSTRIP_SPI{spi}_BYTES {x.ram_bytes} /* ws2812 '{_comment(x.id)}' on "
                         f"{x.data.desc}: a frame's bytes (platform/stm32f1/src/hal.c) */")
    if any(x.type in TM_HUBS and x.fade for x in dev.hubs):
        h.append("#define GM_TM_FADE 1 /* a tm1628 hub with fade: */")
    if m and getattr(m, "vel_fast", None) is not None:
        h.append(f"#define GM_VEL_FAST_DEFAULT {m.vel_fast} /* {m.vel_fast / 10:g} ms */")
    if m and getattr(m, "vel_slow", None) is not None:
        h.append(f"#define GM_VEL_SLOW_DEFAULT {m.vel_slow} /* {m.vel_slow / 10:g} ms */")
    if dev.i2c:
        h += [f"#define GM_I2C_SDA {dev.i2c[0].num} /* {dev.i2c[0].desc} */",
              f"#define GM_I2C_SCL {dev.i2c[1].num} /* {dev.i2c[1].desc} */"]
    h += feedback_defines(dev)
    h += ["", "/* presets (research/core/presets-design.md §10.1) */",
          f"#define GM_N_PRESETS {dev.n_presets} /* " +
          ("presets.slots */" if dev.presets_given else "no presets: in the YAML */"),
          f"#define GM_PRESETS_REVISION {dev.presets_revision} /* presets.revision */",
          f"#define GM_N_YAML_PRESETS {dev.n_yaml_presets} /* slots 1..N come from presets.list */",
          f"#define GM_N_MODES {len(dev.modes)} /* modes: (core/README.md \"Modes\") */"]
    if mode_rgb(dev):
        h.append("#define GM_MODE_RGB 1 /* a cycle step has a full colour: gm_mode_rgb[] */")
    h += write_to_flash_defines(dev)
    enc_twins = sum(1 for e in dev.encoders if getattr(e, "layer", 0))
    twins = sum(1 for b in dev.buttons if getattr(b, "layer", 0)) + len(dev.pad_twins) + enc_twins
    if twins:
        h.append(f"#define GM_LAYERS {twins} /* layer twins (modes: layer:, core/README.md \"Button layers\") */")
    if enc_twins:
        h.append(f"#define GM_ENC_TWINS {enc_twins} /* the last encoders: layer twins, not scanned */")
    h.append(f"#define GM_ENC_ACCEL {1 if enc_accel(dev) else 0} /* 1: gm_enc_accel[] (encoder acceleration:) */")
    h += msg_defines(dev)
    btn_twins = sum(1 for b in dev.buttons if getattr(b, "layer", 0))
    if btn_twins:
        h.append(f"#define GM_BTN_TWINS {btn_twins} /* the last buttons: layer twins (layer.button.*) */")
    if dev.modes:
        h.append(f"#define GM_MODES_ENABLED {', '.join(str(int(m.enabled)) for m in dev.modes)} "
                 "/* layer.enabled defaults, per mode */")
    if getattr(dev, "n_cycles", 0):
        h.append(f"#define GM_N_CYCLES {dev.n_cycles} /* modes: cycle: groups (core/README.md \"Cycles\") */")
    if dev.pad_twins:
        h.append(f"#define GM_PAD_TWINS {len(dev.pad_twins)} /* gm_pad_twins[] */")
    h += pad_defines(dev)
    h += display_defines(dev)
    h += safety_defines(dev)
    h += layout_defines(dev)
    if dev.platform.is_esp:
        h += ["", "#ifndef GM_MIDI_USB", "#define GM_MIDI_USB 1", "#define GM_MIDI_SERIAL 2",
              "#define GM_MIDI_BLE 3", "#endif",
              f"#define GM_MIDI_TRANSPORT {MIDI_TRANSPORT_MACRO[dev.transport]}"]
    h += ["", "#endif", ""]

    c = [head, '#include "gm_device.h"', '#include "gm_device_config.h"', ""]

    def pin_c(pn):
        return pn.c()

    if dev.hubs:
        for hb in dev.hubs:
            c += hb.kind.c_tables(hb)
        c.append("static const gm_hub_t hubs[GM_N_HUBS] = {")
        for hb in dev.hubs:
            fields = hb.kind.c_fields(hb)
            c += [f"    {{ /* {hb.index + 1}: {_comment(hb.id)} ({hb.type}) */",
                  f"        .type = {hb.kind.enum}, {fields[0].lstrip()}", *fields[1:], "    },"]
        c += ["};", ""]
    if dev.buttons:
        c.append("static const gm_button_t buttons[GM_N_BUTTONS] = {")
        c += [f"    {{ {pin_c(b.pin)}, {c_str(b.name)}, {b.led}{twin_c(b)} }}, "
              f"/* {_comment(b.id)}: {_comment(b.pin.desc)} */" for b in dev.buttons]
        c += ["};", ""]
    if dev.leds:
        c.append("static const gm_led_t leds[GM_N_LEDS] = {")
        c += [f"    {{ {pin_c(x.pin)}, {pin_c(x.pin2)}, {pin_c(x.pin3)}{led_looks_c(x)}{twin_c(x)}{led_rgb_c(x)} }}, "
              f"/* {_comment(x.id)}: {_comment(x.pin.desc)} */" for x in dev.leds]
        c += ["};", ""]
    if dev.analogs:
        c.append("static const gm_analog_t analogs[GM_N_ANALOGS] = {")
        c += [f"    {{ {pin_c(a.pin)}, {c_str(a.name)}, {a.raw_min}, {a.raw_max} }}, "
              f"/* {_comment(a.id)}: {_comment(a.pin.desc)} */"
              for a in dev.analogs]
        c += ["};", ""]
    if dev.encoders:
        c.append("static const gm_encoder_t encoders[GM_N_ENCODERS] = {")
        c += [f"    {{ {pin_c(e.a)}, {pin_c(e.b)}, {c_str(e.name)}, {e.steps}{twin_c(e)} }}, "
              f"/* {_comment(e.id)}: {_comment(e.a.desc)}, {_comment(e.b.desc)} */" for e in dev.encoders]
        c += ["};", ""]
    if enc_accel(dev):
        own = [e for e in dev.encoders if not getattr(e, "layer", 0)]
        c.append("/* acceleration: per scanned encoder (twins share theirs): slow ms, fast ms, max, curve */")
        c.append(f"const gm_enc_accel_t gm_enc_accel[{len(own)}] = {{")
        for e in own:
            a = getattr(e, "accel", None)
            body = (f"{a[0]}, {a[1]}, {a[2]}, GM_ACCEL_{ACCEL_CURVES[a[3]].upper()}" if a
                    else "0, 0, 1, GM_ACCEL_LINEAR")
            c.append(f"    {{ {body} }}, /* {_comment(e.id)}: {accel_text(a) if a else 'off'} */")
        c += ["};", ""]
    if m:
        for nm, lst in (("rows", m.rows), ("cols", m.cols)):
            c.append(f"static const gm_pin_t matrix_{nm}[{len(lst)}] = {{")
            c += [f"    {x.c()}, /* {_comment(x.desc)} */" for x in lst]
            c += ["};"]
        kind = "GM_MATRIX_KIND_VELOCITY" if m.kind == "velocity" else "GM_MATRIX_KIND_SWITCH"
        if m.key_map:
            c.append(f"static const uint8_t matrix_key_map[{len(m.key_map)}] = {{")
            per = max(1, len(m.cols) if m.contacts == "rows" or m.kind == "switch" else len(m.cols) // 2)
            for i in range(0, len(m.key_map), per):
                c.append("    " + ", ".join(str(x) for x in m.key_map[i:i + per]) + ",")
            c += ["};"]
        contacts = "GM_MATRIX_CONTACTS_ROWS" if m.contacts == "rows" else "GM_MATRIX_CONTACTS_COLS"
        c += ["static const gm_matrix_t matrix = {",
              f"    .rows = matrix_rows, .n_rows = {len(m.rows)},",
              f"    .cols = matrix_cols, .n_cols = {len(m.cols)},",
              f"    .kind = {kind}, .n_keys = {m.keys}, .first_note = {m.first_note}, "
              f".settle_us = {m.settle_us},",
              f"    .contacts = {contacts}, .key_map = {'matrix_key_map' if m.key_map else '0'},",
              "};", ""]
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
          "/* param defaults; button.action values: core/README.md \"Button actions\" */"]

    def arr(name, count_macro, vals, comment=""):
        size = count_macro if vals else "1"
        body = ", ".join(str(x) for x in vals) if vals else "0"
        return f"const int32_t {name}[{size}] = {{ {body} }};{comment}"

    acts = ", ".join(b.action_name for b in dev.buttons)
    c += [arr("gm_button_action_defaults", "GM_N_BUTTONS", [b.action for b in dev.buttons],
              f" /* {acts} */" if acts else ""),
          arr("gm_button_number_defaults", "GM_N_BUTTONS", [b.number for b in dev.buttons]),
          arr("gm_analog_cc_defaults", "GM_N_ANALOGS", [a.cc for a in dev.analogs]),
          arr("gm_analog_mode_defaults", "GM_N_ANALOGS", [a.mode for a in dev.analogs],
              " /* " + ", ".join(ANALOG_MODES[a.mode] for a in dev.analogs) + " */" if dev.analogs else ""),
          arr("gm_encoder_cc_defaults", "GM_N_ENCODERS", [e.cc for e in dev.encoders]), ""]
    c += msg_tables(dev)
    c += feedback_tables(dev)
    c += preset_tables(dev)
    c += mode_tables(dev)
    c += pad_tables(dev)
    c += display_tables(dev)
    c += layout_tables(dev)
    c += label_tables(dev)
    return {"gm_device_config.h": "\n".join(h), "gm_device_config.c": "\n".join(c)}



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

MATRIX_FAST_COLS = 16   # controller.c FAST_COLS: columns read as port runs
MATRIX_MAX_ROWS = 32    # controller.c: rows scanned


def matrix_col_runs(dev, m):
    """How many col_run_t entries controller.c's gm_init makes for the key matrix: runs of columns
    c, c+1, ... on consecutive bits of one input register. NUC1xx and STM32F1 have a register per
    port of 16 pins (pin = port * 16 + bit); elsewhere, or for hub pins, assume one per column."""
    cols = m.cols[:MATRIX_FAST_COLS]
    if dev.platform.is_esp:
        return len(cols)
    runs, last = 0, None            # last: (port, next bit, next column) that would extend the run
    for c, pn in enumerate(cols):
        if pn.hub != 0:
            continue                # no register: the scan reads pin by pin
        port, bit = divmod(pn.num, 16)
        if last != (port, bit, c):
            runs += 1
        last = (port, bit + 1, c + 1)
    return runs


HW_SCAN_MAX_ROWS = 32   # gm_controller.h GM_SCAN_MAX_ROWS
HW_ADC_MAX_STEPS, HW_ADC_MAX_PINS = 16, 8   # gm_controller.h GM_ADC_MAX_STEPS, GM_ADC_MAX_PINS


def hw_scan_sizes(dev):
    """The largest plans the hardware scans can get (gm_scan_plan, gm_adc_plan), so the
    platform's DMA rings and the readings are sized for this device, not the maximum:
    GM_HW_SCAN_ROWS = key rows + switch-matrix rows (vpin_scan_plan: those within the 32-bit
    cache; shared selects only make it fewer), even; GM_HW_ADC_PINS / _STEPS = the distinct
    MCU ADC pins (direct, or a mux's common) and the most mux steps among the analog inputs
    and pads (xy_pad: read in software)."""
    out = []
    if dev.hw_scan:
        rows = len(dev.matrix.rows) if dev.matrix else 0
        for h in dev.hubs:
            if h.type == "switch_matrix":
                rows += sum(1 for r in range(len(h.rows)) if r * len(h.cols) < 32)
        rows = min(max(rows + (rows & 1), 2), HW_SCAN_MAX_ROWS)
        out.append(f"#define GM_HW_SCAN_ROWS {rows} /* hardware scan plan: rows at most */")
    if dev.hw_adc:
        pins, steps = set(), 1
        for x in [*dev.analogs, *dev.pads]:
            pin = x.pin
            if pin.hub == 0:
                pins.add(pin.num)
            elif 0 < pin.hub <= len(dev.hubs):
                h = dev.hubs[pin.hub - 1]
                if h.type in MUX_HUBS and h.common is not None and h.common.hub == 0:
                    pins.add(h.common.num)
                    steps = max(steps, 1 << h.n_sel)
        out.append(f"#define GM_HW_ADC_PINS {min(max(len(pins), 1), HW_ADC_MAX_PINS)} "
                   "/* hardware analog scan: ADC pins at most */")
        out.append(f"#define GM_HW_ADC_STEPS {min(steps, HW_ADC_MAX_STEPS)} /* ... mux steps at most */")
    return out


def matrix_defines(dev):
    """The key-matrix scan's table sizes in controller.c (core/README.md "Generated code")."""
    m = dev.matrix
    if not m:
        return []
    dec = any(0 < pn.hub <= len(dev.hubs) and dev.hubs[pn.hub - 1].type == "decoder" for pn in m.rows)
    return [f"#define GM_MATRIX_ROWS {min(len(m.rows), MATRIX_MAX_ROWS)} /* key-matrix rows scanned (at most 32) */",
            f"#define GM_MATRIX_COL_RUNS {matrix_col_runs(dev, m)} /* runs of adjacent column bits on one port */",
            f"#define GM_MATRIX_DECODER {1 if dec else 0} /* rows on a decoder: one-store row selects */"]


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


def pad_defines(dev):
    """GM_N_PADS, and with pads the pad.* param defaults from pad_settings: (core/src/settings.c)."""
    h = ["", f"#define GM_N_PADS {len(dev.pads)} /* pads: (core/README.md \"Pads\") */"]
    if dev.pads:
        ps = dev.pad_set
        h += [f"#define GM_PAD_CHANNEL_DEFAULT {ps['channel']} /* 0 = midi.channel */",
              f"#define GM_PAD_CURVE_DEFAULT {ps['curve']} /* {CURVES[ps['curve']]} */",
              f"#define GM_PAD_AFTERTOUCH_DEFAULT {ps['aftertouch']}",
              f"#define GM_PAD_THRESHOLD_DEFAULT {ps['threshold']}",
              f"#define GM_PAD_RELEASE_DEFAULT {ps['release']}",
              f"#define GM_PAD_FULL_DEFAULT {ps['full']}",
              f"#define GM_PAD_WINDOW_DEFAULT {ps['window']} /* ms */",
              f"#define GM_PAD_RETRIGGER_DEFAULT {ps['retrigger']} /* ms */"]
    return h


def pad_tables(dev):
    """gm_pads[] (gm_device.h gm_pad_t) and the pad.note defaults; nothing without pads."""
    if not dev.pads:
        return []
    return ["const gm_pad_t gm_pads[GM_N_PADS] = {",
            *[f"    {{ {x.pin.c()}, {c_str(x.name)}, {x.led} }}, /* {_comment(x.id)}: {_comment(x.pin.desc)} */"
              for x in dev.pads],
            "};",
            "const int32_t gm_pad_note_defaults[GM_N_PADS] = { " + ", ".join(str(x.note) for x in dev.pads) + " };",
            *(["/* pad layer twins: { layer (mode index + 1), base pad, LED } */",
               "const gm_pad_twin_t gm_pad_twins[GM_PAD_TWINS] = {",
               *[f"    {{ {x.layer}, {x.base}, {x.led} }}, /* {_comment(x.id)} */" for x in dev.pad_twins],
               "};"] if dev.pad_twins else []),
            ""]


OWNER_NAMES = {n: k for k, n in LED_OWNERS.items()}
SOURCE_ENUM = {0: "GM_FB_NONE", 1: "GM_FB_NOTE", 2: "GM_FB_CC", 3: "GM_FB_CLOCK", 4: "GM_FB_PLAYING", 5: "GM_FB_MCU"}
OWNER_ENUM = {0: "GM_OWNER_HOST_THEN_LOCAL", 1: "GM_OWNER_HOST", 2: "GM_OWNER_LOCAL"}


def fmt_timeout(t):
    if t == TIMEOUT_NEVER:
        return "never"
    return f"{t / 100:g}s" if t % 10 == 0 and t >= 100 else fmt_us(t * 10_000)


def twin_c(x):
    """gm_button_t / gm_led_t / gm_encoder_t .layer, .base for a layer twin (mode index + 1, the component
    it is a twin of); 0, 0 for the others."""
    return f", {x.layer}, {x.base}" if getattr(x, "layer", 0) else ", 0, 0"


def led_rgb_c(x):
    """gm_led_t.rgb: full-colour idle / active looks, 0 each for none."""
    rgb = getattr(x, "rgb", None)
    if not rgb or not any(c is not None for c in rgb):
        return ", { 0, 0 }"
    return ", { " + ", ".join("0" if c is None else f"GM_LED_RGB(0x{c:06X})" for c in rgb) + " }"


def led_looks_c(x):
    """gm_led_t.looks: 0 for the defaults, else GM_LED_LOOKS(idle, active)."""
    looks = getattr(x, "looks", None)
    return ", 0" if looks is None else f", GM_LED_LOOKS({looks[0]}, {looks[1]})"


def led_fb_rows(dev):
    """Per LED (source, num, channel, owner, timeout); None when every LED uses the device defaults
    (then no table is generated)."""
    rows, special = [], False
    # A cycle's LED always shows its step (controller.c mode_led), whatever a host sends: say so
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
                     f"/* {_comment(x.id)} */")
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
                         f"/* {_comment(e.id)} */")
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
            c.append(f"static const gm_preset_val_t {arr}[{len(pr.vals)}] = {{ /* {_comment(pr.name)} */")
            c += [f"    {{ {pid}, {idx}, {val} }}, /* {_comment(text)} */" for pid, idx, val, text in pr.vals]
            c.append("};")
        prog = "GM_PROGRAM_NONE" if pr.program < 0 else str(pr.program)
        rows.append(f"    {{ {c_str(pr.name)}, {prog}, {len(pr.vals)}, {arr} }}, /* slot {n} */")
    c += ["const gm_preset_def_t gm_presets[GM_N_PRESETS] = {", *rows, "};", ""]
    return c


def write_generated(dev, out_dir, source):
    os.makedirs(out_dir, exist_ok=True)
    files = generate(dev, source)
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
