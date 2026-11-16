"""A component's YAML keys as data (issue #161): each key's type, range, default and a line on what it
does, in one place that the validator, the JSON schema (tools/builder/gemifi_schema.py, for editors) and
the docs site (the key table on each component's page) all read.

    ENCODER_KEYS = Keys(
        Key("steps_per_detent", Int(1, 255), default=4, doc="Quarter steps the encoder counts per click."),
        Key("mode", Enum(ENC_MODES), default="absolute", doc="How a turn is sent.", see="Encoder modes",
            param="encoder.mode"),
        ...)
    spd = ENCODER_KEYS.get(v, d, "steps_per_detent", p)      # checked, and its default when left out

- `doc`: one line, shown in the editor's hover and the docs table. Longer explanations stay in the
  README, and `see` names the heading there ("Encoder modes"), or another doc as the READMEs name one,
  from the repo root (`core/README.md "MIDI messages"`, `hosts/README.md`).
- `param`: the device param (tools/builder/gemifi_device.py PARAMS, the settings schema of
  protocol/sysex.md §4.4) whose starting value the key sets: the web page can change it later without a
  rebuild, and a preset can when it is per preset. A preset's keys for a component are exactly those
  (gemifi_device.py PRESET_COMPONENTS); tests/test_keys.py checks the params exist.
- Rules between keys (`cc:` only with some modes, ...) stay as code in the component's validate().
- A key whose check needs more than its value (pins, references to other ids) is declared here for the
  schema and the docs, and checked by the component's own code.

A leaf module, like api.py: components import it at load time.
"""

import copy
import re

MISSING = object()


# -- JSON schema pieces (the device schema: tools/builder/gemifi_schema.py) --

def sub():
    return {"$ref": "#/definitions/substitution"}


def T(kind, **kw):
    """A scalar type that may also be a ${substitution}."""
    return {"anyOf": [dict({"type": kind}, **kw), sub()]}


def enum(values):
    return {"anyOf": [{"enum": list(values)}, sub()]}


def ref(name):
    return {"$ref": f"#/definitions/{name}"}


def one_of(*alts):
    return {"anyOf": list(alts)}


def lst(item, desc=None):
    s = {"type": "array", "items": item}
    if desc:
        s["description"] = desc
    return s


def code(x):
    return f"`{x}`"


# -- types --

class Type:
    """What a key's value is. text: the docs' type column (Markdown)."""
    text = ""

    def schema(self):
        return {}

    def check(self, v, d, key, path, default, required):
        """The value of d[key] (d the YAML mapping at path), checked with the validator v's own helpers so
        the messages are the usual ones; default when it is left out (or wrong: reported)."""
        raise NotImplementedError(f"{type(self).__name__}: checked by the component's code")

    def bad_values(self):
        """(value, accepted) pairs at the edges of the type, for the tests that the validator agrees."""
        return []

    def sub(self):
        """The keys of the mapping it takes, if any, and how the docs name them under the key: (Keys, prefix),
        prefix "" for `key.sub`, "[]" for a list's items (`key[].sub`), ".<id>" for a mapping by id (`key.<id>.sub`)."""
        return None


class Int(Type):
    """An integer lo..hi; hi None: no top here (the component's code knows it, e.g. the preset slots)."""

    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi
        self.text = f"integer {lo}..{hi}" if hi is not None else f"integer from {lo}"

    def schema(self):
        return T("integer", minimum=self.lo, **({} if self.hi is None else {"maximum": self.hi}))

    def check(self, v, d, key, path, default, required):
        return v.integer(d, key, path, self.lo, self.hi, default=default, required=required)

    def bad_values(self):
        if self.hi is None:
            return [(self.lo, True), (self.lo - 1, False)]
        return [(self.lo, True), (self.hi, True), (self.lo - 1, False), (self.hi + 1, False)]


class Hex(Int):
    """An integer usually written in hex (an I2C address): the docs give its range so."""

    def __init__(self, lo, hi):
        super().__init__(lo, hi)
        self.text = f"integer 0x{lo:02X}..0x{hi:02X}"


class Number(Type):
    """A number, not only a whole one (gamma: 2.2)."""

    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi
        self.text = f"number {lo:g}..{hi:g}"

    def schema(self):
        return T("number", minimum=self.lo, maximum=self.hi)

    def bad_values(self):
        return [(self.lo, True), (self.hi, True), (self.lo - 0.1, False), (self.hi + 0.1, False)]


class Enum(Type):
    """One of a list of names; checked to its index (as the C tables count them)."""

    def __init__(self, values):
        self.values = list(values)
        self.text = ", ".join(code(x) for x in self.values)

    def schema(self):
        return enum(self.values)

    def check(self, v, d, key, path, default, required):
        p = f"{path}.{key}" if path else key
        # the default as an index too (given as a name: Key(default="absolute"))
        dflt = self.values.index(default) if isinstance(default, str) else default
        if d.get(key) is None:
            if required:
                v.err(p, d.line, "is required")
            return dflt
        i = v.enum_value(d, key, p, self.values)
        return dflt if i is None else i

    def bad_values(self):
        return [(self.values[0], True), (f"not_{self.values[0]}", False)]


class Bool(Type):
    text = "`true` or `false`"

    def schema(self):
        return T("boolean")

    def check(self, v, d, key, path, default, required):
        return v.boolean(d, key, path, default=bool(default))


class Period(Type):
    """An ESPHome time period (`125us`, `1.6ms`, `2s`), checked to whole multiples of step_us."""

    def __init__(self, lo_us, hi_us, step_us=1, example="125us"):
        self.lo, self.hi, self.step, self.example = lo_us, hi_us, step_us, example
        self.text = f"time {fmt_us(lo_us)}..{fmt_us(hi_us)}"

    def schema(self):
        return ref("period")

    def check(self, v, d, key, path, default, required):
        return v.period(d, key, path, self.lo, self.hi, default=default, step_us=self.step, example=self.example)

    def bad_values(self):
        return [(f"{self.lo}us", True), (f"{self.hi}us", True), (f"{self.lo - self.step}us", False),
                (f"{self.hi + self.step}us", False)]


class Str(Type):
    """Text; pattern: a regular expression it must match (the schema says so; the component's code checks it)."""
    text = "text"

    def __init__(self, pattern=None, text=None):
        self.pattern = pattern
        if text:
            self.text = text

    def schema(self):
        return {"type": "string", **({"pattern": f"^{self.pattern}$"} if self.pattern else {})}

    def check(self, v, d, key, path, default, required):
        return v.string(d, key, path, required=required, default=default)


class Pin(Type):
    text = "pin"

    def schema(self):
        return ref("pin")


class Id(Type):
    text = "id"

    def schema(self):
        return ref("id")


class IdRef(Type):
    """The id of another component, of a kind ('LED', ...)."""

    def __init__(self, kind):
        self.kind = kind
        self.text = f"{kind} id"

    def schema(self):
        return {"type": "string", "description": f"the id of {'an' if self.kind[0] in 'AEFHILMNORSX' else 'a'} "
                                                 f"{self.kind}"}


class Ref(Type):
    """A type the schema has once, as a definition (#/definitions/<name>) that each key using it refers to;
    keys: its mapping's keys, for the docs (as Map's)."""

    def __init__(self, name, text, keys=None):
        self.name, self.text, self.keys = name, text, keys

    def schema(self):
        return ref(self.name)

    def sub(self):
        return (self.keys, "") if self.keys else None


class Raw(Type):
    """A shape the schema writes out itself (the layout's [x, y] lists ...): schema as given."""

    def __init__(self, schema, text, keys=None):
        self._schema, self.text, self.keys = schema, text, keys

    def schema(self):
        return self._schema

    def sub(self):
        return (self.keys, "") if self.keys else None


class ById(Type):
    """A mapping from a component's id (of a kind: 'button', 'LED', 'component') to a value of a type; names: a
    regular expression the ids match, for the schema (the validator's own messages name a wrong one)."""

    def __init__(self, kind, value, names=None):
        self.kind, self.value, self.names = kind, value, names
        self.text = f"{kind} id → {value.text}"

    def schema(self):
        s = {"type": "object", "description": f"{self.kind} id: ...", "additionalProperties": self.value.schema()}
        if self.names:
            s["propertyNames"] = {"pattern": f"^{self.names}$"}
        return s

    def sub(self):
        s = self.value.sub()
        return (s[0], ".<id>" + s[1]) if s else None


class Null(Type):
    """`null` (in a list: "nothing here")."""
    text = "`null`"

    def schema(self):
        return {"type": "null"}


class List(Type):
    def __init__(self, item):
        self.item = item
        self.text = f"list of {item.text}"

    def schema(self):
        return lst(self.item.schema())

    def sub(self):
        s = self.item.sub()
        return (s[0], "[]" + s[1]) if s else None


class Map(Type):
    """A mapping with keys of its own (Keys): the docs list them under the key, as `key.sub`."""
    text = "mapping"

    def __init__(self, keys, desc=None):
        self.keys, self.desc = keys, desc

    def schema(self):
        return self.keys.schema(self.desc)

    def sub(self):
        return self.keys, ""


class OneOf(Type):
    """Any of several types, e.g. a preset's name or a mapping."""

    def __init__(self, *types):
        self.types = types
        self.text = " or ".join(t.text for t in types)

    def schema(self):
        return one_of(*(t.schema() for t in self.types))

    def keys(self):
        """The sub-keys of its mapping, if it takes one."""
        s = self.sub()
        return s[0] if s else None

    def sub(self):
        return next((t.sub() for t in self.types if t.sub()), None)


# -- keys --

class Key:
    def __init__(self, name, type, default=None, required=False, doc="", see=None, param=None, shown_default=None):
        """param: the device param (gemifi_device.py PARAMS) whose starting value this key sets: the web page
        (and, for a per-preset param, a preset) can change it later. shown_default: what the docs say the
        default is, when it isn't just the value (Markdown)."""
        self.name, self.type, self.default, self.required = name, type, default, required
        self.doc, self.see, self.param, self.shown_default = doc, see, param, shown_default

    @property
    def live(self):
        return self.param is not None

    def but(self, **changes):
        """This key with some fields changed: a shared key in a component (CONTROL_NRPN.but(param=...))."""
        k = copy.copy(self)
        for f, x in changes.items():
            assert hasattr(k, f), f
            setattr(k, f, x)
        return k

    def elsewhere(self, readme):
        """This key in another page's table (a preset's overrides, a mode's layer entries): a `see` naming a
        heading of its own component's README (readme, from the repo root) names that README too."""
        if self.see and not re.match(r"[\w./-]+\.md", self.see):
            return self.but(see=f'{readme} "{self.see}"')
        return self

    def schema(self):
        """The key's JSON schema: its type's, with the doc line and default for the editor's hover."""
        s = self.type.schema()
        if "$ref" in s:                 # draft-07 ignores what sits next to a $ref
            s = one_of(s)
        else:
            s = dict(s)
        shown = self.default_text()
        desc = self.doc + (f" Default: {shown.replace('`', '')}." if shown is not None else "")
        if self.live:
            desc += f" Can be changed on the device ({self.param})."
        if desc.strip():
            s["description"] = desc.strip()
        if self.default is not None:
            s["default"] = self.default
        return s

    def default_text(self):
        """The default as the docs show it (Markdown), or None."""
        if self.shown_default is not None:
            return self.shown_default
        x = self.default
        return None if x is None else code("true" if x is True else "false" if x is False else x)

    def sub_keys(self):
        s = self.type.sub()
        return s[0] if s else None

    def doc_row(self):
        """The key's row in the docs table (gemifi_build.py keys: site/src/_data/component_keys.json)."""
        row = {"key": self.name, "type": self.type.text, "required": self.required,
               "default": self.default_text(), "doc": self.doc}
        if self.see:
            row["see"] = self.see
        if self.live:
            row["param"] = self.param
        s = self.type.sub()
        if s:
            row["keys"] = s[0].doc_rows()
            if s[1]:
                row["sub"] = s[1]           # how the sub-keys are named under it: "[]", ".<id>"
        return row


class Keys:
    """A mapping's keys, in the order the docs list them."""

    def __init__(self, *keys):
        self.keys = keys
        self.by_name = {k.name: k for k in keys}
        assert len(self.by_name) == len(keys), "a key twice"

    @property
    def names(self):
        return [k.name for k in self.keys]

    def __getitem__(self, name):
        return self.by_name[name]

    def __iter__(self):
        return iter(self.keys)

    def __add__(self, other):
        return Keys(*self.keys, *other.keys)

    def without(self, *names):
        return Keys(*(k for k in self.keys if k.name not in names))

    def get(self, v, d, name, path, default=MISSING):
        """d[name], checked by its type (the validator's helpers), or its default (or `default`) when left out."""
        k = self.by_name[name]
        return k.type.check(v, d, name, path, k.default if default is MISSING else default, k.required)

    def types(self):
        """{name: JSON schema}."""
        return {k.name: k.schema() for k in self.keys}

    def schema(self, desc=None):
        s = {"type": "object", "properties": self.types(), "additionalProperties": False}
        if desc:
            s["description"] = desc
        return s

    def doc_rows(self):
        return [k.doc_row() for k in self.keys]


class Section:
    """Where a component's keys go in the device YAML: a top-level key (a list of items, or one mapping),
    and for a platform of a base (led_driver: [{ platform: tm1637, ... }]) its platform name."""

    def __init__(self, yaml_key, keys, is_list=True, platform=None, desc=None):
        """is_list: True (a list of items), False (one mapping) or "either" (one mapping, or a list of them:
        i2c:). desc: what the section is, for the editor's hover on its key."""
        self.yaml_key, self.keys, self.is_list, self.platform, self.desc = yaml_key, keys, is_list, platform, desc

    def doc(self):
        s = {"section": self.yaml_key, "list": self.is_list, "keys": self.keys.doc_rows()}
        if self.platform:
            s["platform"] = self.platform
        return s


def fmt_us(us):
    """125 -> '125us', 1600 -> '1.6ms', 2000000 -> '2s'."""
    if us >= 1_000_000 and us % 100_000 == 0:
        return f"{us / 1_000_000:g}s"
    if us >= 1000 and us % 100 == 0:
        return f"{us / 1000:g}ms"
    return f"{us}us"


# -- keys several controls share (core/README.md "Controls"), described once --

CONTROL_ID = Key("id", Id(), doc="What the rest of the file calls it. May be left out when `name:` is given: it is "
                                 "then made from the name (`Oct down` -> `oct_down`).")
CONTROL_NAME = Key("name", Str(), doc="Its name on the web page and in the CLI. Left out, it is made from the id "
                                      "(`oct_down` -> `Oct down`).", shown_default="from the id")
CONTROL_DAW = Key("daw", Str(), doc="A role for the DAW scripts (`transport.play`, `x_<name>` for your own).",
                  see="hosts/README.md")
# The number an analog input or encoder sends in the NRPN / RPN modes (gemifi_device.py mode_number): the
# component's cc param (`.but(param="encoder.cc")`).
CONTROL_NRPN = Key("nrpn", Int(0, 16383), doc="The NRPN it sends with mode `nrpn` or `nrpn14`, instead of `cc:`.",
                   see='core/README.md "MIDI messages"')
CONTROL_RPN = Key("rpn", Int(0, 16383), doc="The RPN it sends with mode `rpn` or `rpn14`, instead of `cc:`.",
                  see='core/README.md "MIDI messages"')
# channel:, min:, max: (gemifi_device.py control_out): build time only, the same in every preset.
CONTROL_OUT = Keys(
    Key("channel", Int(1, 16), doc="The MIDI channel it sends on, instead of `midi.channel`."),
    Key("min", Int(0, 16383), doc="The value it sends at the bottom of its range, in the mode's units "
                                  "(0..127, or 0..16383 in a 14-bit mode); greater than `max` turns it around."),
    Key("max", Int(0, 16383), doc="The value it sends at the top of its range (default: the mode's top)."),
)
