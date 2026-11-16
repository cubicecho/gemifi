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
        **{k: hub(k) for k in gd.HUB_LISTS if k not in gd.HUB_DOMAINS},
        **{d: hub_domain(d) for d in gd.HUB_DOMAINS},
        "display": screen(),
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
