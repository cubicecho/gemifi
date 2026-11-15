"""What the builder gives components: the pin model, the record type (Hub), the YAML values (LDict,
LList), C strings, a few text helpers and data, and what the device model (dev) alone says about the
MIDI the device sends (sysex_wide, mode_wide, f1_on). A leaf module (it imports nothing of the builder's), so
a component's __init__.py can import it at load time; the builder (tools/builder/gemifi_device.py)
imports components, and passes the rest (the validator, the platform) to their calls.
"""

# gm_pin_t.flags (core/include/gm_pin.h)
INVERT, PULLUP, PULLDOWN, OPEN_DRAIN = 1, 2, 4, 8


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

# The velocity curves (gm_settings.h CURVE_*): velocity.curve, pad_settings.curve.
CURVES = ["normal", "soft", "hard", "fixed"]

# The old label: key of every list item (format 1).
RENAMED_ID = {"label": "renamed: use `id:` (lower_snake_case, e.g. `oct_down`, used for references) "
                       "and/or `name:` (display text)"}

# The analog / encoder modes that send 14-bit values (mode sysex does when its string has msb / lsb too).
WIDE_MODES = {"cc14", "nrpn14", "rpn14"}

# The white keys of an octave (pitch classes): a key matrix's white_from:, the layout's keyboard.
WHITE = {0, 2, 4, 5, 7, 9, 11}

# Said after the message for a key that moved onto a base component (old_keys).
MIGRATE_HINT = " (`gemifi_build.py migrate --write <file>` rewrites a file to the current form)"


class Hub:
    """A record of the device model (a hub, a button, an LED, a mode ...): its fields as attributes."""

    def __init__(self, **kw):
        self.__dict__.update(kw)


def c_str(s):
    """s as a C string literal (UTF-8, escaped)."""
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


def c_param_array(name, count, vals, comment=""):
    """A param's per-control defaults, const int32_t name[count] = { vals }; (count: the controls' GM_N_*
    macro), then comment. Without controls the array still exists, as name[1] = { 0 }."""
    size = count if vals else "1"
    body = ", ".join(str(x) for x in vals) if vals else "0"
    return f"const int32_t {name}[{size}] = {{ {body} }};{comment}"


def twin_c(x):
    """gm_button_t / gm_led_t / gm_encoder_t .layer, .base for a layer twin (mode index + 1, the component
    it is a twin of); 0, 0 for the others."""
    return f", {x.layer}, {x.base}" if getattr(x, "layer", 0) else ", 0, 0"


# The YAML values the builder hands components: mappings and lists that know their lines.
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


def type_name(v):
    return {dict: "mapping", LDict: "mapping", list: "list", LList: "list", str: "string", int: "integer",
            bool: "boolean", float: "number", type(None): "nothing"}.get(type(v), type(v).__name__)


def errors(diags):
    """The errors among diags (Diag: .level is 'error' or 'warning')."""
    return [d for d in diags if d.level == "error"]


def a_(kind):
    """'an LED', 'an analog input', 'a button'."""
    return f"{'an' if kind[:1] in 'aeiouAEIOUL' else 'a'} {kind}"


# What the device model (dev) says about the MIDI the device sends and reads.
def sysex_wide(dev, k):
    """SysEx string k (an index into midi_messages.sysex) sends 14-bit values."""
    return 0 <= k < len(dev.sysex) and bool(dev.sysex[k].flags & 1)


def mode_wide(dev, mname, num):
    """An analog / encoder mode sends 14-bit values (num: its SysEx string, for mode sysex)."""
    return mname in WIDE_MODES or (mname == "sysex" and sysex_wide(dev, num))


def f1_on(dev):
    """Plain feedback reaches the main port: enabled, and the main port isn't the MCU port."""
    return dev.feedback and not (dev.mcu is not None and dev.mcu.cable == 0)


def f1_off_why(dev):
    """Why plain feedback is off (f1_on), for a message."""
    if not dev.feedback:
        return "feedback is off (feedback: { enabled: false })"
    return ("with mcu: on a single port (no USB DAW port), everything the host sends is read as "
            "Mackie Control")
