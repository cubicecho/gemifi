"""The JSON schema of the device YAML, for editors (VS Code's YAML extension: yaml-language-server).

Built from the validator's own tables in gemifi_device.py (TOP_KEYS, BUTTON_KEYS, HUB_KEYS, ACTIONS,
CURVES, ...), so a key the validator learns shows up here too. The validator stays the authority:
the schema gives completion and catches unknown keys and wrong types, the builder checks the rest.
A component whose keys are data (components/keys.py: encoders, the hub types, the display platforms, the
i2c: buses) gives its own types, with each key's doc line and default for the editor's hover; the same data
is the key table on its docs page:

    gemifi_build.py schema -o site/src/schema/device.json   # published at SCHEMA_URL
    gemifi_build.py keys -o site/src/_data/component_keys.json

tests/test_packages.py checks that both files in site/ are current and that the schema's keys are the
validator's on every device in devices/.
"""
import json

import gemifi_device as gd
import gemifi_yaml as gy
import components
from components.keys import Section, T, enum, lst, one_of, ref, sub as _sub

SCHEMA_URL = "https://cubicecho.github.io/gemifi/schema/device.json"
SCHEMA_FILE = "site/src/schema/device.json"     # relative to the repo
KEYS_FILE = "site/src/_data/component_keys.json"
REUSE_KEYS = ["substitutions", "packages", "min_version"]   # gemifi_yaml.py uses them up


def obj(keys, types=None, desc=None, extra=None):
    """A mapping with exactly `keys` (the validator's list), typed from `types` where known. (A component's
    keys are typed by its spec, components/keys.py; this is for the device's own sections.)"""
    types = types or {}
    s = {"type": "object", "properties": {k: types.get(k, {}) for k in keys}, "additionalProperties": False}
    if desc:
        s["description"] = desc
    if extra:
        s.update(extra)
    return s



INT7 = T("integer", minimum=0, maximum=127)
PERIOD = ref("period")
BOOL = T("boolean")
STR = {"type": "string"}
ID_REF = {"type": "string", "description": "the id of another component"}
LOOK = gd.leds.Look().schema()

def hub(typ):
    return lst(obj(gd.HUB_KEYS[typ], hub_key_types(typ)), f"{typ} hubs (core/README.md \"Components\")")


def hub_domain(domain):
    """A domain's list (led_strip: ...): an item takes platform: and the keys of its platforms."""
    plats = gd.HUB_DOMAINS[domain]
    keys = list(dict.fromkeys(k for typ in plats for k in gd.HUB_KEYS[typ]))
    types = {}
    for typ in plats:
        for k, t in hub_key_types(typ).items():
            types[k] = widest(types[k], t) if k in types else t
    types["platform"] = enum(plats)
    item = by_platform(obj(keys, types), {typ: (gd.HUB_KEYS[typ], hub_key_types(typ)) for typ in plats})
    return lst(item, f"{domain} hubs, ESPHome style: {{ id, platform: {' | '.join(plats)}, ... }} "
                     f"(components/{domain}/README.md)")


def by_platform(item, plats):
    """An item of a list whose items each name a platform (led_strip:, led_driver:, display:): the keys of
    all of them (item), and for each platform (plats: name -> (its keys, their types)) only its own keys,
    typed and described as its spec says, so an editor offers and explains that platform's keys and flags
    another's (JSON schema if / then)."""
    item["allOf"] = [{"if": {"properties": {"platform": {"const": p}}, "required": ["platform"]},
                      "then": {"properties": {k: {} if k == "platform" else types.get(k, {}) for k in keys},
                               "additionalProperties": False}}
                     for p, (keys, types) in plats.items()]
    return item


def widest(a, b):
    """One key's type on two platforms: the wider of two integer ranges; untyped where they differ."""
    if a == b:
        return a
    a, b = bare(a), bare(b)     # one platform's doc line doesn't hold for the others
    if a == b:
        return a
    ia, ib = a.get("anyOf", [{}])[0], b.get("anyOf", [{}])[0]
    if ia.get("type") == ib.get("type") == "integer":
        lo = [x["minimum"] for x in (ia, ib) if "minimum" in x]
        hi = [x["maximum"] for x in (ia, ib) if "maximum" in x]
        return T("integer", **({"minimum": min(lo)} if len(lo) == 2 else {}),
                 **({"maximum": max(hi)} if len(hi) == 2 else {}))
    return {}


def bare(s):
    """A key's type without its doc line and default (Key.schema): a lone $ref unwrapped again."""
    s = {k: x for k, x in s.items() if k not in ("description", "default")}
    return s["anyOf"][0] if list(s) == ["anyOf"] and len(s["anyOf"]) == 1 else s


def hub_key_types(typ):
    """The JSON schema types of a hub type's keys, by key: its spec's (every hub type has one)."""
    return gd.HUB_DEFS[typ].spec.types()


def screen():
    """display: the screens, each on a platform (components/<platform>, its ScreenType's keys: their types
    from its spec, the wider where platforms share a key)."""
    plats = list(gd.DISPLAY_PLATFORMS.values())
    keys = list(dict.fromkeys(k for t in plats for k in t.keys))
    types = {}
    for t in plats:
        for k, s in t.spec.types().items():
            types[k] = widest(types[k], s) if k in types else s
    types["platform"] = enum(gd.DISPLAY_PLATFORMS)
    item = by_platform(obj(keys, types), {name: (t.keys, t.spec.types()) for name, t in gd.DISPLAY_PLATFORMS.items()})
    return lst(item, "screens, ESPHome style: { id, platform: " + " | ".join(gd.DISPLAY_PLATFORMS) +
               ", ... } (components/display/README.md); text displays name one with display: <id>")


def matrix_lines():
    """rows: (pins or decoder outputs) and columns: of key_matrix and switch_matrix hubs."""
    return {"rows": one_of(lst(ref("pin")), obj(gd.DECODER_ROWS_KEYS, {"decoder": ID_REF,
                                                                     "first": T("integer", minimum=0)})),
            "columns": lst(ref("pin"))}


def section_keys(yaml_key):
    """The Keys of a component's section (KEY_SECTIONS), by its YAML key."""
    return next(s.keys for s in key_sections() if s.yaml_key == yaml_key)


def action_list():
    """actions: (components/buttons/README.md "Action lists"): the messages a press sends, in order (the
    definition every actions: key refers to, components/buttons ACTION_LIST)."""
    return lst(one_of(enum(gd.buttons.ITEM_ACTIONS), gd.buttons.ITEM.schema()),
               "messages sent in order at the press: an action's name, or { cc / note / program / mmc / sysex, "
               "value, bank, channel }")


def presets():
    comps = {}
    for sec, (_kind, _pl, allowed, _hw) in gd.PRESET_COMPONENTS.items():
        spec = section_keys(sec)
        comps[sec] = {"type": "object", "description": f"per {sec[:-1] if sec.endswith('s') else sec} id",
                      "additionalProperties": obj(allowed, {k: spec[k].schema() for k in allowed})}
    globs = {}
    for sec, keys in gd.PRESET_GLOBALS.items():
        types = {}
        for k, (_p, _d, kind, lo, hi) in keys.items():
            types[k] = (enum(kind) if isinstance(kind, list) else PERIOD if kind == "ms" else BOOL
                        if kind == "bool" else T("integer", minimum=lo, maximum=hi))
        globs[sec] = obj(list(keys), types)
    item = obj(gd.PRESET_KEYS, {"name": STR, "program": INT7, "bank": gd.buttons.BANK.schema(), **globs, **comps})
    return obj(gd.PRESETS_KEYS, {"slots": T("integer", minimum=2, maximum=gd.MAX_PRESETS),
                                 "revision": T("integer", minimum=0, maximum=gd.PRESET_REVISION_MAX),
                                 "list": lst(item)}, "preset slots (core/README.md \"Presets\")")


def modes():
    toggle = one_of(ID_REF, obj(["button", "with"], {"button": ID_REF, "with": ID_REF}))
    # a layer entry takes keys of a button, a pad or an encoder (components/modes), typed as theirs
    layer_keys = sorted(set(gd.LAYER_KEYS) | set(gd.LAYER_PAD_KEYS) | set(gd.LAYER_ENCODER_KEYS))
    specs = [section_keys(s) for s in ("buttons", "pads", "encoders", "leds")]   # idle:/active: are an LED's
    layer_types = {k: next(sp[k].schema() for sp in specs if k in sp.by_name) for k in layer_keys
                   if any(k in sp.by_name for sp in specs)}
    layer_entry = obj(layer_keys, {**layer_types, "cc": one_of(INT7, {"const": "none"})})
    step = obj(gd.STEP_KEYS, {"id": ref("id"), "name": STR, "enabled": BOOL, "colour": LOOK,
                              "layer": {"type": "object", "additionalProperties": layer_entry}})
    item = obj(gd.MODE_KEYS, {
        "id": ref("id"), "name": STR, "daw": STR,
        "toggle": toggle, "hold": one_of(ID_REF, obj(["button"], {"button": ID_REF})),
        "one_shot": one_of(ID_REF, obj(["button"], {"button": ID_REF})), "cycle": toggle,
        "parent": ID_REF, "preset_blink": {},
        "led": one_of(ID_REF, {"type": "object", "description": "{ <LED id>: blink }"}),
        "keys": {}, "buttons": {"type": "object", "description": "button id: what it does in the mode"},
        "layer": {"type": "object", "description": "component id: what it does on the layer",
                  "additionalProperties": layer_entry},
        "steps": lst(step)})
    return lst(item, "modes, layers and cycles (components/modes/README.md \"Modes\")")


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
                          **{k: ID_REF for k in gd.PIN_HUB_KEYS}}))


def device_schema():
    top_types = {
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
        **{k: hub(k) for k in gd.HUB_LISTS if k not in gd.HUB_DOMAINS},
        **{d: hub_domain(d) for d in gd.HUB_DOMAINS},
        "presets": presets(),
        "modes": modes(),
        "display": screen(),
        "displays": lst(obj(gd.DISPLAY_KEYS, {"id": ref("id"), "daw": STR, "type": enum(gd.DISPLAY_TYPES),
                                              "shows": enum(gd.DISPLAY_SHOWS),
                                              "segment_step": T("integer", minimum=1, maximum=64),
                                              "segment_order": lst(enum(sorted({n for ns in gd.SEG_NAMES.values()
                                                                                for n in ns})),
                                                                   "the segments in pin order"),
                                              "chars": lst(one_of(ref("pin"), lst(ref("pin"))),
                                                           "each character's first pin, or its segments' pins"),
                                              "display": ref("id"),
                                              "line": T("integer", minimum=0, maximum=7),
                                              "x": T("integer", minimum=0, maximum=127),
                                              "scale": T("integer", minimum=1, maximum=4),
                                              "length": T("integer", minimum=1, maximum=gd.MAX_TEXT_CHARS)})),
        "layout": layout(),
        "write_to_flash": one_of(BOOL, obj(gd.WTF_KEYS, {**{k: BOOL for k in gd.WTF_ITEMS},
                                                         "delay": PERIOD, "idle": PERIOD})),
    }
    # the device's own keys, by area (gemifi_device.py CORE_AREAS), and the components' sections (KEY_SECTIONS)
    for area in gd.CORE_AREAS.values():
        for k, s in area.types().items():
            assert k not in top_types, f"{k}: in CORE_AREAS and written here too"
            top_types[k] = s
    for s in key_sections():
        assert s.yaml_key not in top_types, f"{s.yaml_key}: in KEY_SECTIONS and written here too"
        one, many = s.keys.schema(s.desc), lst(s.keys.schema(), s.desc)
        top_types[s.yaml_key] = one_of(one, many) if s.is_list == "either" else many if s.is_list else one
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
            "action_list": action_list(),
        },
    }


def schema_text():
    return json.dumps(device_schema(), indent=2, ensure_ascii=False) + "\n"


def key_sections():
    """Every component's KEY_SECTIONS (the top-level YAML keys whose items' keys are data)."""
    return [s for m in components.modules().values() for s in getattr(m, "KEY_SECTIONS", ())]


def component_keys():
    """The key tables of the components whose keys are data (components/keys.py), for their docs pages:
    component name -> [its sections: where the keys go in the YAML, and each key's type, default, doc]. A
    component's sections are its KEY_SECTIONS, and one per hub type of its HUB_TYPES and display platform of
    its DISPLAY_PLATFORMS with a spec."""
    out = {}
    for name, m in components.modules().items():
        secs = list(getattr(m, "KEY_SECTIONS", ()))
        secs += [Section(t.domain or t.key, t.spec, platform=t.key if t.domain else None)
                 for t in getattr(m, "HUB_TYPES", ()) if t.spec is not None]
        secs += [Section("display", t.spec, platform=t.platform)
                 for t in getattr(m, "DISPLAY_PLATFORMS", ()) if t.spec is not None]
        if secs:
            out[name] = [s.doc() for s in secs]
    # the device's own keys: "core/<area>" -> its top-level keys (core/yaml/<area>.md)
    for area, keys in gd.CORE_AREAS.items():
        out[f"core/{area}"] = [Section("", keys, is_list=False).doc()]
    return out


def keys_text():
    return json.dumps(component_keys(), indent=2, ensure_ascii=False) + "\n"
