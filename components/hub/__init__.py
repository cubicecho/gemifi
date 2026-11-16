"""hub: the base component of the chips that give a device pins (shift registers, I/O expanders,
multiplexers, LED drivers ...). Each hub type is a component of its own (components/<chip>/) whose
__init__.py lists its HubType instances in HUB_TYPES; this is the class they derive from, with what
several types share (ShiftRegister, I2CExpander). Interchangeable chips for one job are platforms of a
base component of their own (led_strip), whose class derives from this one.

The C side is gm_hub.h here (gm_hub_ops_t) and core/src/vpin.c; README.md here.
"""
import re

from components.i2c import I2C_ID, address
from components.keys import Id, Int, Key, Keys, Pin

# The core's pin dispatch (core/src/vpin.c) is always built, so this is too.
REQUIRED = True


# gm_device.h GM_HUB_N_TYPES: one more than the last built-in GM_HUB_* (15 is retired). External hub types
# (core/README.md "External components") are numbered from here (tests/test_external.py checks the header).
BUILTIN_TYPES = 16


class HubType:
    key = ""                    # the YAML list key, and the pin key ({ <key>: <hub id>, number: n })
    pin_key = None              # ... unless the pin key differs (mcp23017: mcp23xxx)
    # A platform of a domain: the base component whose YAML list it is in, ESPHome style (led_strip:
    # [{ id, platform: <key>, ... }]); the domain is then the list key and the pin key.
    domain = None
    old_keys = ()               # older list and pin keys, refused with moved_text() (gemifi_build.py migrate
                                # rewrites them)
    enum = ""                   # gm_device.h GM_HUB_*
    num = 0                     # ... its value
    # Its keys as data (components/keys.py Keys: type, default, doc): the JSON schema and the docs take
    # each key's type from it, and `keys` (the validator's list) is its names.
    spec = None
    keys = ("id",)              # a hub's YAML keys
    lines = {}                  # control-pin keys: the line kind, for allow_other_uses (same kind on each)
    roles = frozenset()         # what its pins can be: "in", "out", "analog" (and "row": decoder outputs)
    ram = False                 # its pins are in the hub RAM (GM_HUB_RAM): ram_bytes of it
    # Its LEDs show any colour (vpin.c: the type has an `rgb` op): an LED is colour_pins(hub) pins in a
    # row from LED * colour_pins (red, green, blue[, white]), and takes full-colour looks.
    full_colour = False
    # core/src/vpin.c has code of its own for this type, by its GM_HUB_* number, beyond its gm_hub_ops_t (the mux's
    # channel reads and analog scan, the decoder's rows, the switch matrix's scan, the TM1628's fade, the XY pad's
    # touch): an external component can't replace it.
    core_paths = False
    pullup = False              # its pins have a pull-up (pin mode INPUT_PULLUP)
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

    def final_validate(self, v, dev, hubs):
        """After every hub is validated, so every hub id is known (ESPHome's FINAL_VALIDATE_SCHEMA):
        checks across hubs, this type's (hubs, in file order: those with this key), and anything that
        names a hub declared further down."""

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


def check_own_latch(v, hubs, key, what, shared):
    """Each of hubs (on one bus: clock and data pins shared) needs its own MCU latch pin (hub.latch, YAML
    key): what = 'chips' or 'chains', shared = the keys they may share, for the message."""
    seen = {}
    for h in hubs:
        if h.latch.hub != 0:
            continue
        if h.latch.num in seen:
            v.err(f"{h.path}.{key}", h.line, f"{h.latch.desc} is already the {key} of hub '{seen[h.latch.num]}': "
                                             f"{what} can share {shared}, but each needs its own {key}")
        seen.setdefault(h.latch.num, h.id)


class ShiftRegister(HubType):
    """74HC595 (outputs) / 74HC165 (inputs): sr_count chips in a chain, 8 pins each. The two share it
    (components/sn74hc595, sn74hc165), and their config (.sr, gm_device.h)."""
    lines = {"clock_pin": "clock", "latch_pin": "latch", "load_pin": "latch", "data_pin": "data", "oe_pin": "enable"}

    def __init__(self, key, enum, num, out, c_ops=None):
        self.key, self.enum, self.num, self.out, self.c_ops = key, enum, num, out, c_ops
        own = f"What its pins name: `{{ {key}: <id>, number: n }}`."
        count = Key("sr_count", Int(1, 4), default=1,       # the drivers: at most 4 chips (32 pins)
                    doc=f"The chips in the chain, 8 {'outputs' if out else 'inputs'} each (8..15 on the second).")
        if out:
            self.spec = Keys(
                Key("id", Id(), required=True, doc=own),
                Key("data_pin", Pin(), required=True, doc="The serial data in (SER)."),
                Key("clock_pin", Pin(), required=True, doc="The shift clock (SRCLK)."),
                Key("latch_pin", Pin(), required=True, doc="The latch (RCLK): a pulse puts the shifted bits on "
                                                           "the outputs."),
                Key("oe_pin", Pin(), doc="The output enable (/OE), driven low. Leave it out when /OE is tied low."),
                count)
        else:
            self.spec = Keys(
                Key("id", Id(), required=True, doc=own),
                Key("data_pin", Pin(), required=True, doc="The serial out (QH) of the chip nearest the MCU."),
                Key("clock_pin", Pin(), required=True, doc="The shift clock (CLK)."),
                Key("load_pin", Pin(), required=True, doc="The load pin (SH/LD): the inputs are loaded while it "
                                                          "is low."),
                count)
        self.keys = tuple(self.spec.names)
        self.roles = frozenset({"out"} if out else {"in"})

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = self.spec.get(v, h, "sr_count", p)
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
    roles = frozenset({"in", "out"})
    i2c = True

    def __init__(self, key, enum, num, n_pins, addrs, addr_text, pin_key=None, c_ops=None):
        self.key, self.enum, self.num, self.n_pins = key, enum, num, n_pins
        self.addrs, self.addr_text, self.pin_key, self.c_ops = addrs, addr_text, pin_key, c_ops
        self.pullup = key == "mcp23017"
        self.spec = Keys(
            Key("id", Id(), required=True, doc=f"What its pins name: `{{ {pin_key or key}: <id>, number: n }}`."),
            address(0x20, f"Its I2C address, set by its A0..A2 pins: usually {addr_text}."),
            I2C_ID)
        self.keys = tuple(self.spec.names)

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = self.spec.get(v, h, "address", p)
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
