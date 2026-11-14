"""hub: the base component of the chips that give a device pins (shift registers, I/O expanders,
multiplexers, LED drivers ...). Each hub type is a component of its own (components/<chip>/) whose
__init__.py lists its HubType instances in HUB_TYPES; this is the class they derive from.

The C side is core/src/gm_hub.h (gm_hub_ops_t) and core/src/vpin.c; core/README.md "Virtual pins and
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
