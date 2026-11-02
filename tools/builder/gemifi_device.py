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

# core/README.md "Button actions". `mcu` (13) only exists on devices with an `mcu:` section.
ACTIONS = ["none", "cc_momentary", "cc_toggle", "cc_ramp", "note", "octave_down", "octave_up",
           "transpose_down", "transpose_up", "bend_down", "bend_up", "program_next", "program_prev", "mcu"]
CC_ACTIONS = {"cc_momentary", "cc_toggle", "cc_ramp"}

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

# Hubs (I/O expanders), declared ESPHome style: one top-level list per chip, named like the
# ESPHome component where there is one. A pin on a hub names the hub by its chip key:
# { sn74hc595: leds, number: 3 }. ESPHome's pin key for the MCP23017 is mcp23xxx.
HUB_ENUM = {"sn74hc595": "GM_HUB_HC595", "sn74hc165": "GM_HUB_HC165", "cd74hc4051": "GM_HUB_MUX",
            "cd74hc4067": "GM_HUB_MUX", "mcp23017": "GM_HUB_MCP23017", "pcf8574": "GM_HUB_PCF8574"}
HUB_TYPES = list(HUB_ENUM)
HUB_PIN_KEY = {t: ("mcp23xxx" if t == "mcp23017" else t) for t in HUB_TYPES}
PIN_KEY_HUB = {k: t for t, k in HUB_PIN_KEY.items()}
I2C_HUBS = {"mcp23017", "pcf8574"}
SHIFT_HUBS = {"sn74hc595", "sn74hc165"}
MUX_HUBS = {"cd74hc4051", "cd74hc4067"}
MUX_SELECT = {"cd74hc4051": 3, "cd74hc4067": 4}
OUT_HUBS = {"sn74hc595", "mcp23017", "pcf8574"}
IN_HUBS = {"sn74hc165", "cd74hc4051", "cd74hc4067", "mcp23017", "pcf8574"}
HUB_KEYS = {
    "sn74hc595": ["id", "data_pin", "clock_pin", "latch_pin", "oe_pin", "sr_count"],
    "sn74hc165": ["id", "data_pin", "clock_pin", "load_pin", "sr_count"],
    "cd74hc4051": ["id", "pin_s0", "pin_s1", "pin_s2", "common_pin", "enable_pin", "delay"],
    "cd74hc4067": ["id", "pin_s0", "pin_s1", "pin_s2", "pin_s3", "common_pin", "enable_pin", "delay"],
    "mcp23017": ["id", "address"],
    "pcf8574": ["id", "address"],
}
# hub control pins that may be shared between hubs (allow_other_uses): same line kind on each
CTRL_LINE = {"clock_pin": "clock", "latch_pin": "latch", "load_pin": "latch", "data_pin": "data",
             "oe_pin": "enable", "enable_pin": "enable", "common_pin": "common",
             "pin_s0": "s0", "pin_s1": "s1", "pin_s2": "s2", "pin_s3": "s3"}

TOP_KEYS = ["gemifi", "name", "friendly_name", "description", "project", "board_id", "platform", "chip",
            "usb", "midi", "i2c", "scan_interval", "boot_combo", "feedback", "mcu", *HUB_TYPES,
            "buttons", "leds", "analog", "encoders", "key_matrix"]
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

INVERT, PULLUP, PULLDOWN = 1, 2, 4
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
                             (PULLDOWN, "GM_PIN_PULLDOWN")) if self.flags & b]
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
        self.mcu = None              # Hub(extender=bool, cable=0|1) when mcu: is given
        self.daw_port = False


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

    def mode(self, v, path, role):
        """ESPHome pin mode (string or mapping) -> set of flags, checked against the role.
        Returns None after an error."""
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
        if "open_drain" in flags:
            bad = "open_drain isn't supported by gemifi's pin drivers"
        elif "pullup" in flags and "pulldown" in flags:
            bad = "pullup and pulldown are both set"
        elif role == "out" and flags - {"output"}:
            bad = f"this pin is {ROLE_TEXT[role]}: use mode: OUTPUT (or leave mode out)"
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

    def pin(self, v, path, line, role, allow_hub=True, line_kind=None, owner=None, hubs=None, why=None):
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
            mode = self.mode(v, path, role)
            if mode is None:
                return None
            flags |= (PULLUP if "pullup" in mode else 0) | (PULLDOWN if "pulldown" in mode else 0)
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
            extra = f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8)" if hub.type in SHIFT_HUBS else ""
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
            "hub": "faders"}.get(kind, "item")


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
    if dev.chip and not plat.is_esp and not re.fullmatch(r"NUC1\d\d\w+", dev.chip, re.I):
        v.warn("chip", data.line_of("chip"), f"'{dev.chip}' doesn't look like a NUC1xx part number")

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

    validate_components(v, dev, data)

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

    if len(dev.buttons) > 32:
        v.err("buttons", data.line_of("buttons"), f"{len(dev.buttons)} buttons: at most 32 (the runtime "
                                                  "keeps button state in a 32-bit mask)")
    for kind, lst in (("buttons", dev.buttons), ("leds", dev.leds), ("analog", dev.analogs),
                      ("encoders", dev.encoders)):
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
    if not (dev.buttons or dev.analogs or dev.encoders or dev.matrix):
        v.warn("", data.line, "the device has no inputs (buttons, analog, encoders or key_matrix)")
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
    """feedback: { enabled, led_owner, led_timeout } (all optional): F1 host feedback on the main port."""
    if data.get("feedback") is None:
        return
    fb = v.mapping(data["feedback"], "feedback", data.line_of("feedback"), ["enabled", "led_owner", "led_timeout"])
    if fb is None:
        return
    dev.feedback = v.boolean(fb, "enabled", "feedback", default=True)
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
        d = v.mapping(d, p, leds.line_of(i), ["id", "name", "pin", "pin2", "feedback", "owner", "timeout"],
                      renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "LED", len(dev.leds))
        name = v.display_name(d, p, ident)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "out")
        pin2 = v.pin(d["pin2"], f"{p}.pin2", d.line_of("pin2"), "out") if d.get("pin2") is not None else NO_PIN
        src = validate_led_source(v, dev, d, p)
        owner = validate_owner(v, d, p)
        timeout = validate_timeout(v, d, "timeout", p, None)
        if timeout is not None and owner in (LED_OWNERS["host"], LED_OWNERS["local"]):
            v.warn(f"{p}.timeout", d.line_of("timeout"), "only used with owner: host_then_local")
        dev.leds.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, pin2=pin2 or NO_PIN, source=src,
                            owner=owner, timeout=timeout, path=p, line=d.line, used_by=[]))

    buttons = v.seq(data.get("buttons"), "buttons", data.line_of("buttons"))
    for i, d in enumerate(buttons):
        p = f"buttons[{i}]"
        d = v.mapping(d, p, buttons.line_of(i), ["id", "name", "pin", "action", "cc", "note", "led", "mcu"],
                      renamed=RENAMED_BUTTON)
        if d is None:
            continue
        ident = v.ident(d, p, "button", len(dev.buttons))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "button", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "in")
        act = d.get("action", "mcu" if "mcu" in d else "none")
        if act not in ACTIONS:
            close = difflib.get_close_matches(str(act), ACTIONS, 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.action", d.line_of("action"), f"unknown action '{act}' ({hint}valid: "
                                                       f"{', '.join(ACTIONS)})")
            act = "none"
        if act == "mcu" or "mcu" in d:
            num = button_mcu(v, dev, d, p, act)
            if act != "mcu":
                act = "none"
        else:
            num = button_number(v, d, p, act)
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
        dev.buttons.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, action=ACTIONS.index(act),
                               action_name=act, number=num, led=led))

    analogs = v.seq(data.get("analog"), "analog", data.line_of("analog"))
    for i, d in enumerate(analogs):
        p = f"analog[{i}]"
        d = v.mapping(d, p, analogs.line_of(i), ["id", "name", "pin", "cc", "adc_min", "adc_max", "takeover",
                                                 "mcu"], renamed=RENAMED_ANALOG)
        if d is None:
            continue
        ident = v.ident(d, p, "analog input", len(dev.analogs))
        name = v.display_name(d, p, ident)
        unique_name(v, names, "analog input", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog")
        fader = mcu_name(v, dev, d, p, MCU_FADERS, "fader_1")
        cc = v.integer(d, "cc", p, 0, 127, default=0, required=fader is None)
        lo = v.integer(d, "adc_min", p, 0, 4095, default=0)
        hi = v.integer(d, "adc_max", p, 0, 4095, default=4095)
        if lo == hi:
            v.err(p, d.line, f"adc_min and adc_max are both {lo}")
        pickup = validate_takeover(v, dev, d, p, fader)
        dev.analogs.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, cc=cc, raw_min=lo, raw_max=hi,
                               pickup=pickup, mcu=fader))

    encs = v.seq(data.get("encoders"), "encoders", data.line_of("encoders"))
    for i, d in enumerate(encs):
        p = f"encoders[{i}]"
        d = v.mapping(d, p, encs.line_of(i), ["id", "name", "pin_a", "pin_b", "cc", "steps_per_detent", "mcu",
                                              "ring"], renamed=RENAMED_ENCODER)
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
        dev.encoders.append(Hub(id=ident or "", name=name, a=pins["pin_a"] or NO_PIN, b=pins["pin_b"] or NO_PIN,
                                cc=cc, steps=spd, mcu=vpot, ring=ring))
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
    for k in ("cc", "note"):
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
    if vpot is None:
        if dev.mcu is not None or "mcu" not in d:
            v.err(f"{p}.ring", line, "ring: shows an MCU V-Pot ring: give the encoder `mcu: vpot_N`")
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


def button_number(v, d, p, act):
    """The default button.number: cc: for the CC actions, note: for action note."""
    want = "note" if act == "note" else "cc" if act in CC_ACTIONS else None
    if "cc" in d and "note" in d:
        v.err(p, d.line, "give cc: or note:, not both")
    if want is None:
        given = next((k for k in ("cc", "note") if k in d), None)
        if given:
            v.warn(f"{p}.{given}", d.line_of(given), f"action {act} sends no CC or note; {given}: only "
                                                     "sets the default button.number for when the action is "
                                                     "changed in the config")
            return v.integer(d, given, p, 0, 127, default=0)
        return 0
    other = "cc" if want == "note" else "note"
    if other in d:
        v.err(f"{p}.{other}", d.line_of(other), f"action {act} takes {want}:, not {other}:")
        return v.integer(d, other, p, 0, 127, default=0)
    if "number" in d:          # format 1; reported as renamed
        return 0
    if want not in d:
        v.err(p, d.line, f"action {act} needs {want}: (0..127)")
        return 0
    return v.integer(d, want, p, 0, 127, default=0)


def validate_hub(v, dev, h, typ, p, line):
    index = len(dev.hubs)
    if not isinstance(h, dict):
        v.err(p, line, f"must be a mapping (got {type_name(h)})")
        return
    v.mapping(h, p, line, HUB_KEYS[typ], renamed=RENAMED_HUB)
    hid = v.ident(h, p, "hub", index, allow_name=False)
    hub = Hub(id=hid or f"#{index}", type=typ, index=index, path=p, line=line, used=False, chain=0,
              n_sel=0, addr=0, data=NO_PIN, clock=NO_PIN, latch=NO_PIN, sel=[], common=None,
              common_adc=False, enable=NO_PIN, settle_us=0, n_pins=0)
    if hid is not None:
        v.hub_by_id[hid] = hub
    dev.hubs.append(hub)

    def ctl(key, role, required=True):
        if h.get(key) is None:
            if required:
                v.err(f"{p}.{key}", h.line, "is required")
            return NO_PIN
        return v.pin(h[key], f"{p}.{key}", h.line_of(key), role, allow_hub=False, line_kind=CTRL_LINE[key],
                     owner=index) or NO_PIN

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
    else:
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x20)
        hub.n_pins = 16 if typ == "mcp23017" else 8
        ok = range(0x20, 0x28) if typ == "mcp23017" else list(range(0x20, 0x28)) + list(range(0x38, 0x40))
        if hub.addr not in ok:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for a {typ} "
                                                         f"(usually 0x20-0x27{'' if typ == 'mcp23017' else ', PCF8574A 0x38-0x3F'})")


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
    d = v.mapping(d, p, line, ["type", "rows", "columns", "keys", "first_note", "settle", "velocity"],
                  renamed=RENAMED_MATRIX)
    if d is None:
        return
    kind = d.get("type")
    if kind not in ("switch", "velocity"):
        if not (kind is None and "kind" in d):
            v.err(f"{p}.type", d.line_of("type"), f"{'is required' if kind is None else repr(kind) + ' is unknown'} "
                                                   "(switch: one contact per key, velocity: two contacts per key)")
        kind = "switch"
    pins = {}
    for key, role, mh in (("rows", "out", {"sn74hc595"}), ("columns", "in", {"sn74hc165"})):
        lst = d.get(key)
        if not isinstance(lst, list) or not lst:
            if not (key == "columns" and "cols" in d):
                v.err(f"{p}.{key}", d.line_of(key), "needs a list of pins")
            lst = LList()
        if len(lst) > 255:
            v.err(f"{p}.{key}", d.line_of(key), f"{len(lst)} {key}: at most 255")
        why = "the matrix is scanned from the timer interrupt, so only MCU pins, sn74hc595 rows and sn74hc165 columns"
        pins[key] = [v.pin(x, f"{p}.{key}[{i}]", lst.line_of(i), role, hubs=mh, why=why) or NO_PIN
                     for i, x in enumerate(lst)]
    nr, nc = len(pins["rows"]), len(pins["columns"])
    cap = nr * nc if kind == "switch" else nr * (nc // 2)
    if kind == "velocity" and nc % 2:
        v.err(f"{p}.columns", d.line_of("columns"), f"type velocity needs an even number of columns "
                                                    f"(two contacts per key), got {nc}")
    keys = v.integer(d, "keys", p, 1, 128, default=min(cap, 128) or 1)
    first = v.integer(d, "first_note", p, 0, 127, default=48)
    if keys is not None and nr and nc and keys > cap:
        v.err(f"{p}.keys", d.line_of("keys"), f"{keys} keys don't fit a {nr} x {nc} {kind} matrix "
                                              f"(at most {cap})")
    if keys is not None and first is not None and first + keys > 128:
        v.err(f"{p}.keys", d.line_of("keys"), f"first_note {first} + {keys} keys goes past note 127")
    settle = v.period(d, "settle", p, 0, 255, default=1, example="1us")
    vel_fast, vel_slow = validate_velocity(v, d, p, kind)
    dev.matrix = Hub(kind=kind, rows=pins["rows"], cols=pins["columns"], keys=keys or 0,
                     first_note=first or 0, settle_us=settle, vel_fast=vel_fast, vel_slow=vel_slow)


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
    if m and getattr(m, "vel_fast", None) is not None:
        h.append(f"#define GM_VEL_FAST_DEFAULT {m.vel_fast} /* {m.vel_fast / 10:g} ms */")
    if m and getattr(m, "vel_slow", None) is not None:
        h.append(f"#define GM_VEL_SLOW_DEFAULT {m.vel_slow} /* {m.vel_slow / 10:g} ms */")
    if dev.i2c:
        h += [f"#define GM_I2C_SDA {dev.i2c[0].num} /* {dev.i2c[0].desc} */",
              f"#define GM_I2C_SCL {dev.i2c[1].num} /* {dev.i2c[1].desc} */"]
    h += feedback_defines(dev)
    if dev.platform.is_esp:
        h += ["", "#ifndef GM_MIDI_USB", "#define GM_MIDI_USB 1", "#define GM_MIDI_SERIAL 2",
              "#define GM_MIDI_BLE 3", "#endif",
              f"#define GM_MIDI_TRANSPORT {MIDI_TRANSPORT_MACRO[dev.transport]}"]
    h += ["", "#endif", ""]

    c = [head, '#include "gm_device.h"', '#include "gm_device_config.h"', ""]

    def pin_c(pn):
        return pn.c()

    if dev.hubs:
        c.append("static const gm_hub_t hubs[GM_N_HUBS] = {")
        for hb in dev.hubs:
            sel = [s.c() for s in hb.sel] + ["GM_NO_PIN"] * (4 - len(hb.sel))
            c += [f"    {{ /* {hb.index + 1}: {_comment(hb.id)} ({hb.type}) */",
                  f"        .type = {HUB_ENUM[hb.type]}, .chain = {hb.chain}, .n_sel = {hb.n_sel}, "
                  f".i2c_addr = 0x{hb.addr:02X},",
                  f"        .data = {hb.data.c()}, .clock = {hb.clock.c()}, .latch = {hb.latch.c()},",
                  f"        .sel = {{ {', '.join(sel)} }},",
                  f"        .common = {(hb.common or NO_PIN).c()}, .enable = {hb.enable.c()},",
                  f"        .settle_us = {hb.settle_us},",
                  "    },"]
        c += ["};", ""]
    if dev.buttons:
        c.append("static const gm_button_t buttons[GM_N_BUTTONS] = {")
        c += [f"    {{ {pin_c(b.pin)}, {c_str(b.name)}, {b.led} }}, /* {_comment(b.id)}: {_comment(b.pin.desc)} */"
              for b in dev.buttons]
        c += ["};", ""]
    if dev.leds:
        c.append("static const gm_led_t leds[GM_N_LEDS] = {")
        c += [f"    {{ {pin_c(x.pin)}, {pin_c(x.pin2)} }}, /* {_comment(x.id)}: {_comment(x.pin.desc)} */"
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
        c += ["static const gm_matrix_t matrix = {",
              f"    .rows = matrix_rows, .n_rows = {len(m.rows)},",
              f"    .cols = matrix_cols, .n_cols = {len(m.cols)},",
              f"    .kind = {kind}, .n_keys = {m.keys}, .first_note = {m.first_note}, "
              f".settle_us = {m.settle_us},",
              "};", ""]
    combo = " | ".join(f"(1u << {i})" for i in sorted(dev.boot_combo)) or "0"
    c += ["const gm_device_t gm_device = {",
          "    .name = GM_DEVICE_NAME,",
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
          arr("gm_encoder_cc_defaults", "GM_N_ENCODERS", [e.cc for e in dev.encoders]), ""]
    c += feedback_tables(dev)
    return {"gm_device_config.h": "\n".join(h), "gm_device_config.c": "\n".join(c)}


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
        rings = []
        for i, e in enumerate(dev.encoders):
            if e.ring:
                c.append(f"static const uint8_t ring_{i}[{len(e.ring)}] = {{ {', '.join(map(str, e.ring))} }}; "
                         f"/* {_comment(e.id)} */")
                rings.append(f"{{ ring_{i}, {len(e.ring)} }}")
            else:
                rings.append("{ 0, 0 }")
        c.append(f"const gm_ring_t gm_encoder_ring[{'GM_N_ENCODERS' if dev.encoders else '1'}] = "
                 f"{{ {', '.join(rings) or '{ 0, 0 }'} }};")
        c.append("")
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
