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

FORMAT_VERSION = 1

# core/README.md "Button actions"
ACTIONS = ["none", "cc_momentary", "cc_toggle", "cc_ramp", "note", "octave_down", "octave_up",
           "transpose_down", "transpose_up", "bend_down", "bend_up", "program_next", "program_prev"]
NUMBER_ACTIONS = {"cc_momentary", "cc_toggle", "cc_ramp", "note"}

# gm_device.h
HUB_ENUM = {"74hc595": "GM_HUB_HC595", "74hc165": "GM_HUB_HC165", "74hc4051": "GM_HUB_MUX",
            "74hc4067": "GM_HUB_MUX", "mux": "GM_HUB_MUX", "mcp23017": "GM_HUB_MCP23017",
            "pcf8574": "GM_HUB_PCF8574"}
I2C_HUBS = {"mcp23017", "pcf8574"}
SHIFT_HUBS = {"74hc595", "74hc165"}
MUX_HUBS = {"74hc4051", "74hc4067", "mux"}
OUT_HUBS = {"74hc595", "mcp23017", "pcf8574"}
IN_HUBS = {"74hc165", "74hc4051", "74hc4067", "mux", "mcp23017", "pcf8574"}
HUB_KEYS = {
    "74hc595": {"data", "clock", "latch", "enable", "chain"},
    "74hc165": {"data", "clock", "latch", "chain"},
    "74hc4051": {"select", "common", "enable", "settle_us"},
    "74hc4067": {"select", "common", "enable", "settle_us"},
    "mux": {"select", "common", "enable", "settle_us"},
    "mcp23017": {"address"},
    "pcf8574": {"address"},
}
HUB_COMMON_KEYS = {"id", "type", "shared"}

TOP_KEYS = ["gemifi", "name", "description", "board_id", "platform", "chip", "usb", "midi", "i2c",
            "scan_us", "boot_combo", "hubs", "buttons", "leds", "analog", "encoders", "key_matrix"]
PIN_KEYS = ["pin", "hub", "channel", "inverted", "pullup", "pulldown"]

INVERT, PULLUP, PULLDOWN = 1, 2, 4
MIDI_TRANSPORT_MACRO = {"usb": "GM_MIDI_USB", "serial": "GM_MIDI_SERIAL", "ble": "GM_MIDI_BLE"}
DEFAULT_USB = (0x1209, 0x0002)   # pid.codes test PID (protocol/sysex.md section 7)


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

    def line_of(self, key):
        return self.key_lines.get(key, self.line)


class LList(list):
    line = 0

    def __init__(self, *a):
        super().__init__(*a)
        self.item_lines = []

    def line_of(self, i):
        return self.item_lines[i] if i < len(self.item_lines) else self.line


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
            active.discard(id(n))
            return d
        if isinstance(n, yaml.SequenceNode):
            active.add(id(n))
            lst = LList()
            lst.line = n.start_mark.line + 1
            for item in n.value:
                lst.append(conv(item))
                lst.item_lines.append(item.start_mark.line + 1)
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
            out.append(("warning", "NUC1xx GPIOs have no internal pull-down: fit an external one"))
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
                out.append(("warning", f"{nm} has no internal pull-up/down on esp32: fit an external one"))
            if self.name == "esp8266":
                if num == 16 and flags & PULLUP:
                    out.append(("warning", "GPIO16 on esp8266 has a pull-down only, no pull-up"))
                if num != 16 and flags & PULLDOWN:
                    out.append(("warning", f"{nm} on esp8266 has no internal pull-down"))
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
        self.boot_combo = []
        self.i2c = None


class Use:
    def __init__(self, path, line, role, ctrl=None, shared=False, hub=None):
        self.path, self.line, self.role, self.ctrl, self.shared, self.hub = path, line, role, ctrl, shared, hub


# ---------------------------------------------------------------------------------------------
# validation

class _V:
    def __init__(self, file):
        self.file, self.diags = file, []
        self.uses = {}
        self.plat = None
        self.transport = None
        self.hub_by_id = {}

    def err(self, path, line, msg):
        self.diags.append(Diag("error", path, line, msg, self.file))

    def warn(self, path, line, msg):
        self.diags.append(Diag("warning", path, line, msg, self.file))

    # -- generic field helpers --

    def mapping(self, v, path, line, keys):
        if not isinstance(v, dict):
            self.err(path, line, f"must be a mapping (got {type_name(v)})")
            return None
        for k in v:
            if k not in keys:
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
            self.err(p, d.line_of(key), f"must be an integer {lo}..{hi} (got {type_name(v)} {v!r})")
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
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            v = str(v)
        if not isinstance(v, str) or not v.strip():
            self.err(p, d.line_of(key), f"must be a non-empty string (got {v!r})")
            return default
        if any(ord(c) > 126 or ord(c) < 32 for c in v):
            self.warn(p, d.line_of(key), "contains non-ASCII or control characters; they may not "
                                         "show correctly in the web UI")
        return v

    def label(self, d, path, seen):
        lab = self.string(d, "label", path, required=True)
        if lab is not None:
            if lab in seen:
                self.err(f"{path}.label", d.line_of("label"), f"'{lab}' is already used by {seen[lab]}")
            else:
                seen[lab] = path
        return lab

    # -- pins --

    def mcu_pin(self, v, path, line, role):
        try:
            num = self.plat.parse_pin(v)
        except ValueError as e:
            self.err(path, line, str(e))
            return None
        return num

    def pin(self, v, path, line, role, allow_hub=True, ctrl=None, shared=False, owner=None, hubs=None,
            why=None):
        """Resolve a pin spec (string / int / mapping). role: 'in' | 'out' | 'analog'.
        Registers the use for the duplicate check. Returns Pin or None."""
        if v is None:
            self.err(path, line, "is required")
            return None
        flags, hub_id, raw = 0, None, v
        if isinstance(v, dict):
            v = self.mapping(v, path, line, PIN_KEYS)
            if v is None:
                return None
            for key, bit in (("inverted", INVERT), ("pullup", PULLUP), ("pulldown", PULLDOWN)):
                if self.boolean(v, key, path):
                    flags |= bit
            if flags & PULLUP and flags & PULLDOWN:
                self.err(path, line, "pullup and pulldown are both set")
            hub_id = v.get("hub")
            if hub_id is not None:
                if "pin" in v and "channel" in v:
                    self.err(path, line, "give either pin or channel, not both")
                    return None
                raw = v.get("pin", v.get("channel"))
                if raw is None:
                    self.err(path, line, f"needs pin: (or channel:) on hub '{hub_id}'")
                    return None
            else:
                if "channel" in v:
                    self.err(f"{path}.channel", v.line_of("channel"), "channel: needs hub:")
                    return None
                raw = v.get("pin")
                if raw is None:
                    self.err(path, line, "needs pin: (or hub: + pin:)")
                    return None
                line = v.line_of("pin")
        if hub_id is None:
            if isinstance(raw, (dict, list)):
                self.err(path, line, f"must be a pin name (got {type_name(raw)})")
                return None
            num = self.mcu_pin(raw, path, line, role)
            if num is None:
                return None
            name = self.plat.pin_name(num)
            if role == "analog":
                ok, w = self.plat.adc(num)
                if not ok:
                    self.err(path, line, f"{name} has no ADC on {self.plat.name} "
                                         f"({adc_text(self.plat)}; or use a mux hub channel)")
                elif w:
                    self.warn(path, line, w)
            for lvl, msg in self.plat.check(num, role, flags, self.transport):
                (self.err if lvl == "error" else self.warn)(path, line, msg)
            self.use(("mcu", num), name, Use(path, line, role, ctrl, shared, owner))
            return Pin(0, flags, num, name)
        if not allow_hub:
            self.err(path, line, "must be an MCU pin (hub control pins can't be on another hub)")
            return None
        hub = self.hub_by_id.get(hub_id)
        if hub is None:
            known = ", ".join(self.hub_by_id) or "none defined"
            close = difflib.get_close_matches(str(hub_id), list(self.hub_by_id), 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            self.err(path, line, f"unknown hub '{hub_id}' (hubs: {known}{hint})")
            return None
        hub.used = True
        num = self.hub_pin_num(hub, raw, path, line)
        if num is None:
            return None
        allowed = hubs or {"out": OUT_HUBS, "in": IN_HUBS, "analog": MUX_HUBS}[role]
        what = {"out": "an output", "in": "an input", "analog": "an analog input"}[role]
        if hub.type not in allowed:
            self.err(path, line, f"hub '{hub.id}' is a {hub.type}, which can't be {what} "
                                 f"(use {' / '.join(sorted(allowed))}{'; ' + why if why else ''})")
            return None
        if role == "analog" and hub.common is not None and not hub.common_adc:
            self.err(path, line, f"hub '{hub.id}' common pin {hub.common.desc} has no ADC, so its "
                                 f"channels can't be analog ({adc_text(self.plat)})")
        if flags & PULLDOWN:
            self.warn(path, line, "pulldown has no effect on a hub pin")
        if flags & PULLUP and hub.type != "mcp23017":
            self.warn(path, line, f"pullup has no effect on a {hub.type} pin")
        desc = f"{hub.id}.{num}"
        self.use(("hub", hub.index, num), desc, Use(path, line, role))
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
        for key, lst in self.uses.items():
            if len(lst) < 2:
                continue
            name, first = lst[0]
            ok_share = all(u.ctrl and u.shared for _, u in lst) and \
                len({u.ctrl for _, u in lst}) == 1 and len({u.hub for _, u in lst}) == len(lst)
            if ok_share:
                continue
            for _, u in lst[1:]:
                hint = ""
                if u.ctrl and first.ctrl and u.ctrl == first.ctrl:
                    hint = " (to share a hub control line between hubs, set shared: true on each of them)"
                self.err(u.path, u.line, f"{name} is already used by {first.path} (line {first.line}){hint}")


def type_name(v):
    return {dict: "mapping", LDict: "mapping", list: "list", LList: "list", str: "string", int: "integer",
            bool: "boolean", float: "number", type(None): "nothing"}.get(type(v), type(v).__name__)


def adc_text(plat):
    if plat.name == "nuvoton-nuc1xx":
        return "ADC pins: PA0-PA7"
    if plat.name == "esp8266":
        return "the only ADC pin is A0"
    return f"ADC pins: {_ranges(plat.adc1 | plat.adc2, lambda n: f'GPIO{n}')}"


def validate(data, file=None):
    """data: the loaded YAML (LDict). Returns (Device or None, [Diag])."""
    v = _V(file)
    dev = Device()
    if not isinstance(data, dict):
        v.err("", getattr(data, "line", 1), "the top level must be a mapping (gemifi: 1, name: ..., ...)")
        return None, v.diags
    v.mapping(data, "", data.line, TOP_KEYS)

    fmt = v.integer(data, "gemifi", "", 0, 1000, required=True)
    if fmt is not None and fmt != FORMAT_VERSION:
        v.err("gemifi", data.line_of("gemifi"), f"format version {fmt} is not supported (this builder "
                                                 f"reads version {FORMAT_VERSION})")
    dev.name = v.string(data, "name", "", required=True) or "?"
    dev.description = v.string(data, "description", "")
    dev.board_id = v.integer(data, "board_id", "", 0, 0x3FFF, required=True) or 0
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
    if "usb" in data:
        usb = v.mapping(data["usb"], "usb", data.line_of("usb"), ["vid", "pid"])
        if usb is not None:
            dev.usb = (v.integer(usb, "vid", "usb", 0, 0xFFFF, required=True) or 0,
                       v.integer(usb, "pid", "usb", 0, 0xFFFF, required=True) or 0)
            if tr != "usb":
                v.warn("usb", data.line_of("usb"), f"ignored: midi.transport is {tr}")
    elif tr == "usb":
        dev.usb = DEFAULT_USB
        v.warn("usb", data.line, "not given: using the pid.codes test IDs 1209:0002 (development only)")

    dev.scan_us = v.integer(data, "scan_us", "", 1, 0xFFFF, default=125)

    # hubs (before anything that refers to them)
    hubs = v.seq(data.get("hubs"), "hubs", data.line_of("hubs"))
    for i, h in enumerate(hubs):
        validate_hub(v, dev, h, f"hubs[{i}]", hubs.line_of(i), i)
    if len(dev.hubs) > 254:
        v.err("hubs", hubs.line, f"{len(dev.hubs)} hubs: at most 254")

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
                                "add i2c: {{ sda: ..., scl: ... }}")
    addrs = {}
    for h in dev.hubs:
        if h.type in I2C_HUBS:
            if h.addr in addrs:
                v.err(f"{h.path}.address", h.line, f"I2C address 0x{h.addr:02X} is already used by hub "
                                                   f"'{addrs[h.addr]}'")
            addrs[h.addr] = h.id

    # LEDs first (buttons refer to them)
    leds = v.seq(data.get("leds"), "leds", data.line_of("leds"))
    seen, led_index = {}, {}
    for i, d in enumerate(leds):
        p = f"leds[{i}]"
        d = v.mapping(d, p, leds.line_of(i), ["label", "pin", "pin2"])
        if d is None:
            continue
        lab = v.label(d, p, seen)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "out")
        pin2 = v.pin(d["pin2"], f"{p}.pin2", d.line_of("pin2"), "out") if d.get("pin2") is not None else NO_PIN
        if lab is not None:
            led_index.setdefault(lab, len(dev.leds))
        dev.leds.append(Hub(label=lab or "", pin=pin or NO_PIN, pin2=pin2 or NO_PIN))

    buttons = v.seq(data.get("buttons"), "buttons", data.line_of("buttons"))
    seen, btn_index = {}, {}
    for i, d in enumerate(buttons):
        p = f"buttons[{i}]"
        d = v.mapping(d, p, buttons.line_of(i), ["label", "pin", "action", "number", "led"])
        if d is None:
            continue
        lab = v.label(d, p, seen)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "in")
        act = d.get("action", "none")
        if act not in ACTIONS:
            close = difflib.get_close_matches(str(act), ACTIONS, 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.action", d.line_of("action"), f"unknown action '{act}' ({hint}valid: "
                                                       f"{', '.join(ACTIONS)})")
            act = "none"
        num = v.integer(d, "number", p, 0, 127, default=0)
        if act in NUMBER_ACTIONS and "number" not in d:
            v.warn(p, d.line, f"action {act} without number: using 0")
        led = -1
        if d.get("led") is not None:
            ll = str(d["led"])
            if ll not in led_index:
                close = difflib.get_close_matches(ll, list(led_index), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(f"{p}.led", d.line_of("led"), f"no LED labelled '{ll}' "
                                                    f"(LEDs: {', '.join(led_index) or 'none'}{hint})")
            else:
                led = led_index[ll]
                if led > 127:
                    v.err(f"{p}.led", d.line_of("led"), "LED index over 127 can't be referenced")
                    led = -1
        if lab is not None:
            btn_index.setdefault(lab, len(dev.buttons))
        dev.buttons.append(Hub(label=lab or "", pin=pin or NO_PIN, action=ACTIONS.index(act),
                               action_name=act, number=num, led=led))

    analogs = v.seq(data.get("analog"), "analog", data.line_of("analog"))
    seen = {}
    for i, d in enumerate(analogs):
        p = f"analog[{i}]"
        d = v.mapping(d, p, analogs.line_of(i), ["label", "pin", "cc", "raw_min", "raw_max"])
        if d is None:
            continue
        lab = v.label(d, p, seen)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog")
        cc = v.integer(d, "cc", p, 0, 127, default=0, required=True)
        lo = v.integer(d, "raw_min", p, 0, 4095, default=0)
        hi = v.integer(d, "raw_max", p, 0, 4095, default=4095)
        if lo == hi:
            v.err(p, d.line, f"raw_min and raw_max are both {lo}")
        dev.analogs.append(Hub(label=lab or "", pin=pin or NO_PIN, cc=cc, raw_min=lo, raw_max=hi))

    encs = v.seq(data.get("encoders"), "encoders", data.line_of("encoders"))
    seen = {}
    for i, d in enumerate(encs):
        p = f"encoders[{i}]"
        d = v.mapping(d, p, encs.line_of(i), ["label", "a", "b", "cc", "steps_per_detent"])
        if d is None:
            continue
        lab = v.label(d, p, seen)
        enc_hubs = IN_HUBS - MUX_HUBS
        why = "encoders can't be on a mux"
        a = v.pin(d.get("a"), f"{p}.a", d.line_of("a"), "in", hubs=enc_hubs, why=why)
        b = v.pin(d.get("b"), f"{p}.b", d.line_of("b"), "in", hubs=enc_hubs, why=why)
        for key, pn in (("a", a), ("b", b)):
            if pn and pn.hub and dev.hubs[pn.hub - 1].type in I2C_HUBS:
                v.warn(f"{p}.{key}", d.line_of(key), "encoder on an I2C hub is polled from the main "
                                                     "loop: fast spinning may miss steps")
        cc = v.integer(d, "cc", p, 0, 127, default=0, required=True)
        spd = v.integer(d, "steps_per_detent", p, 1, 255, default=4)
        dev.encoders.append(Hub(label=lab or "", a=a or NO_PIN, b=b or NO_PIN, cc=cc, steps=spd))

    if data.get("key_matrix") is not None:
        validate_matrix(v, dev, data["key_matrix"], data.line_of("key_matrix"))

    # boot combo
    if data.get("boot_combo") is not None:
        bc = v.seq(data["boot_combo"], "boot_combo", data.line_of("boot_combo"))
        for i, lab in enumerate(bc):
            lab = str(lab)
            p, ln = f"boot_combo[{i}]", bc.line_of(i)
            if lab not in btn_index:
                close = difflib.get_close_matches(lab, list(btn_index), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(p, ln, f"no button labelled '{lab}' (buttons: {', '.join(btn_index) or 'none'}{hint})")
            elif btn_index[lab] >= 32:
                v.err(p, ln, f"button '{lab}' is button {btn_index[lab]}; boot_combo can only use "
                             "the first 32 buttons")
            elif btn_index[lab] in dev.boot_combo:
                v.warn(p, ln, f"'{lab}' is listed twice")
            else:
                dev.boot_combo.append(btn_index[lab])

    if len(dev.buttons) > 32:
        v.err("buttons", data.line_of("buttons"), f"{len(dev.buttons)} buttons: at most 32 (the runtime "
                                                  "keeps button state in a 32-bit mask)")
    for kind, lst in (("buttons", dev.buttons), ("leds", dev.leds), ("analog", dev.analogs),
                      ("encoders", dev.encoders)):
        if len(lst) > 255:
            v.err(kind, data.line_of(kind), f"{len(lst)} items: at most 255")

    for h in dev.hubs:
        if not h.used:
            v.warn(h.path, h.line, f"hub '{h.id}' is not used by any component")
    v.check_uses()
    if not (dev.buttons or dev.analogs or dev.encoders or dev.matrix):
        v.warn("", data.line, "the device has no inputs (buttons, analog, encoders or key_matrix)")
    return (None if errors(v.diags) else dev), v.diags


def validate_hub(v, dev, h, p, line, index):
    if not isinstance(h, dict):
        v.err(p, line, f"must be a mapping (got {type_name(h)})")
        return
    hid = v.string(h, "id", p, required=True)
    typ = h.get("type")
    if typ not in HUB_ENUM:
        close = difflib.get_close_matches(str(typ), list(HUB_ENUM), 1)
        hint = f"; did you mean '{close[0]}'?" if close else ""
        v.err(f"{p}.type", h.line_of("type"), f"{'unknown hub type ' + repr(typ) if typ else 'is required'} "
                                               f"(known: {', '.join(HUB_ENUM)}{hint})")
        typ = None
    keys = sorted(HUB_COMMON_KEYS | HUB_KEYS.get(typ, set()))
    if typ:
        v.mapping(h, p, line, keys)
    if hid is not None and not re.fullmatch(r"[A-Za-z_][\w-]*", hid):
        v.err(f"{p}.id", h.line_of("id"), f"'{hid}' must be a simple name (letters, digits, _ and -)")
    if hid in v.hub_by_id:
        v.err(f"{p}.id", h.line_of("id"), f"hub id '{hid}' is already used by {v.hub_by_id[hid].path}")
        hid = None
    shared = v.boolean(h, "shared", p)
    hub = Hub(id=hid or f"#{index}", type=typ, index=index, path=p, line=line, used=False, chain=0,
              n_sel=0, addr=0, data=NO_PIN, clock=NO_PIN, latch=NO_PIN, sel=[], common=None,
              common_adc=False, enable=NO_PIN, settle_us=0, n_pins=0, shared=shared)
    if hid is not None:
        v.hub_by_id[hid] = hub
    dev.hubs.append(hub)
    if typ is None:
        return
    if shared and typ in I2C_HUBS:
        v.warn(f"{p}.shared", h.line_of("shared"), "I2C hubs always share the bus; shared: has no effect")

    def ctl(key, role, required=True, idx=None):
        pp = f"{p}.{key}" if idx is None else f"{p}.{key}[{idx}]"
        val = h.get(key) if idx is None else h[key][idx]
        ln = h.line_of(key) if idx is None else h[key].line_of(idx)
        if val is None:
            if required:
                v.err(pp, h.line, "is required")
            return NO_PIN
        tag = key if idx is None else f"{key}{idx}"
        return v.pin(val, pp, ln, role, allow_hub=False, ctrl=tag, shared=shared, owner=index) or NO_PIN

    if typ in SHIFT_HUBS:
        hub.chain = v.integer(h, "chain", p, 1, 4, default=1)   # vpin.c: at most 4 chips (32 pins)
        hub.n_pins = 8 * hub.chain
        hub.data = ctl("data", "out" if typ == "74hc595" else "in")
        hub.clock = ctl("clock", "out")
        hub.latch = ctl("latch", "out")
        if typ == "74hc595":
            hub.enable = ctl("enable", "out", required=False)
    elif typ in MUX_HUBS:
        want = {"74hc4051": 3, "74hc4067": 4}.get(typ)
        sel = h.get("select")
        if not isinstance(sel, list) or not sel:
            v.err(f"{p}.select", h.line_of("select"), f"needs a list of {want or '1-4'} select pins (S0 first)")
            sel = []
        elif want and len(sel) != want:
            v.err(f"{p}.select", h.line_of("select"), f"a {typ} has {want} select lines (S0..S{want - 1}), "
                                                      f"got {len(sel)}")
        elif not 1 <= len(sel) <= 4:
            v.err(f"{p}.select", h.line_of("select"), f"a mux has 1 to 4 select lines, got {len(sel)}")
        sel = sel[:4]
        hub.sel = [ctl("select", "out", idx=i) for i in range(len(sel))]
        hub.n_sel = len(sel) or (want or 0)
        hub.n_pins = 1 << hub.n_sel if hub.n_sel else 0
        hub.common = ctl("common", "in")
        if hub.common.hub == 0:
            hub.common_adc = v.plat.adc(hub.common.num)[0]
            if hub.common_adc and v.plat.adc(hub.common.num)[1]:
                v.warn(f"{p}.common", h.line_of("common"), v.plat.adc(hub.common.num)[1])
        else:
            hub.common = None
        hub.enable = ctl("enable", "out", required=False)
        hub.settle_us = v.integer(h, "settle_us", p, 0, 0xFFFF, default=0)
    else:
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x20)
        hub.n_pins = 16 if typ == "mcp23017" else 8
        ok = range(0x20, 0x28) if typ == "mcp23017" else list(range(0x20, 0x28)) + list(range(0x38, 0x40))
        if hub.addr not in ok:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for a {typ} "
                                                         f"(usually 0x20-0x27{'' if typ == 'mcp23017' else ', PCF8574A 0x38-0x3F'})")


def validate_matrix(v, dev, d, line):
    p = "key_matrix"
    d = v.mapping(d, p, line, ["kind", "rows", "cols", "keys", "first_note", "settle_us"])
    if d is None:
        return
    kind = d.get("kind")
    if kind not in ("switch", "velocity"):
        v.err(f"{p}.kind", d.line_of("kind"), f"{'is required' if kind is None else repr(kind) + ' is unknown'} "
                                               "(switch: one contact per key, velocity: two contacts per key)")
        kind = "switch"
    pins = {}
    for key, role, mh in (("rows", "out", {"74hc595"}), ("cols", "in", {"74hc165"})):
        lst = d.get(key)
        if not isinstance(lst, list) or not lst:
            v.err(f"{p}.{key}", d.line_of(key), "needs a list of pins")
            lst = LList()
        if len(lst) > 255:
            v.err(f"{p}.{key}", d.line_of(key), f"{len(lst)} {key}: at most 255")
        why = "the matrix is scanned from the timer interrupt, so only MCU pins, 74hc595 rows and 74hc165 cols"
        pins[key] = [v.pin(x, f"{p}.{key}[{i}]", lst.line_of(i), role, hubs=mh, why=why) or NO_PIN
                     for i, x in enumerate(lst)]
    nr, nc = len(pins["rows"]), len(pins["cols"])
    cap = nr * nc if kind == "switch" else nr * (nc // 2)
    if kind == "velocity" and nc % 2:
        v.err(f"{p}.cols", d.line_of("cols"), f"kind velocity needs an even number of columns "
                                              f"(two contacts per key), got {nc}")
    keys = v.integer(d, "keys", p, 1, 128, default=min(cap, 128) or 1)
    first = v.integer(d, "first_note", p, 0, 127, default=48)
    if keys is not None and nr and nc and keys > cap:
        v.err(f"{p}.keys", d.line_of("keys"), f"{keys} keys don't fit a {nr} x {nc} {kind} matrix "
                                              f"(at most {cap})")
    if keys is not None and first is not None and first + keys > 128:
        v.err(f"{p}.keys", d.line_of("keys"), f"first_note {first} + {keys} keys goes past note 127")
    settle = v.integer(d, "settle_us", p, 0, 255, default=1)
    dev.matrix = Hub(kind=kind, rows=pins["rows"], cols=pins["cols"], keys=keys or 0,
                     first_note=first or 0, settle_us=settle)


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
         f"#define GM_DEVICE_NAME {c_str(dev.name)}",
         f"#define GM_BOARD_ID 0x{dev.board_id:04X}",
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
    if dev.i2c:
        h += [f"#define GM_I2C_SDA {dev.i2c[0].num} /* {dev.i2c[0].desc} */",
              f"#define GM_I2C_SCL {dev.i2c[1].num} /* {dev.i2c[1].desc} */"]
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
        c += [f"    {{ {pin_c(b.pin)}, {c_str(b.label)}, {b.led} }}, /* {_comment(b.pin.desc)} */"
              for b in dev.buttons]
        c += ["};", ""]
    if dev.leds:
        c.append("static const gm_led_t leds[GM_N_LEDS] = {")
        c += [f"    {{ {pin_c(x.pin)}, {pin_c(x.pin2)}, {c_str(x.label)} }}, /* {_comment(x.pin.desc)} */"
              for x in dev.leds]
        c += ["};", ""]
    if dev.analogs:
        c.append("static const gm_analog_t analogs[GM_N_ANALOGS] = {")
        c += [f"    {{ {pin_c(a.pin)}, {c_str(a.label)}, {a.raw_min}, {a.raw_max} }}, /* {_comment(a.pin.desc)} */"
              for a in dev.analogs]
        c += ["};", ""]
    if dev.encoders:
        c.append("static const gm_encoder_t encoders[GM_N_ENCODERS] = {")
        c += [f"    {{ {pin_c(e.a)}, {pin_c(e.b)}, {c_str(e.label)}, {e.steps} }}, "
              f"/* {_comment(e.a.desc)}, {_comment(e.b.desc)} */" for e in dev.encoders]
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
    return {"gm_device_config.h": "\n".join(h), "gm_device_config.c": "\n".join(c)}


def write_generated(dev, out_dir, source):
    os.makedirs(out_dir, exist_ok=True)
    files = generate(dev, source)
    for name, text in files.items():
        p = os.path.join(out_dir, name)
        tmp = p + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, p)
    return [os.path.join(out_dir, n) for n in files]
