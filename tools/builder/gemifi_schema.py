"""The JSON schema of the device YAML, for editors (VS Code's YAML extension: yaml-language-server).

Built from the validator's own tables in gemifi_device.py (TOP_KEYS, BUTTON_KEYS, HUB_KEYS, ACTIONS,
CURVES, ...), so a key the validator learns shows up here too. The validator stays the authority:
the schema gives completion and catches unknown keys and wrong types, the builder checks the rest.

    gemifi_build.py schema -o site/src/schema/device.json   # published at SCHEMA_URL

tests/test_packages.py checks that the file in site/ is current and that the schema's keys are the
validator's on every device in devices/.
"""
import json

import gemifi_device as gd
import gemifi_yaml as gy

SCHEMA_URL = "https://cubicecho.github.io/gemifi/schema/device.json"
SCHEMA_FILE = "site/src/schema/device.json"     # relative to the repo
REUSE_KEYS = ["substitutions", "packages", "min_version"]   # gemifi_yaml.py uses them up


def _sub():
    return {"$ref": "#/definitions/substitution"}


def T(kind, **kw):
    """A scalar type that may also be a ${substitution}."""
    return {"anyOf": [dict({"type": kind}, **kw), _sub()]}


def enum(values):
    return {"anyOf": [{"enum": list(values)}, _sub()]}


def ref(name):
    return {"$ref": f"#/definitions/{name}"}


def obj(keys, types=None, desc=None, extra=None):
    """A mapping with exactly `keys` (the validator's list), typed from `types` where known."""
    types = types or {}
    s = {"type": "object", "properties": {k: types.get(k, KEY_TYPES.get(k, {})) for k in keys},
         "additionalProperties": False}
    if desc:
        s["description"] = desc
    if extra:
        s.update(extra)
    return s


def lst(item, desc=None):
    s = {"type": "array", "items": item}
    if desc:
        s["description"] = desc
    return s


def one_of(*alts):
    return {"anyOf": list(alts)}


INT7 = T("integer", minimum=0, maximum=127)
INT14 = T("integer", minimum=0, maximum=16383)
NOTE = one_of(T("integer", minimum=0, maximum=127), {"type": "string"})
PERIOD = ref("period")
BOOL = T("boolean")
STR = {"type": "string"}
ID_REF = {"type": "string", "description": "the id of another component"}
LOOK = {"type": ["string", "boolean"], "description": "an LED look: off, on, or a colour (" +
        ", ".join(n for n in gd.LED_LOOK_NAMES if n != "off") + ")"}

# What a key means wherever it appears, unless the section says otherwise (obj(types=...)).
KEY_TYPES = {
    "id": ref("id"), "name": STR, "pin": ref("pin"), "pin2": ref("pin"), "pin3": ref("pin"),
    "pin_a": ref("pin"), "pin_b": ref("pin"), "cc": INT7, "note": NOTE, "preset": T("integer", minimum=1),
    "action": enum(gd.ACTIONS), "led": ID_REF, "daw": STR, "mcu": STR, "idle": LOOK, "active": LOOK,
    "channel": T("integer", minimum=0, maximum=16), "settle": PERIOD, "delay": PERIOD, "timer": {},
    "dma": BOOL, "address": T("integer"), "count": T("integer", minimum=1),
    # MIDI messages (core/README.md "MIDI messages")
    "program": INT7, "mmc": one_of(enum(gd.MMC_COMMANDS), T("integer", minimum=1, maximum=127)),
    "sysex": {"type": "string", "description": "the id of a midi_messages.sysex string"},
    "nrpn": INT14, "rpn": INT14,
}
KEY_TYPES["bank"] = one_of(INT14, obj(gd.BANK_KEYS, {"msb": INT7, "lsb": INT7}))
# channel:, min: and max: on a button, analog input or encoder (build time only)
CTL_TYPES = {"channel": T("integer", minimum=1, maximum=16), "min": INT14, "max": INT14}

# hub keys: pins are `*_pin` and `pin_s*`
HUB_TYPES = {"sr_count": T("integer", minimum=1, maximum=4), "ram_bytes": T("integer", minimum=1),
             "intensity": T("integer", minimum=0, maximum=7), "grids": T("integer", minimum=4, maximum=7),
             "fade": BOOL,
             "touch_min": T("integer", minimum=0, maximum=4095), "touch_max": T("integer", minimum=0, maximum=4095),
             "rows_per_scan": T("integer", minimum=1, maximum=32), "groups": {}}


def hub(typ):
    keys = gd.HUB_KEYS[typ]
    types = {k: ref("pin") for k in keys if k.endswith("_pin") or k.startswith("pin_")}
    types.update({k: v for k, v in HUB_TYPES.items() if k in keys})
    types.update({k: v for k, v in matrix_lines().items() if k in keys})
    if typ in gd.ROW_HUBS:
        types["address"] = lst(ref("pin"), "the address pins, A0 first")
    elif "address" in keys:
        types["address"] = T("integer", minimum=0x08, maximum=0x77)
    types["id"] = ref("id")
    return lst(obj(keys, types), f"{typ} hubs (core/README.md \"Virtual pins and hubs\")")


def matrix_lines():
    """rows: (pins or decoder outputs) and columns: of key_matrix and switch_matrix hubs."""
    return {"rows": one_of(lst(ref("pin")), obj(gd.DECODER_ROWS_KEYS, {"decoder": ID_REF,
                                                                     "first": T("integer", minimum=0)})),
            "columns": lst(ref("pin"))}


def accel():
    return one_of(enum(gd.ACCEL_PRESETS), obj(gd.ACCEL_KEYS, {"slow": PERIOD, "fast": PERIOD,
                                                               "max": T("integer", minimum=1, maximum=32),
                                                               "curve": enum(gd.ACCEL_CURVES)}))


def presets():
    comps = {}
    for sec, (_kind, _pl, allowed, _hw) in gd.PRESET_COMPONENTS.items():
        types = {"mode": enum(gd.ENC_MODES if sec == "encoders" else gd.ANALOG_MODES)}
        comps[sec] = {"type": "object", "description": f"per {sec[:-1] if sec.endswith('s') else sec} id",
                      "additionalProperties": obj(allowed, types)}
    globs = {}
    for sec, keys in gd.PRESET_GLOBALS.items():
        types = {}
        for k, (_p, _d, kind, lo, hi) in keys.items():
            types[k] = (enum(kind) if isinstance(kind, list) else PERIOD if kind == "ms" else BOOL
                        if kind == "bool" else T("integer", minimum=lo, maximum=hi))
        globs[sec] = obj(list(keys), types)
    item = obj(gd.PRESET_KEYS, {"program": INT7, **globs, **comps})
    return obj(gd.PRESETS_KEYS, {"slots": T("integer", minimum=1, maximum=gd.MAX_PRESETS),
                                 "revision": T("integer", minimum=0, maximum=gd.PRESET_REVISION_MAX),
                                 "list": lst(item)}, "preset slots (core/README.md \"Presets\")")


def modes():
    toggle = one_of(ID_REF, obj(["button", "with"], {"button": ID_REF, "with": ID_REF}))
    layer_entry = obj(sorted(set(gd.LAYER_KEYS) | set(gd.LAYER_PAD_KEYS) | set(gd.LAYER_ENCODER_KEYS)),
                      {"name": STR, "cc": one_of(INT7, {"const": "none"}), "preset": T("integer", minimum=1)})
    step = obj(gd.STEP_KEYS, {"enabled": BOOL, "colour": LOOK, "layer": {"type": "object",
                                                                          "additionalProperties": layer_entry}})
    item = obj(gd.MODE_KEYS, {
        "toggle": toggle, "hold": one_of(ID_REF, obj(["button"], {"button": ID_REF})), "cycle": toggle,
        "parent": ID_REF, "preset_blink": {},
        "led": one_of(ID_REF, {"type": "object", "description": "{ <LED id>: blink }"}),
        "keys": {}, "buttons": {"type": "object", "description": "button id: what it does in the mode"},
        "layer": {"type": "object", "description": "component id: what it does on the layer",
                  "additionalProperties": layer_entry},
        "steps": lst(step)})
    return lst(item, "modes, layers and cycles (core/README.md \"Modes\")")


def layout():
    """layout: (core/README.md "Layout"): [x, y], [x, y, w, h] or { x, y, w, h, shape } per control id."""
    unit = T("integer", minimum=0, maximum=gd.LAYOUT_MAX)
    size = T("integer", minimum=1, maximum=gd.LAYOUT_MAX)
    def ints(n):
        return {"type": "array", "items": {"type": "integer", "minimum": 0, "maximum": gd.LAYOUT_MAX},
                "minItems": n, "maxItems": n}
    at = [ints(2), ints(4)]          # [x, y] or [x, y, w, h]
    place = obj(gd.LAYOUT_ITEM_KEYS, {"x": unit, "y": unit, "w": size, "h": size,
                                      "shape": enum(s for s in gd.LAYOUT_SHAPES if s != "keys")},
                extra={"required": ["x", "y"]})
    rect = one_of(*at, place)
    return obj(gd.LAYOUT_KEYS, {
        "size": {"type": "array", "items": {"type": "integer", "minimum": 1, "maximum": gd.LAYOUT_MAX},
                 "minItems": 2, "maxItems": 2, "description": "[w, h]: the drawing's size in units"},
        "keys": one_of(*at, obj(gd.LAYOUT_ITEM_KEYS, {"x": unit, "y": unit, "w": size, "h": size,
                                                       "shape": enum(["keys"])}, extra={"required": ["x", "y"]})),
        "controls": {"type": "object", "description": "control id -> [x, y], [x, y, w, h] or { x, y, w, h, shape }",
                     "propertyNames": {"pattern": "^" + gd.ID_RE.pattern + "$"},
                     "additionalProperties": rect},
    }, "where the controls are, in units of about 5 mm from the top left, for drawing the device "
       "(core/README.md \"Layout\")")


def pin():
    return one_of(
        {"type": "string", "description": "an MCU pin: PA1, GPIO5, 5"},
        {"type": "integer"},
        obj(gd.PIN_KEYS, {"number": one_of(STR, {"type": "integer"}), "inverted": BOOL,
                          "allow_other_uses": BOOL, "mode": one_of(enum(gd.MODE_STRINGS), obj(gd.MODE_FLAGS, {
                              k: BOOL for k in gd.MODE_FLAGS})),
                          **{k: ID_REF for k in gd.PIN_KEY_HUB}}))


def device_schema():
    top_types = {
        "gemifi": {"enum": [gd.FORMAT_VERSION], "description": "the format version"},
        "name": {"type": "string", "pattern": "^" + gd.NODE_NAME_RE.pattern + "$",
                 "description": "node name: lowercase letters, digits and -, at most 31"},
        "friendly_name": {"type": "string", "description": "display name (USB product string, HELLO)"},
        "description": STR,
        "project": obj(gd.PROJECT_KEYS, {"version": {"type": "string", "pattern": r"^\d+\.\d+\.\d+$"}}),
        "board_id": T("integer", minimum=0, maximum=0x3FFF),
        "platform": enum(gd.PLATFORMS),
        "chip": STR,
        "board": {"enum": [b for p in gd.gbd.PLATFORMS for b in gd.gbd.boards(p)],
                  "description": "the board profile (platform/<platform>/boards/<board>.json: layout and board "
                                 "pins; " + ", ".join(f"{p}: {b}" for p, b in gd.gbd.DEFAULT_BOARD.items()) +
                                 " when left out)"},
        "usb": obj(gd.USB_KEYS, {"vid": T("integer", minimum=0, maximum=0xFFFF),
                                 "pid": T("integer", minimum=0, maximum=0xFFFF), "daw_port": BOOL}),
        "midi": obj(gd.MIDI_KEYS, {"transport": enum(gd.MIDI_TRANSPORT_MACRO)}),
        "i2c": obj(gd.I2C_KEYS, {"sda": ref("pin"), "scl": ref("pin")}),
        "scan_interval": PERIOD,
        "hardware_scan": obj(gd.HW_SCAN_KEYS, {
            "matrix": obj(gd.HW_SCAN_MATRIX_KEYS, {"row_time": PERIOD, "precharge": BOOL}),
            "analog": one_of(BOOL, obj(gd.HW_SCAN_ANALOG_KEYS))}),
        "boot_combo": lst(ID_REF),
        # safety (core/README.md "Factory-reset combo", "Physical unlock", "Status LED")
        "factory_reset_combo": one_of(
            lst(ID_REF, "buttons held at power-up to wipe the settings (5 s)"),
            obj(gd.COMBO_KEYS, {"combo": lst(ID_REF), "hold": PERIOD}, "combo, and hold: 1s..30s (default 5s)")),
        "unlock": one_of(
            {"type": "boolean", "description": "true: the boot_combo buttons, 2 s; false/left out: off"},
            obj(gd.COMBO_KEYS, {"combo": lst(ID_REF), "hold": PERIOD},
                "opt-in physical unlock: combo (default boot_combo), hold: 0s..10s (default 2s)")),
        "status_led": ID_REF,
        "feedback": one_of(BOOL, obj(gd.FEEDBACK_KEYS, {"enabled": BOOL, "script": BOOL,
                                                        "led_owner": enum(gd.LED_OWNERS),
                                                        "led_timeout": one_of(PERIOD, {"const": "never"})})),
        "mcu": one_of({"type": "null"}, obj(gd.MCU_KEYS, {"extender": BOOL})),
        **{t: hub(t) for t in gd.HUB_TYPES},
        "buttons": lst(obj(gd.BUTTON_KEYS, CTL_TYPES)),
        "led_defaults": obj(gd.LED_DEFAULTS_KEYS),
        "leds": lst(obj(gd.LED_KEYS, {
            "feedback": obj([*gd.LED_SOURCES, "channel"], {"channel": T("integer", minimum=1, maximum=16)}),
            "owner": enum(gd.LED_OWNERS), "timeout": one_of(PERIOD, {"const": "never"})})),
        "analog": lst(obj(gd.ANALOG_KEYS, {**CTL_TYPES, "mode": enum(gd.ANALOG_MODES),
                                           "adc_min": T("integer", minimum=0, maximum=4095),
                                           "adc_max": T("integer", minimum=0, maximum=4095),
                                           "takeover": enum(gd.TAKEOVER), "touch": enum(gd.TOUCH)})),
        "encoders": lst(obj(gd.ENCODER_KEYS, {**CTL_TYPES, "mode": enum(gd.ENC_MODES), "steps_per_detent": T("integer", minimum=1, maximum=255),
                                              "ring": lst(ID_REF), "ring_center": ID_REF,
                                              "acceleration": accel()})),
        "encoder_settings": obj(gd.ENCODER_SETTINGS_KEYS, {"acceleration": accel()}),
        "key_matrix": obj(gd.MATRIX_KEYS, {
            "type": enum(["switch", "velocity"]), "contacts": enum(["columns", "rows"]), **matrix_lines(),
            "key_map": lst(lst(one_of(INT7, {"type": "null"}))),
            "keys": T("integer", minimum=1, maximum=128), "first_note": INT7,
            "velocity": obj(gd.VELOCITY_KEYS, {"fast": PERIOD, "slow": PERIOD})}),
        "pads": lst(obj(gd.PAD_KEYS)),
        "pad_settings": obj(gd.PAD_SETTINGS_KEYS, {"curve": enum(gd.CURVES), "aftertouch": BOOL,
                                                   "threshold": T("integer", minimum=1, maximum=4095),
                                                   "release": T("integer", minimum=0, maximum=4094),
                                                   "full": T("integer", minimum=2, maximum=4095),
                                                   "window": PERIOD, "retrigger": PERIOD}),
        "presets": presets(),
        "modes": modes(),
        "displays": lst(obj(gd.DISPLAY_KEYS, {"type": enum(gd.DISPLAY_TYPES), "shows": enum(gd.DISPLAY_SHOWS),
                                              "segment_step": T("integer", minimum=1, maximum=64),
                                              "chars": lst(one_of(ref("pin"), lst(ref("pin"))),
                                                           "each character's first pin, or its segments' pins")})),
        "din_midi": obj(gd.DIN_KEYS, {"tx_pin": ref("pin"), "rx_pin": ref("pin"), "mode": enum(gd.DIN_MODES),
                                      "port_name": STR, "thru": BOOL, "controller_out": BOOL,
                                      "usb_to_din": BOOL}),
        "midi_messages": obj(gd.MIDI_MESSAGES_KEYS, {
            "enable": lst(enum([*gd.MSG_FEATURES, *gd.MSG_ALIASES])), "null_rpn": BOOL,
            "mmc_device_id": INT7,
            "sysex": lst(obj(gd.SYSEX_KEYS, {"data": {"type": "string", "description": "hex bytes F0 .. F7, "
                                                      "with v, msb, lsb, ch and [ ... ] cs placeholders"}}))},
            "MIDI messages: 14-bit CC, NRPN/RPN, bank select, panic, MMC, SysEx (core/README.md \"MIDI messages\")"),
        "layout": layout(),
        "write_to_flash": one_of(BOOL, obj(gd.WTF_KEYS, {**{k: BOOL for k in gd.WTF_ITEMS},
                                                         "delay": PERIOD, "idle": PERIOD})),
    }
    # the keys gemifi_yaml.py uses up before validation
    reuse = {
        "substitutions": {"type": "object", "description": "name: value, used as ${name} in string values",
                          "propertyNames": {"pattern": "^" + gy.SUB_NAME_RE.pattern + "$"},
                          "additionalProperties": {"type": ["string", "number", "boolean", "null"]}},
        "packages": {"description": "parts of a device file merged under this one: a file name, "
                                    f"{gy.PRESET_PREFIX}<name>, !include or a mapping",
                     "anyOf": [{"type": "object", "additionalProperties": {"type": ["string", "object"]}},
                               {"type": "array", "items": {"type": ["string", "object"]}}]},
        "min_version": {"type": "string", "pattern": r"^\d+\.\d+\.\d+$",
                        "description": "the oldest gemifi builder that can build this file"},
    }
    assert list(reuse) == REUSE_KEYS
    props = {**reuse, **{k: top_types.get(k, {}) for k in gd.TOP_KEYS}}
    # a section may be an `!include file.yaml` (a string to the editor)
    for k, s in props.items():
        if k not in reuse and s.get("type") in ("object", "array"):
            props[k] = one_of(s, {"type": "string", "description": "!include file.yaml"})
    return {
        "$schema": "https://json-schema.org/draft-07/schema#",
        "$id": SCHEMA_URL,
        "title": "gemifi device YAML",
        "description": f"gemifi: {gd.FORMAT_VERSION} device files (core/README.md \"Device YAML\"). Generated by "
                       "tools/builder/gemifi_build.py schema: don't edit.",
        "type": "object",
        "properties": props,
        "additionalProperties": False,
        "definitions": {
            "substitution": {"type": "string", "pattern": r"\$\{[A-Za-z_][A-Za-z0-9_]*\}"},
            "id": {"type": "string", "pattern": "^" + gd.ID_RE.pattern + "$",
                   "description": "lower_snake_case, unique across the device"},
            "period": {"anyOf": [{"type": "string", "pattern": "^" + gd.TIME_RE.pattern + "$"}, _sub()],
                       "description": "a time period with a unit: 125us, 1.6ms, 2s"},
            "pin": pin(),
        },
    }


def schema_text():
    return json.dumps(device_schema(), indent=2, ensure_ascii=False) + "\n"
