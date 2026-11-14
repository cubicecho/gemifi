"""hub: the base component of the chips that give a device pins (shift registers, I/O expanders,
multiplexers, LED drivers ...). Each hub type is a component of its own (components/<chip>/) whose
__init__.py lists its HubType instances in HUB_TYPES; this is the class they derive from.

The C side is gm_hub.h here (gm_hub_ops_t) and core/src/vpin.c; core/README.md "Virtual pins and
hubs".
"""

# The core's pin dispatch (core/src/vpin.c) is always built, so this is too.
REQUIRED = True


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
    # The C side, for a hub type whose code is in its component (components/<name>/; gm_components.h):
    c_header = None             # its header, e.g. "ws2812.h"
    c_config = None             # its config in gm_hub_t's union: (C type, member), e.g. ("gm_hub_ws2812_t", "ws")
    c_state = None              # its state in gm_hub_state_t's union, the same way (or None)
    c_ops = None                # its gm_hub_ops_t, e.g. "gm_ws2812_ops"

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


def c_comment(s):
    """s, safe inside a C comment."""
    return s.replace("*/", "* /")
