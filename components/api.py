"""What the builder gives components: the pin model, the record type (Hub) and C strings. A leaf module
(it imports nothing of the builder's), so a component's __init__.py can import it at load time; the
builder (tools/builder/gemifi_device.py) imports components, and passes the rest (the validator, the
platform) to their calls.
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


def twin_c(x):
    """gm_button_t / gm_led_t / gm_encoder_t .layer, .base for a layer twin (mode index + 1, the component
    it is a twin of); 0, 0 for the others."""
    return f", {x.layer}, {x.base}" if getattr(x, "layer", 0) else ", 0, 0"
