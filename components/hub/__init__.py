"""hub: the base component of the chips that give a device pins (shift registers, I/O expanders,
multiplexers, LED drivers ...). Each hub type is a component of its own (components/<chip>/) whose
__init__.py lists its HubType instances in HUB_TYPES; this is the class they derive from, with what
several types share (ShiftRegister, I2CExpander). Interchangeable chips for one job are platforms of a
base component of their own (led_strip), whose class derives from this one.

The C side is gm_hub.h here (gm_hub_ops_t) and core/src/vpin.c; README.md here.
"""
import re

# The core's pin dispatch (core/src/vpin.c) is always built, so this is too.
REQUIRED = True


class HubType:
    key = ""                    # the YAML list key, and the pin key ({ <key>: <hub id>, number: n })
    pin_key = None              # ... unless the pin key differs (mcp23017: mcp23xxx)
    # A platform of a domain: the base component whose YAML list it is in, ESPHome style (led_strip:
    # [{ id, platform: <key>, ... }]); the domain is then the list key and the pin key.
    domain = None
    old_keys = ()               # older list and pin keys, refused with moved_text()
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

    def moved_text(self):
        """What an old list or pin key (old_keys) says: the new form."""
        raise NotImplementedError(self.key)

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


class ShiftRegister(HubType):
    """74HC595 (outputs) / 74HC165 (inputs): sr_count chips in a chain, 8 pins each. The two share it
    (components/sn74hc595, sn74hc165), and their config (.sr, gm_device.h)."""
    lines = {"clock_pin": "clock", "latch_pin": "latch", "load_pin": "latch", "data_pin": "data", "oe_pin": "enable"}

    def __init__(self, key, enum, num, out, c_ops=None):
        self.key, self.enum, self.num, self.out, self.c_ops = key, enum, num, out, c_ops
        self.keys = ("id", "data_pin", "clock_pin", "latch_pin", "oe_pin", "sr_count") if out else \
            ("id", "data_pin", "clock_pin", "load_pin", "sr_count")
        self.roles = frozenset({"out"} if out else {"in"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "sr_count", p, 1, 4, default=1)   # the drivers: at most 4 chips (32 pins)
        hub.n_pins = 8 * hub.chain
        hub.data = ctl("data_pin", "out" if self.out else "in")
        hub.clock = ctl("clock_pin", "out")
        if self.out:
            hub.latch = ctl("latch_pin", "out")
            hub.enable = ctl("oe_pin", "out", required=False)
        else:
            hub.latch = ctl("load_pin", "out")

    def range_text(self, hub):
        return f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8)"

    def c_fields(self, hub):
        return [f"        .sr = {{ .data = {hub.data.c()}, .clock = {hub.clock.c()}, .latch = {hub.latch.c()},",
                f"                .oe = {hub.enable.c()}, .chain = {hub.chain} }},"]


class I2CExpander(HubType):
    """MCP23017 (16 GPIO) / PCF8574 (8 quasi-bidirectional) on the i2c: bus, read and written from
    the main loop: the two share it (components/mcp23017, pcf8574), and their config (.i2c) and pin
    state (gm_hub.h u.io, gm_hub_i2c_*)."""
    keys = ("id", "address", "i2c_id")
    roles = frozenset({"in", "out"})
    i2c = True

    def __init__(self, key, enum, num, n_pins, addrs, addr_text, pin_key=None, c_ops=None):
        self.key, self.enum, self.num, self.n_pins = key, enum, num, n_pins
        self.addrs, self.addr_text, self.pin_key, self.c_ops = addrs, addr_text, pin_key, c_ops
        self.pullup = key == "mcp23017"

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x20)
        hub.n_pins = self.n_pins
        if hub.addr not in self.addrs:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for a {self.key} "
                                                         f"(usually {self.addr_text})")

    def c_fields(self, hub):
        return [f"        .i2c = {{ .addr = 0x{hub.addr:02X}, .bus = {hub.bus} }},"]

    def pin_name(self, s):
        if self.key == "mcp23017":
            m = re.fullmatch(r"(?:GP)?([AB])([0-7])", s)
            return (8 if m.group(1) == "B" else 0) + int(m.group(2)) if m else None
        m = re.fullmatch(r"P([0-7])", s)
        return int(m.group(1)) if m else None


def c_comment(s):
    """s, safe inside a C comment."""
    return s.replace("*/", "* /")
