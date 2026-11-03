"""gemifi device YAML: loading (with line numbers), validation and C code generation.

The format and the generated-code contract are defined in core/README.md ("Device YAML" and
"Generated code"); the C structs are core/include/gm_device.h. This module is used by
gemifi_build.py (CLI + dashboard) and the tests.

    dev, diags = load_device(path)          # dev is None if there are errors
    files = generate(dev, source="devices/x.yaml")   # {"gm_device_config.h": text, ...}
"""
import difflib
import os
import re

try:
    import yaml
except ImportError:  # pragma: no cover - reported by gemifi_build.py
    yaml = None

YAML_HINT = ("PyYAML is required: `pip install pyyaml` (or `sudo apt install python3-yaml`, "
             "or `python3 -m venv tools/builder/.venv && tools/builder/.venv/bin/pip install pyyaml`)")

FORMAT_VERSION = 2

# core/README.md "Button actions": YAML name -> button.action value (gm_settings.h ACT_*). The preset
# actions (13-15) need 2+ preset slots; `mcu` (16) only exists on devices with an `mcu:` section.
ACTION_IDS = {"none": 0, "cc_momentary": 1, "cc_toggle": 2, "cc_ramp": 3, "note": 4, "octave_down": 5,
              "octave_up": 6, "transpose_down": 7, "transpose_up": 8, "bend_down": 9, "bend_up": 10,
              "program_next": 11, "program_prev": 12, "preset_next": 13, "preset_prev": 14,
              "preset_select": 15, "mcu": 16}
ACTIONS = list(ACTION_IDS)
CC_ACTIONS = {"cc_momentary", "cc_toggle", "cc_ramp"}
PRESET_ACTIONS = {"preset_next", "preset_prev", "preset_select"}
NUMBER_KEYS = ("cc", "note", "preset")      # the YAML keys that give button.number (mcu: aside)


def number_key(act):
    """The key that gives button.number for an action (None: the action takes no number)."""
    return "note" if act == "note" else "cc" if act in CC_ACTIONS else "preset" if act == "preset_select" else None


# ---- params and their scope (research/core/presets-design.md §1, §10.3) ----
# The ONE place the builder knows core/src/settings.c's param table: the id (gm_settings.h P_*), the
# scope (PRESET = PF_PRESET: each preset has its own value; GLOBAL: one value for the device;
# VOLATILE: never stored) and how many values a device has (0 = the param doesn't exist there).
# The storage budget (§10.5) counts the stored values from it. Keep it in step with settings.c.
PRESET, GLOBAL, VOLATILE = "preset", "global", "volatile"


def _vel(dev):
    return 1 if dev.matrix is not None and dev.matrix.kind == "velocity" else 0


PARAMS = [  # (name, id, scope, count(dev))
    ("midi.channel", 1, PRESET, lambda d: 1),
    ("midi.transpose", 2, PRESET, lambda d: 1),
    ("midi.octave", 3, PRESET, lambda d: 1),
    ("velocity.curve", 4, PRESET, _vel),
    ("velocity.fixed", 5, PRESET, lambda d: 1),
    ("velocity.fast", 6, GLOBAL, _vel),
    ("velocity.slow", 7, GLOBAL, _vel),
    ("analog.cc", 8, PRESET, lambda d: len(d.analogs)),
    ("button.action", 9, PRESET, lambda d: len(d.buttons)),
    ("button.number", 10, PRESET, lambda d: len(d.buttons)),
    ("bend.glide", 11, PRESET, lambda d: 1 if d.buttons else 0),
    ("ramp.time", 12, PRESET, lambda d: 1 if d.buttons else 0),
    ("debug.events", 13, GLOBAL, lambda d: 1),
    ("debug.log_level", 14, GLOBAL, lambda d: 1),
    ("encoder.cc", 15, PRESET, lambda d: len(d.encoders)),
    ("encoder.mode", 16, PRESET, lambda d: len(d.encoders)),
    ("preset.active", 17, GLOBAL, lambda d: 1),
    ("preset.program", 18, PRESET, lambda d: 1 if d.n_presets > 1 else 0),
    ("preset.dirty", 19, VOLATILE, lambda d: 1 if d.n_presets > 1 else 0),
    ("pad.note", 20, PRESET, lambda d: len(d.pads)),
    *((n, i, s, (lambda d: 1 if d.pads else 0)) for n, i, s in (
        ("pad.channel", 21, PRESET), ("pad.curve", 22, PRESET), ("pad.aftertouch", 23, PRESET),
        ("pad.threshold", 24, GLOBAL), ("pad.release", 25, GLOBAL), ("pad.full", 26, GLOBAL),
        ("pad.window", 27, GLOBAL), ("pad.retrigger", 28, GLOBAL))),
    ("analog.mode", 29, PRESET, lambda d: len(d.analogs)),
]
# Params whose stored values can need 16 bits (settings.c min/max outside -128..127); every other
# value fits a byte. The packed stm32f1 record (platform/stm32f1/src/config_store.c) sizes its groups
# from this, so keep it in step with settings.c (test_builder checks it).
WIDE_PARAMS = {"velocity.fast", "velocity.slow", "bend.glide", "ramp.time",
               "pad.threshold", "pad.release", "pad.full", "pad.retrigger"}
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
ENC_MODES = ["absolute", "relative_twos", "relative_offset"]
ANALOG_MODES = ["cc", "pitch_bend", "channel_pressure"]    # analog.mode (gm_settings.h AN_MODE_*)
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
                     "aftertouch": ("pad.aftertouch", lambda d: d.pad_set["aftertouch"], "bool", 0, 1)},
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
    "buttons": ("button", "buttons", ["action", "cc", "note", "preset", "mcu"], ["id", "name", "pin", "led", "daw"]),
    "analog": ("analog input", "analog inputs", ["cc", "mode"],
               ["id", "name", "pin", "adc_min", "adc_max", "takeover", "mcu", "daw"]),
    "encoders": ("encoder", "encoders", ["cc", "mode"],
                 ["id", "name", "pin_a", "pin_b", "steps_per_detent", "mcu", "ring", "ring_center", "daw"]),
    "pads": ("pad", "pads", ["note"], ["id", "name", "pin"]),
}
PRESET_KEYS = ["name", "program", *PRESET_GLOBALS, *PRESET_COMPONENTS]
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
    # two 1 KB pages swapped on a rewrite: the newest record of every slot must fit one page together
    # (platform/stm32f1/src/config_store.c), the same rule as one image; records are packed (v3)
    "stm32f1": {"kind": "image", "total": 1024, "format": "packed", "what": "config page"},
    # nvs: one blob per slot (each <= record bytes), all of them <= total bytes
    **{p: {"kind": "nvs", "record": 1984, "total": 12 * 1024, "what": "NVS"}
       for p in ("esp32", "esp32s2", "esp32s3")},
    # image: every slot in one image of total bytes
    "esp8266": {"kind": "image", "total": 4096, "what": "EEPROM sector"},
}
BUDGET_WARN = 0.85             # warn above this share of the budget (more erase cycles)

# ---- host feedback / DAW (core/README.md "DAW feedback") ----
# LED owners (gm_device.h GM_OWNER_*): who drives an LED that both the host and the device want.
LED_OWNERS = {"host_then_local": 0, "host": 1, "local": 2}
LED_SOURCES = {"note": 1, "cc": 2, "clock": 3, "transport": 4, "mcu": 5}   # gm_device.h GM_FB_*
LED_SOURCE_VALUES = {"clock": ["beat"], "transport": ["playing"]}
TAKEOVER = ["none", "pickup"]
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
    "bank_prev", "bank_next", "master_volume", "cue_volume", "crossfader", "tempo",
    "device_prev", "device_next", "device_bank_prev", "device_bank_next", "device_on",
    "scene_launch", "scene_prev", "scene_next", "stop_all_clips",
}
DAW_ROLE_RE = re.compile(r"x_[a-z0-9_]{1,29}")
COMP_STR_MAX = 31               # sysex.md lstring

# Hubs (I/O expanders), declared ESPHome style: one top-level list per chip, named like the
# ESPHome component where there is one. A pin on a hub names the hub by its chip key:
# { sn74hc595: leds, number: 3 }. ESPHome's pin key for the MCP23017 is mcp23xxx.
HUB_ENUM = {"sn74hc595": "GM_HUB_HC595", "sn74hc165": "GM_HUB_HC165", "cd74hc4051": "GM_HUB_MUX",
            "cd74hc4067": "GM_HUB_MUX", "mcp23017": "GM_HUB_MCP23017", "pcf8574": "GM_HUB_PCF8574",
            "decoder": "GM_HUB_DECODER", "switch_matrix": "GM_HUB_SWMATRIX",
            "tm1628": "GM_HUB_TM1628", "rgb_serial": "GM_HUB_RGB_SERIAL"}
HUB_TYPES = list(HUB_ENUM)
HUB_PIN_KEY = {t: ("mcp23xxx" if t == "mcp23017" else t) for t in HUB_TYPES}
PIN_KEY_HUB = {k: t for t, k in HUB_PIN_KEY.items()}
I2C_HUBS = {"mcp23017", "pcf8574"}
SHIFT_HUBS = {"sn74hc595", "sn74hc165"}
MUX_HUBS = {"cd74hc4051", "cd74hc4067"}
MUX_SELECT = {"cd74hc4051": 3, "cd74hc4067": 4}
# TM1628-class LED drivers (TM1628, TM1638, TM1620 ...): pin = display RAM bit, byte * 8 + bit.
TM_HUBS = {"tm1628"}
TM_RAM_MAX = 16                 # TM1638 has 16 bytes, TM1628 14
# RGB LEDs behind a serial link (the M-Audio Code pad LED board): pin = LED * 3 + colour
# (0 red, 1 green, 2 blue). Kept in the same RAM as the TM1628s, sent from the main loop.
RGB_SERIAL_MAX = 32
RAM_HUBS = TM_HUBS | {"rgb_serial"}
OUT_HUBS = {"sn74hc595", "mcp23017", "pcf8574", "tm1628", "rgb_serial"}
IN_HUBS = {"sn74hc165", "cd74hc4051", "cd74hc4067", "mcp23017", "pcf8574", "switch_matrix"}
# A decoder's outputs are one-of-N selects: only matrix rows (key_matrix, switch_matrix).
ROW_HUBS = {"decoder"}
HUB_KEYS = {
    "sn74hc595": ["id", "data_pin", "clock_pin", "latch_pin", "oe_pin", "sr_count"],
    "sn74hc165": ["id", "data_pin", "clock_pin", "load_pin", "sr_count"],
    "cd74hc4051": ["id", "pin_s0", "pin_s1", "pin_s2", "common_pin", "enable_pin", "delay"],
    "cd74hc4067": ["id", "pin_s0", "pin_s1", "pin_s2", "pin_s3", "common_pin", "enable_pin", "delay"],
    "mcp23017": ["id", "address"],
    "pcf8574": ["id", "address"],
    "decoder": ["id", "address", "groups"],
    "switch_matrix": ["id", "rows", "columns", "settle", "rows_per_scan"],
    "tm1628": ["id", "clk_pin", "dio_pin", "stb_pin", "ram_bytes", "intensity", "grids"],
    "rgb_serial": ["id", "tx_pin", "count"],
}
# hub control pins that may be shared between hubs (allow_other_uses): same line kind on each
CTRL_LINE = {"clock_pin": "clock", "latch_pin": "latch", "load_pin": "latch", "data_pin": "data",
             "oe_pin": "enable", "enable_pin": "enable", "common_pin": "common",
             "pin_s0": "s0", "pin_s1": "s1", "pin_s2": "s2", "pin_s3": "s3",
             # TM1628 lines only pair with each other: they're bit-banged from the main loop
             "clk_pin": "tm_clk", "dio_pin": "tm_dio", "stb_pin": "tm_stb", "tx_pin": "uart_tx"}

TOP_KEYS = ["gemifi", "name", "friendly_name", "description", "project", "board_id", "platform", "chip",
            "usb", "midi", "i2c", "scan_interval", "boot_combo", "feedback", "mcu", *HUB_TYPES,
            "buttons", "leds", "analog", "encoders", "key_matrix", "pads", "pad_settings", "presets", "modes",
            "displays"]
PIN_KEYS = ["number", "inverted", "mode", "allow_other_uses", *PIN_KEY_HUB]
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
    def __init__(self, level, path, line, msg, file=None):
        self.level, self.path, self.line, self.msg, self.file = level, path, line, msg, file

    def __str__(self):
        loc = self.file or "<device>"
        if self.line:
            loc += f":{self.line}"
        where = f"{self.path}: " if self.path else ""
        return f"{loc}: {self.level}: {where}{self.msg}"

    def as_dict(self):
        return {"level": self.level, "path": self.path, "line": self.line, "msg": self.msg}


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


def load_yaml(text):
    """Parse one YAML document into LDict / LList / plain scalars. Raises YamlLoadError."""
    if yaml is None:
        raise YamlLoadError(0, YAML_HINT)
    try:
        node = yaml.compose(text, Loader=yaml.SafeLoader)
    except yaml.YAMLError as e:
        mark = getattr(e, "problem_mark", None) or getattr(e, "context_mark", None)
        line = mark.line + 1 if mark else 0
        msg = " ".join(str(x) for x in (getattr(e, "context", None), getattr(e, "problem", None)) if x)
        raise YamlLoadError(line, f"YAML syntax: {msg or e}")
    if node is None:
        raise YamlLoadError(1, "the file is empty")
    ctor = yaml.SafeLoader("")
    active = set()

    def conv(n):
        if id(n) in active:
            raise YamlLoadError(n.start_mark.line + 1, "recursive YAML alias")
        if isinstance(n, yaml.MappingNode):
            active.add(id(n))
            ctor.flatten_mapping(n)
            d = LDict()
            d.line = n.start_mark.line + 1
            for kn, vn in n.value:
                k = conv(kn)
                kl = kn.start_mark.line + 1
                if isinstance(k, (dict, list)):
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
            lst.line = n.start_mark.line + 1
            for item in n.value:
                lst.append(conv(item))
                lst.item_lines.append(item.start_mark.line + 1)
                lst.item_raw.append(item.value if isinstance(item, yaml.ScalarNode) else None)
            active.discard(id(n))
            return lst
        try:
            return ctor.construct_object(n, deep=True)
        except yaml.YAMLError as e:
            raise YamlLoadError(n.start_mark.line + 1, f"bad value: {e}")

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


class Platform:
    name = ""
    native_usb = False
    transports = ("usb",)
    default_transport = "usb"
    is_esp = False

    def parse_pin(self, v):
        """-> pin number, or raise ValueError(friendly message)."""
        raise NotImplementedError

    def pin_name(self, num):
        return str(num)

    def check(self, num, role, flags, transport):
        """-> list of (level, message) for using pin num as role ('in' | 'out' | 'analog')."""
        return []

    def serial_tx(self, num):
        """-> None if num can be an rgb_serial tx_pin, else why not."""
        return f"rgb_serial isn't supported on {self.name} yet"

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
        """-> None if num can be an rgb_serial tx_pin, else why not."""
        return None if num == 2 else "only PA2 (USART2 TX) is supported as an rgb_serial tx_pin on stm32f1"


class EspPlatform(Platform):
    is_esp = True

    def __init__(self, name, valid, flash, strapping, input_only, adc1, adc2, usb_pins=(),
                 uart0=(), psram=(), transports=("serial",), default_transport="serial"):
        self.name, self.valid = name, set(valid)
        self.flash, self.strapping, self.input_only = set(flash), set(strapping), set(input_only)
        self.adc1, self.adc2, self.usb_pins = set(adc1), set(adc2), set(usb_pins)
        self.uart0, self.psram = set(uart0), set(psram)
        self.transports, self.default_transport = transports, default_transport
        self.native_usb = "usb" in transports

    def valid_text(self):
        return _ranges(self.valid - {17} if self.name == "esp8266" else self.valid,
                       lambda n: f"GPIO{n}") + (", A0" if self.name == "esp8266" else "")

    def parse_pin(self, v):
        if isinstance(v, bool):
            raise ValueError(f"'{v}' is not a pin")
        s = str(v).strip()
        if self.name == "esp8266" and s.upper() == "A0":
            return 17
        m = re.fullmatch(r"(?:GPIO|IO)?(\d{1,2})", s, re.I)
        n = int(m.group(1)) if m else None
        if n is None or n not in self.valid or (self.name == "esp8266" and n == 17):
            raise ValueError(f"'{v}' is not a pin on {self.name} (valid: {self.valid_text()})")
        return n

    def pin_name(self, num):
        return "A0" if self.name == "esp8266" and num == 17 else f"GPIO{num}"

    def check(self, num, role, flags, transport):
        out, nm = [], self.pin_name(num)
        if self.name == "esp8266" and num == 17 and role != "analog":
            out.append(("error", "A0 is analog-only on esp8266"))
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
            if self.name == "esp8266":
                if num == 16 and flags & PULLUP:
                    out.append(("error", "GPIO16 on esp8266 has a pull-down only, no pull-up"))
                if num != 16 and flags & PULLDOWN:
                    out.append(("error", f"{nm} on esp8266 has no internal pull-down (only GPIO16 has one)"))
        return out

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
    EspPlatform("esp8266", valid=list(range(0, 18)), flash=range(6, 12), strapping=[0, 2, 15],
                input_only=[], adc1=[17], adc2=[], uart0=[1, 3], transports=("serial",)),
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
        self.hubs, self.buttons, self.leds, self.analogs, self.encoders = [], [], [], [], []
        self.matrix = None
        self.version = None
        self.boot_combo = []
        self.i2c = None
        self.feedback = True
        self.led_owner, self.led_timeout = 0, DEFAULT_LED_TIMEOUT_MS // 10
        self.host_fb = True          # feedback.script: GET_COMPONENT and FB_* (sysex.md 4.6)
        self.mcu = None              # Hub(extender=bool, cable=0|1) when mcu: is given
        self.daw_port = False
        # presets (research/core/presets-design.md): slot n = presets[n - 1], Hub(name, program, vals)
        self.n_presets = 1
        self.presets_given = False   # the YAML has presets:
        self.presets_revision = 0
        self.presets = []
        self.modes = []              # Hub per mode (validate_modes), in YAML order
        self.pads = []               # Hub per pad (validate_pads), in YAML order
        self.pad_set = dict(PAD_DEFAULTS)   # pad_settings: the pad.* param defaults
        self.displays = []           # Hub per display (validate_displays), in YAML order
        self.budget = None           # storage_budget(self), after validation


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
        self.transport = None
        self.hub_by_id = {}
        self.presets = None      # the presets: mapping, once preset_slots has checked it
        self.mode_toggles = {}   # button index -> mode id, for the preset check
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
            bad = "open_drain is only supported on a tm1628 dio_pin"
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
            for lvl, msg in self.plat.check(num, "in" if role == "common" else role, flags, self.transport):
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
        allowed = hubs or {"out": OUT_HUBS, "in": IN_HUBS, "analog": MUX_HUBS}[role]
        if hub.type not in allowed:
            self.err(path, line, f"hub '{hub.id}' is a {hub.type}, which can't be {ROLE_TEXT[role]} "
                                 f"(use {' / '.join(sorted(allowed))}{'; ' + why if why else ''})")
            return None
        if role == "analog" and hub.common is not None and not hub.common_adc:
            self.err(path, line, f"hub '{hub.id}' common_pin {hub.common.desc} has no ADC, so its "
                                 f"channels can't be analog ({adc_text(self.plat)})")
        if flags & PULLDOWN:
            self.err(path, line, f"a {hub.type} pin has no pull-down")
        if flags & PULLUP and hub.type != "mcp23017":
            self.err(path, line, f"a {hub.type} pin has no pull-up (only mcp23017 pins do)")
        desc = f"{hub.id}.{num}"
        self.use(("hub", hub.index, num), desc, Use(path, line))
        return Pin(hub.index + 1, flags, num, desc)

    def hub_pin_num(self, hub, raw, path, line):
        n = None
        if isinstance(raw, int) and not isinstance(raw, bool):
            n = raw
        elif isinstance(raw, str):
            s = raw.strip().upper()
            m = re.fullmatch(r"(?:GP)?([AB])([0-7])", s) if hub.type == "mcp23017" else \
                re.fullmatch(r"P([0-7])", s) if hub.type == "pcf8574" else None
            if m and hub.type == "mcp23017":
                n = (8 if m.group(1) == "B" else 0) + int(m.group(2))
            elif m:
                n = int(m.group(1))
            elif s.isdigit():
                n = int(s)
        if n is None:
            self.err(path, line, f"'{raw}' is not a pin of hub '{hub.id}' (use a number 0..{hub.n_pins - 1})")
            return None
        if not 0 <= n < hub.n_pins:
            what = "channel" if hub.type in MUX_HUBS else "pin"
            extra = f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8)" if hub.type in SHIFT_HUBS else \
                f" ({hub.ram_bytes} RAM bytes x 8; pin = byte * 8 + bit)" if hub.type in TM_HUBS else \
                f" ({hub.chain} LEDs x 3; pin = LED * 3 + colour, 0 red 1 green 2 blue)" \
                if hub.type == "rgb_serial" else ""
            self.err(path, line, f"{what} {n} is out of range for hub '{hub.id}' ({hub.type}{extra}: "
                                 f"0..{hub.n_pins - 1})")
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
    if plat.name == "esp8266":
        return "the only ADC pin is A0"
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

    # midi
    midi = data.get("midi") or LDict()
    if "midi" in data and v.mapping(midi, "midi", data.line_of("midi"), ["transport"]) is None:
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
        usb = v.mapping(data["usb"], "usb", data.line_of("usb"), ["vid", "pid", "daw_port"])
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

    dev.scan_us = v.period(data, "scan_interval", "", 1, 0xFFFF, default=125)

    # hubs (before anything that refers to them), in file order
    for key in data:
        if key in HUB_ENUM:
            lst = v.seq(data[key], key, data.line_of(key))
            for i, h in enumerate(lst):
                validate_hub(v, dev, h, key, f"{key}[{i}]", lst.line_of(i))
    if len(dev.hubs) > 254:
        v.err("", data.line, f"{len(dev.hubs)} hubs: at most 254")
    resolve_switch_matrices(v, dev)

    # i2c
    need_i2c = any(h.type in I2C_HUBS for h in dev.hubs)
    if "i2c" in data:
        i2c = v.mapping(data["i2c"], "i2c", data.line_of("i2c"), ["sda", "scl"])
        if i2c is not None:
            sda = v.pin(i2c.get("sda"), "i2c.sda", i2c.line_of("sda"), "out", allow_hub=False)
            scl = v.pin(i2c.get("scl"), "i2c.scl", i2c.line_of("scl"), "out", allow_hub=False)
            if sda and scl:
                dev.i2c = (sda, scl)
            if not need_i2c:
                v.warn("i2c", data.line_of("i2c"), "no I2C hub (mcp23017 / pcf8574) uses it")
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

    preset_slots(v, dev, data)
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

    validate_modes(v, dev, data)
    validate_presets(v, dev, data)
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
    if not (dev.buttons or dev.analogs or dev.encoders or dev.matrix or dev.pads):
        v.warn("", data.line, "the device has no inputs (buttons, analog, encoders, pads or key_matrix)")
    return (None if errors(v.diags) else dev), v.diags


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
    fb = v.mapping(data["feedback"], "feedback", data.line_of("feedback"),
                   ["enabled", "script", "led_owner", "led_timeout"])
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


def validate_mcu(v, dev, data, tr, daw_port):
    """mcu: { extender } turns on Mackie Control. On USB it gets the second cable (usb.daw_port, default
    on with mcu:); on serial / BLE (one port) the whole device speaks MCU."""
    if "mcu" in data:
        m = data["mcu"]
        if m is None:
            m = LDict()
            m.line = data.line_of("mcu")
        m = v.mapping(m, "mcu", data.line_of("mcu"), ["extender"])
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


def validate_components(v, dev, data):
    names = {}       # (kind, display name) -> path, for the duplicate-name warning
    # LEDs first (buttons refer to them)
    leds = v.seq(data.get("leds"), "leds", data.line_of("leds"))
    for i, d in enumerate(leds):
        p = f"leds[{i}]"
        d = v.mapping(d, p, leds.line_of(i), ["id", "name", "pin", "pin2", "pin3", "feedback", "owner", "timeout",
                                                     "daw"],
                      renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "LED", len(dev.leds))
        name = v.display_name(d, p, ident)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "out")
        pin2 = v.pin(d["pin2"], f"{p}.pin2", d.line_of("pin2"), "out") if d.get("pin2") is not None else NO_PIN
        pin3 = v.pin(d["pin3"], f"{p}.pin3", d.line_of("pin3"), "out") if d.get("pin3") is not None else NO_PIN
        if d.get("pin3") is not None and d.get("pin2") is None:
            v.err(f"{p}.pin3", d.line_of("pin3"), "an RGB LED needs pin2 too (pin red, pin2 green, pin3 blue)")
        src = validate_led_source(v, dev, d, p)
        owner = validate_owner(v, d, p)
        timeout = validate_timeout(v, d, "timeout", p, None)
        if timeout is not None and owner in (LED_OWNERS["host"], LED_OWNERS["local"]):
            v.warn(f"{p}.timeout", d.line_of("timeout"), "only used with owner: host_then_local")
        led_owner_line = d.line_of("owner") if owner == LED_OWNERS["local"] else None
        dev.leds.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, pin2=pin2 or NO_PIN,
                            pin3=pin3 or NO_PIN, source=src,
                            owner=owner, timeout=timeout, path=p, line=d.line, used_by=[],
                            daw=daw_role(v, dev, d, p), local_line=led_owner_line))

    buttons = v.seq(data.get("buttons"), "buttons", data.line_of("buttons"))
    for i, d in enumerate(buttons):
        p = f"buttons[{i}]"
        d = v.mapping(d, p, buttons.line_of(i), ["id", "name", "pin", "action", "cc", "note", "preset", "led",
                                                 "mcu", "daw"],
                      renamed=RENAMED_BUTTON)
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
        dev.buttons.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, action=ACTION_IDS[act],
                               action_name=act, number=num, led=led, daw=daw_role(v, dev, d, p)))

    analogs = v.seq(data.get("analog"), "analog", data.line_of("analog"))
    for i, d in enumerate(analogs):
        p = f"analog[{i}]"
        d = v.mapping(d, p, analogs.line_of(i), ["id", "name", "pin", "cc", "mode", "adc_min", "adc_max",
                                                 "takeover", "mcu", "daw"], renamed=RENAMED_ANALOG)
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
        if mode != 0 and fader is not None:
            v.err(f"{p}.mode", d.line_of("mode"), "a Mackie Control fader (mcu:) sends its own messages: "
                                                  f"drop mode: {ANALOG_MODES[mode]} or mcu:")
        cc = v.integer(d, "cc", p, 0, 127, default=0, required=fader is None and mode == 0)
        lo = v.integer(d, "adc_min", p, 0, 4095, default=0)
        hi = v.integer(d, "adc_max", p, 0, 4095, default=4095)
        if lo == hi:
            v.err(p, d.line, f"adc_min and adc_max are both {lo}")
        pickup = validate_takeover(v, dev, d, p, fader)
        dev.analogs.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, cc=cc, mode=mode, raw_min=lo,
                               raw_max=hi, pickup=pickup, mcu=fader, daw=daw_role(v, dev, d, p)))

    encs = v.seq(data.get("encoders"), "encoders", data.line_of("encoders"))
    for i, d in enumerate(encs):
        p = f"encoders[{i}]"
        d = v.mapping(d, p, encs.line_of(i), ["id", "name", "pin_a", "pin_b", "cc", "steps_per_detent", "mcu",
                                              "ring", "ring_center", "daw"], renamed=RENAMED_ENCODER)
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
        cc = v.integer(d, "cc", p, 0, 127, default=0, required=vpot is None)
        spd = v.integer(d, "steps_per_detent", p, 1, 255, default=4)
        ring = encoder_ring(v, dev, d, p, vpot)
        center = ring_center(v, dev, d, p, vpot)
        dev.encoders.append(Hub(id=ident or "", name=name, a=pins["pin_a"] or NO_PIN, b=pins["pin_b"] or NO_PIN,
                                cc=cc, steps=spd, mcu=vpot, ring=ring, ring_center=center,
                                daw=daw_role(v, dev, d, p)))
    check_mcu_unique(v, dev, data)


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
    if not isinstance(role, str) or not (role in DAW_ROLES or DAW_ROLE_RE.fullmatch(role)):
        close = difflib.get_close_matches(str(raw), sorted(DAW_ROLES), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{p}.daw", line, f"unknown DAW role '{raw}' ({hint}roles: hosts/README.md; or x_<name> for "
                                "your own)")
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
            if base or k == "preset":
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
        return v.integer(d, other[0], p, 0, 127, default=0) if other[0] != "preset" else keep
    if "number" in d:          # format 1; reported as renamed
        return 0
    lo, hi = (1, dev.n_presets) if want == "preset" else (0, 127)
    if want not in d:
        if base and act == base[0]:
            return keep
        v.err(p, d.line, f"action {act} needs {want}: ({lo}..{hi}" +
              (", the preset slot" if want == "preset" else "") + ")")
        return 0
    return v.integer(d, want, p, lo, hi, default=0)


def validate_hub(v, dev, h, typ, p, line):
    index = len(dev.hubs)
    if not isinstance(h, dict):
        v.err(p, line, f"must be a mapping (got {type_name(h)})")
        return
    v.mapping(h, p, line, HUB_KEYS[typ], renamed=RENAMED_HUB)
    hid = v.ident(h, p, "hub", index, allow_name=False)
    hub = Hub(id=hid or f"#{index}", type=typ, index=index, path=p, line=line, used=False, chain=0,
              n_sel=0, addr=0, data=NO_PIN, clock=NO_PIN, latch=NO_PIN, sel=[], common=None,
              common_adc=False, enable=NO_PIN, settle_us=0, n_pins=0, en=[], group_lv=[], rows=[], cols=[],
              ram_bytes=0, intensity=0, grids=0, raw=h)
    if hid is not None:
        v.hub_by_id[hid] = hub
    dev.hubs.append(hub)

    def ctl(key, role, required=True, open_drain=False):
        if h.get(key) is None:
            if required:
                v.err(f"{p}.{key}", h.line, "is required")
            return NO_PIN
        return v.pin(h[key], f"{p}.{key}", h.line_of(key), role, allow_hub=False, line_kind=CTRL_LINE[key],
                     owner=index, open_drain=open_drain) or NO_PIN

    if typ in SHIFT_HUBS:
        hub.chain = v.integer(h, "sr_count", p, 1, 4, default=1)   # vpin.c: at most 4 chips (32 pins)
        hub.n_pins = 8 * hub.chain
        hub.data = ctl("data_pin", "out" if typ == "sn74hc595" else "in")
        hub.clock = ctl("clock_pin", "out")
        if typ == "sn74hc595":
            hub.latch = ctl("latch_pin", "out")
            hub.enable = ctl("oe_pin", "out", required=False)
        else:
            hub.latch = ctl("load_pin", "out")
    elif typ in TM_HUBS:
        hub.ram_bytes = v.integer(h, "ram_bytes", p, 1, TM_RAM_MAX, default=14)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = v.integer(h, "intensity", p, 0, 7, default=7)
        hub.grids = v.integer(h, "grids", p, 4, 7, default=0)   # 0: no display-mode command
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        hub.latch = ctl("stb_pin", "out")
    elif typ == "rgb_serial":
        hub.chain = v.integer(h, "count", p, 1, RGB_SERIAL_MAX, default=16)
        hub.n_pins = 3 * hub.chain
        hub.ram_bytes = (hub.n_pins + 7) // 8
        hub.data = ctl("tx_pin", "out")
        if hub.data.hub == 0 and h.get("tx_pin") is not None:
            why = v.plat.serial_tx(hub.data.num)
            if why:
                v.err(f"{p}.tx_pin", h.line_of("tx_pin"), why)
    elif typ in MUX_HUBS:
        hub.n_sel = MUX_SELECT[typ]
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
    elif typ == "decoder":
        validate_decoder(v, hub, h, p, index)
    elif typ == "switch_matrix":
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
    else:
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x20)
        hub.n_pins = 16 if typ == "mcp23017" else 8
        ok = range(0x20, 0x28) if typ == "mcp23017" else list(range(0x20, 0x28)) + list(range(0x38, 0x40))
        if hub.addr not in ok:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for a {typ} "
                                                         f"(usually 0x20-0x27{'' if typ == 'mcp23017' else ', PCF8574A 0x38-0x3F'})")


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
        m = v.mapping(rows, p, line, ["decoder", "first", "count"])
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
    proj = v.mapping(data["project"], "project", data.line_of("project"), ["version"])
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


def validate_matrix(v, dev, d, line):
    p = "key_matrix"
    d = v.mapping(d, p, line, ["type", "rows", "columns", "contacts", "key_map", "keys", "first_note", "settle",
                               "velocity", "daw"], renamed=RENAMED_MATRIX)
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
    if len(lst) > 255:
        v.err(f"{p}.columns", d.line_of("columns"), f"{len(lst)} columns: at most 255")
    # MCU columns are a "column" line, so a switch_matrix can share them (allow_other_uses)
    pins["columns"] = [v.pin(x, f"{p}.columns[{i}]", lst.line_of(i), "in", hubs={"sn74hc165"}, why=why,
                             line_kind=None if isinstance(x, dict) and any(k in x for k in PIN_KEY_HUB)
                             else "column", owner="key_matrix") or NO_PIN
                       for i, x in enumerate(lst)]
    nr, nc = len(pins["rows"]), len(pins["columns"])
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
        if nr > 32:
            v.err(f"{p}.rows", d.line_of("rows"), f"{nr} rows: contacts: rows takes at most 32")
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
    vd = v.mapping(d.get("velocity"), p, d.line_of("velocity"), ["fast", "slow"],
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
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog", why="a pad is read with the ADC")
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



DISPLAY_KEYS = ["id", "type", "chars"]
DISPLAY_TYPES = {"segment14": 1}     # gm_display.h GM_DISPLAY_SEG14
SEG14_PINS = 15                      # segments a..m, then the dot
MAX_DISPLAY_CHARS = 16               # core/src/display.c MAXC
MAX_DISPLAYS = 1                     # the core only writes display 0 for now


def validate_displays(v, dev, data):
    """displays: character displays on hub output pins. chars: each character's first pin
    (segment a); the 14 segments a..m and the dot are that pin and the next 14."""
    lst = v.seq(data.get("displays"), "displays", data.line_of("displays"))
    for i, d in enumerate(lst):
        p = f"displays[{i}]"
        d = v.mapping(d, p, lst.line_of(i), DISPLAY_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "display", len(dev.displays))
        typ = d.get("type", "segment14")
        if typ not in DISPLAY_TYPES:
            v.err(f"{p}.type", d.line_of("type"), f"unknown display type '{typ}' (use {', '.join(DISPLAY_TYPES)})")
            continue
        chars = v.seq(d.get("chars"), f"{p}.chars", d.line_of("chars"))
        if not chars:
            v.err(f"{p}.chars", d.line_of("chars") or d.line, "needs chars: each character's first pin "
                                                              "(segment a), left to right")
            continue
        if len(chars) > MAX_DISPLAY_CHARS:
            v.err(f"{p}.chars", d.line_of("chars"), f"{len(chars)} characters: at most {MAX_DISPLAY_CHARS}")
            continue
        pins = []
        for j, raw in enumerate(chars):
            cp = f"{p}.chars[{j}]"
            line = chars.line_of(j)
            if not isinstance(raw, dict):
                v.err(cp, line, "must be a hub pin, e.g. { tm1628: leds_a, number: 0 }: the 15 pins from "
                                "it (segments a..m, then the dot) must be in a row")
                continue
            pin = v.pin(raw, cp, line, "out")
            if pin is None:
                continue
            if pin.hub == 0:
                v.err(cp, line, "must be a hub pin: the 15 pins from it (segments a..m, then the dot) "
                                "must be in a row")
                continue
            hub = dev.hubs[pin.hub - 1]
            if pin.num + SEG14_PINS > hub.n_pins:
                v.err(cp, line, f"pins {pin.num}..{pin.num + SEG14_PINS - 1} (segments a..m and the dot) "
                                f"don't fit on hub '{hub.id}' (0..{hub.n_pins - 1})")
                continue
            for k in range(1, SEG14_PINS):      # the first one was recorded by v.pin
                v.use(("hub", hub.index, pin.num + k), f"{hub.id}.{pin.num + k}", Use(cp, line))
            pins.append(pin)
        dev.displays.append(Hub(id=ident or "", type=DISPLAY_TYPES[typ], chars=pins, path=p, line=d.line))
    if len(dev.displays) > MAX_DISPLAYS:
        v.err("displays", data.line_of("displays"), f"{len(dev.displays)} displays: at most {MAX_DISPLAYS} "
                                                    "for now")


def display_tables(dev):
    """gm_displays[] (gm_display.h gm_display_t); nothing without displays."""
    if not dev.displays:
        return []
    c = ['#include "gm_display.h"']
    for i, x in enumerate(dev.displays):
        c.append(f"static const gm_pin_t display_{i}_chars[{len(x.chars)}] = {{")
        c += [f"    {pn.c()}, /* {_comment(pn.desc)} */" for pn in x.chars]
        c.append("};")
    c.append("const gm_display_t gm_displays[GM_N_DISPLAYS] = {")
    c += [f"    {{ GM_DISPLAY_SEG14, {len(x.chars)}, display_{i}_chars }}, /* {_comment(x.id)} */"
          for i, x in enumerate(dev.displays)]
    c += ["};", ""]
    return c

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
    dev.n_presets = min(slots or max(n_list, 1), MAX_PRESETS)
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
        vals = []
        if slot == 1:
            for k in d:
                if k in PRESET_GLOBALS or k in PRESET_COMPONENTS:
                    v.err(f"{p}.{k}", d.line_of(k), "preset 1 is the component defaults written in the rest "
                                                    "of the file, so it takes only name: and program: (change "
                                                    "the component itself, or put this in another preset)")
        else:
            vals = preset_values(v, dev, d, p)     # none: a named copy of preset 1 (fine)
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
                if "cc" in o:
                    put("analog.cc", idx, v.integer(o, "cc", cp, 0, 127), dev.analogs[idx].cc, f"{cp}.cc",
                        line("cc"), f"analog.cc[{cid}]")
                if "mode" in o:
                    m = enum_value(v, o, "mode", f"{cp}.mode", ANALOG_MODES)
                    put("analog.mode", idx, m, dev.analogs[idx].mode, f"{cp}.mode", line("mode"),
                        f"analog.mode[{cid}]", None if m is None else ANALOG_MODES[m])
            elif sec == "pads":
                put("pad.note", idx, v.integer(o, "note", cp, 0, 127), dev.pads[idx].note, f"{cp}.note",
                    line("note"), f"pad.note[{cid}]")
            else:
                e = dev.encoders[idx]
                if "cc" in o:
                    put("encoder.cc", idx, v.integer(o, "cc", cp, 0, 127), e.cc, f"{cp}.cc", line("cc"),
                        f"encoder.cc[{cid}]")
                if "mode" in o:
                    m = enum_value(v, o, "mode", f"{cp}.mode", ENC_MODES)
                    put("encoder.mode", idx, m, 0, f"{cp}.mode", line("mode"), f"encoder.mode[{cid}]",
                        None if m is None else ENC_MODES[m])
    return out


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
                "channel": (10, "channel"), "velocity_curve": (11, "curve")}
MK_MODE = 12                   # GM_MK_MODE: go to a sub-mode
MODE_KEYS = ["id", "toggle", "parent", "led", "preset_blink", "keys"]
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
    if dev.matrix is None:
        v.err("modes", data.line_of("modes"), "modes turn the keys into function keys: the device needs a "
                                              "key_matrix")
        return
    n_errors = len(errors(v.diags))
    # pass 1: ids, toggles and parents
    modes, by_id = [], {}
    for i, d in enumerate(lst):
        p = f"modes[{i}]"
        d = v.mapping(d, p, lst.line_of(i), MODE_KEYS)
        if d is None:
            continue
        if "id" not in d:
            v.err(p, d.line, "needs id: (e.g. `id: edit`; keys go to a sub-mode by its id)")
            continue
        mid = d["id"]
        why = id_problem(mid, d.raw_of("id"))
        if why:
            v.err(f"{p}.id", d.line_of("id"), why)
            continue
        if mid in by_id:
            v.err(f"{p}.id", d.line_of("id"), f"mode '{mid}' is already {by_id[mid].path}: mode ids are unique")
            continue
        m = Hub(id=mid, path=p, line=d.line, d=d, index=len(modes), toggle=None, parent=None, root=None,
                led=None, blink=False, show=None, keys=[])
        has_t, has_p = d.get("toggle") is not None, d.get("parent") is not None
        if has_t == has_p:
            v.err(p, d.line, f"mode '{mid}' needs " + (
                "toggle: or parent:, not both" if has_t else
                "toggle: { button: <id> } (the button that enters and leaves it) or parent: <mode id> "
                "(a sub-mode, entered from a key of another mode)"))
        elif has_t:
            mode_toggle(v, dev, m, d, p)
        modes.append(m)
        by_id[mid] = m
    for m in modes:
        d = m.d
        if d.get("parent") is None or d.get("toggle") is not None:
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
    # pass 2: LEDs and keys (a key may name a sub-mode further down the file)
    for m in modes:
        d, p = m.d, m.path
        if d.get("led") is not None:
            r = mode_led_ref(v, d, f"{p}.led")
            if r:
                m.led, m.blink = r
        if d.get("preset_blink") is not None:
            mode_show(v, dev, m, d, p)
        keys = d.get("keys")
        if keys is None:
            v.warn(p, d.line, f"mode '{m.id}' has no keys: it does nothing")
        elif not isinstance(keys, dict):
            v.err(f"{p}.keys", d.line_of("keys"), "must be a mapping of keys to what they do, e.g. "
                                                   "`keys: { C3: octave_reset, E3: { mode: pick } }`")
        else:
            mode_keys(v, dev, m, keys, f"{p}.keys", by_id)
    targets = {k[2] for m in modes for k in m.keys if k[1] == MK_MODE}
    for m in modes:
        if m.parent is not None and m.index not in targets:
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
    dev.modes = modes


def mode_toggle(v, dev, m, d, p):
    """toggle: { button: <id> } (or toggle: <id>): the button that enters and leaves a root mode."""
    t, tl = d["toggle"], d.line_of("toggle")
    if isinstance(t, dict):
        if v.mapping(t, f"{p}.toggle", tl, ["button"]) is None:
            return
        if "button" not in t:
            v.err(f"{p}.toggle", tl, "needs button: (e.g. `toggle: { button: edit }`)")
            return
        holder = t
    else:
        holder = LDict(button=t)
        holder.line = tl
    idx = v.ref(holder, "button", f"{p}.toggle.button", holder.line_of("button"), "button", "buttons")
    if idx is None:
        return
    b = dev.buttons[idx]
    if idx in v.mode_toggles:
        v.err(f"{p}.toggle", tl, f"button '{b.id}' already toggles mode '{v.mode_toggles[idx]}'")
    elif b.action_name != "none":
        v.err(f"{p}.toggle", tl, f"button '{b.id}' toggles this mode, so its own action would never run: remove "
                                 f"`action: {b.action_name}` (and its cc:/note:) from the button")
    else:
        v.mode_toggles[idx] = m.id
        m.toggle = idx


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


def mode_tables(dev):
    """gm_modes[] (gm_device.h gm_mode_t), with each mode's key bindings sorted by key."""
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
        show = m.show or (-1, -1)
        toggle = "GM_MODE_NONE" if m.toggle is None else m.toggle
        parent = "GM_MODE_NONE" if m.parent is None else m.parent
        rows.append(f"    {{ {toggle}, {parent}, {m.root}, {-1 if m.led is None else m.led}, {int(m.blink)}, "
                    f"{show[0]}, {show[1]}, {len(m.keys)}, {arr} }}, /* {_comment(m.id)} */")
    c += ["/* { toggle button, parent, root, led, led_blink, blink_count, blink_done, n_keys, keys } */",
          "const gm_mode_t gm_modes[GM_N_MODES] = {", *rows, "};", ""]
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
        per = 2 if name in WIDE_PARAMS else 4
        words += (1 if per == 4 else 2) if c == 1 else 1 + -(-c // 32) + -(-c // per)
    return 4 * (REC_HEADER_WORDS + REC_NAME_WORDS + words + REC_CRC_WORDS)


def storage_need(plat_name, preset_rec, global_rec, slots):
    """-> (record_ok, used, capacity, unit) for slots presets + the globals at worst case.
    unit is "pages" for a journal, "B" otherwise."""
    b = STORE_BUDGETS.get(plat_name)
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
    limit = b.get("record", b["total"])
    return big <= limit, total, b["total"], "B"


def storage_budget(dev):
    """The worst case of what the device stores, against its platform's store. -> dict (also shown by
    gemifi_build.py and the dashboard)."""
    n_preset, n_global = stored_counts(dev)
    b = STORE_BUDGETS.get(dev.platform.name)
    if b and b.get("format") == "packed":
        prec, grec = packed_record_bytes(dev, PRESET), packed_record_bytes(dev, GLOBAL)
    else:
        prec, grec = record_bytes(n_preset), record_bytes(n_global)
    ok, used, cap, unit = storage_need(dev.platform.name, prec, grec, dev.n_presets)
    fits = [s for s in range(1, MAX_PRESETS + 1)
            if (lambda r: r[0] and r[1] <= r[2])(storage_need(dev.platform.name, prec, grec, s))]
    calm = [s for s in fits if storage_need(dev.platform.name, prec, grec, s)[1] <=
            BUDGET_WARN * storage_need(dev.platform.name, prec, grec, s)[2]]
    return {"slots": dev.n_presets, "preset_values": n_preset, "global_values": n_global,
            "preset_record": prec, "global_record": grec, "record_ok": ok,
            "record_limit": None if b is None else b.get("record") or b.get("page") or b["total"],
            "worst_bytes": dev.n_presets * prec + grec, "used": used, "capacity": cap, "unit": unit,
            "fits": ok and used <= cap, "max_slots": max(fits) if fits else 0,
            "max_slots_calm": max(calm) if calm else 0, "store": b and b["what"],
            "page": b and b.get("page")}


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
    if bd["store"] is None:
        return
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
    else:
        need = f"{bd['slots']} presets + the globals need up to {bd['worst_bytes']} of {bd['capacity']} B"
    if not bd["fits"]:
        v.err(path, line, f"presets don't fit the {where}: {need}; {recs}. At most "
                          f"{bd['max_slots']} slots fit (slots: {bd['max_slots']})")
    elif bd["used"] > BUDGET_WARN * bd["capacity"]:
        v.warn(path, line, f"presets use {100 * bd['used'] // bd['capacity']}% of the {where} at worst case: "
                           f"{need}, so a full store means more erase cycles. Up to {bd['max_slots']} slots "
                           f"fit; {bd['max_slots_calm']} or fewer stay under {int(BUDGET_WARN * 100)}%")


def load_device(path, text=None):
    """Load and validate a device YAML. Returns (Device or None, [Diag])."""
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
    return validate(data, path)


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
         f"#define GM_USB_VID 0x{dev.usb[0]:04X}",
         f"#define GM_USB_PID 0x{dev.usb[1]:04X}",
         f"#define GM_N_HUBS {len(dev.hubs)}",
         f"#define GM_N_BUTTONS {len(dev.buttons)}",
         f"#define GM_N_LEDS {len(dev.leds)}",
         f"#define GM_N_ANALOGS {len(dev.analogs)}",
         f"#define GM_N_ENCODERS {len(dev.encoders)}",
         f"#define GM_HAS_MATRIX {1 if m else 0}",
         f"#define GM_MATRIX_KEYS {m.keys if m else 0}",
         f"#define GM_MATRIX_VELOCITY {1 if m and m.kind == 'velocity' else 0}",
         f"#define GM_HAS_I2C {1 if any(x.type in I2C_HUBS for x in dev.hubs) else 0}"]
    tm_ram = sum(x.ram_bytes for x in dev.hubs if x.type in RAM_HUBS)
    if tm_ram:
        h.append(f"#define GM_TM1628_RAM {tm_ram} /* RAM bytes of all tm1628 and rgb_serial hubs */")
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
    h += pad_defines(dev)
    h += ["", f"#define GM_N_DISPLAYS {len(dev.displays)} /* displays: (core/README.md \"Displays\") */"]
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
            if hb.type != "switch_matrix" or not hb.rows or not hb.cols:
                continue
            for nm, lst in (("rows", hb.rows), ("cols", hb.cols)):
                c.append(f"static const gm_pin_t hub{hb.index + 1}_{nm}[{len(lst)}] = {{")
                c += [f"    {x.c()}, /* {_comment(x.desc)} */" for x in lst]
                c += ["};"]
        c.append("static const gm_hub_t hubs[GM_N_HUBS] = {")
        for hb in dev.hubs:
            sel = [s.c() for s in hb.sel] + ["GM_NO_PIN"] * (4 - len(hb.sel))
            extra = []
            if hb.type == "decoder":
                en = [x.c() for x in hb.en] + ["GM_NO_PIN"] * (3 - len(hb.en))
                lv = [str(x) for x in hb.group_lv] + ["0"] * (4 - len(hb.group_lv))
                extra = [f"        .en = {{ {', '.join(en)} }}, .n_en = {len(hb.en)},",
                         f"        .n_groups = {len(hb.group_lv)}, .group_lv = {{ {', '.join(lv)} }},"]
            elif hb.type == "switch_matrix" and hb.rows and hb.cols:
                n = hb.index + 1
                extra = [f"        .rows = hub{n}_rows, .cols = hub{n}_cols, .n_rows = {len(hb.rows)}, "
                         f".n_cols = {len(hb.cols)}, .rows_per_scan = {hb.rows_per_scan or 0},"]
            c += [f"    {{ /* {hb.index + 1}: {_comment(hb.id)} ({hb.type}) */",
                  f"        .type = {HUB_ENUM[hb.type]}, .chain = {hb.chain}, .n_sel = {hb.n_sel}, "
                  f".i2c_addr = 0x{hb.addr:02X},",
                  f"        .data = {hb.data.c()}, .clock = {hb.clock.c()}, .latch = {hb.latch.c()},",
                  f"        .sel = {{ {', '.join(sel)} }},",
                  f"        .common = {(hb.common or NO_PIN).c()}, .enable = {hb.enable.c()},",
                  f"        .settle_us = {hb.settle_us},",
                  *extra,
                  *([f"        .ram_bytes = {hb.ram_bytes}, .brightness = {hb.intensity}, "
                     f".grids = {hb.grids},"] if hb.type in TM_HUBS else []),
                  *([f"        .ram_bytes = {hb.ram_bytes},"] if hb.type == "rgb_serial" else []),
                  "    },"]
        c += ["};", ""]
    if dev.buttons:
        c.append("static const gm_button_t buttons[GM_N_BUTTONS] = {")
        c += [f"    {{ {pin_c(b.pin)}, {c_str(b.name)}, {b.led} }}, /* {_comment(b.id)}: {_comment(b.pin.desc)} */"
              for b in dev.buttons]
        c += ["};", ""]
    if dev.leds:
        c.append("static const gm_led_t leds[GM_N_LEDS] = {")
        c += [f"    {{ {pin_c(x.pin)}, {pin_c(x.pin2)}, {pin_c(x.pin3)} }}, /* {_comment(x.id)}: {_comment(x.pin.desc)} */"
              for x in dev.leds]
        c += ["};", ""]
    if dev.analogs:
        c.append("static const gm_analog_t analogs[GM_N_ANALOGS] = {")
        c += [f"    {{ {pin_c(a.pin)}, {c_str(a.name)}, {a.raw_min}, {a.raw_max} }}, "
              f"/* {_comment(a.id)}: {_comment(a.pin.desc)} */"
              for a in dev.analogs]
        c += ["};", ""]
    if dev.encoders:
        c.append("static const gm_encoder_t encoders[GM_N_ENCODERS] = {")
        c += [f"    {{ {pin_c(e.a)}, {pin_c(e.b)}, {c_str(e.name)}, {e.steps} }}, "
              f"/* {_comment(e.id)}: {_comment(e.a.desc)}, {_comment(e.b.desc)} */" for e in dev.encoders]
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
    c += feedback_tables(dev)
    c += preset_tables(dev)
    c += mode_tables(dev)
    c += pad_tables(dev)
    c += display_tables(dev)
    return {"gm_device_config.h": "\n".join(h), "gm_device_config.c": "\n".join(c)}


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
            ""]


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
    for x in dev.leds:
        src = x.source or (0, 0, 0)
        owner = x.owner
        if owner is None:
            mcu_driven = src[0] == LED_SOURCES["mcu"] or any(
                k == "ring" or a == "mcu" for k, a in x.used_by)
            owner = LED_OWNERS["host"] if mcu_driven else dev.led_owner
        timeout = dev.led_timeout if x.timeout is None else x.timeout
        if src[0] or owner != dev.led_owner or timeout != dev.led_timeout:
            special = True
        rows.append((src, owner, timeout))
    return rows if special else None


def feedback_defines(dev):
    mcu = dev.mcu
    pickup = any(a.pickup for a in dev.analogs)
    h = ["", "/* host feedback / DAW (core/README.md \"DAW feedback\") */",
         f"#define GM_FEEDBACK {1 if f1_on(dev) else 0}",
         f"#define GM_LED_FB {1 if led_fb_rows(dev) else 0}",
         f"#define GM_LED_OWNER {dev.led_owner} /* {OWNER_NAMES[dev.led_owner]} */",
         f"#define GM_LED_TIMEOUT {dev.led_timeout} /* {fmt_timeout(dev.led_timeout)}, 10 ms units */",
         f"#define GM_ANALOG_PICKUP {1 if pickup else 0}",
         f"#define GM_MCU {1 if mcu else 0}",
         f"#define GM_HOST_FB {1 if dev.host_fb else 0} /* DAW-script messages (sysex.md 4.6) */",
         f"#define GM_RINGS {1 if mcu or dev.host_fb else 0}",
         f"#define GM_USB_DAW_PORT {1 if dev.daw_port else 0}"]
    if mcu:
        h += [f"#define GM_MCU_CABLE {mcu.cable} /* {'the DAW port' if mcu.cable else 'the only port'} */",
              f"#define GM_MCU_ID 0x{0x15 if mcu.extender else 0x14:02X} /* {'MCU XT' if mcu.extender else 'MCU'} */"]
    if dev.daw_port:
        h.append(f"#define GM_DAW_PORT_NAME {c_str(dev.name + ' DAW')}")
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
    """A component's role for GET_COMPONENT: daw:, else its mcu: name, else empty (sysex.md 4.6.1)."""
    return getattr(x, "daw", None) or mcu_name or ""


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
