"""display: the base component of screens, the panels the YAML's display: list declares
(`display: [{ id: oled, platform: ssd1306, ... }]`) and text displays (displays:, `display: oled`) draw
on. Each platform is a component of its own (components/<platform>/) whose __init__.py lists its
ScreenType instances in DISPLAY_PLATFORMS. The C side is gm_screen.h and screen.c here; README.md.
"""


class ScreenType:
    platform = ""               # the YAML platform: name, and the component that has it
    enum = ""                   # gm_screen.h GM_SCREEN_*
    num = 0                     # ... its value
    keys = ("id", "platform")   # a screen's YAML keys
    i2c = False                 # on the i2c: bus (address:)
    schema = {}                 # key -> (min, max) for the JSON schema
    enums = {}                  # key -> allowed values, for the JSON schema
    # The C side (gm_components.h): its header, its config in gm_screen_t's union (C type, member) and
    # its gm_screen_ops_t.
    c_header = None
    c_config = None
    c_ops = None

    def validate(self, v, dev, scr, d, p):
        """Check screen d (the YAML mapping at path p) and fill in scr: width and height (pixels, height a
        multiple of 8), addr for an I2C one."""
        raise NotImplementedError(self.platform)

    def c_fields(self, scr):
        """The C lines of the entry's config: its gm_screen_t union member, e.g. `.ssd1306 = { ... },`."""
        raise NotImplementedError(self.platform)
