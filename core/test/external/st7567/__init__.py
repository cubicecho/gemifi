"""st7567: an ST7567 128x64 LCD on the I2C bus, as an external component (core/README.md "External
components"): the display platform the core tests and tools/builder/tests/test_external.py load.
"""
from components.display import ScreenType

GEMIFI_API = 1
DEPENDENCIES = ("display",)
PLATFORMS = ("esp32", "esp32s2", "esp32s3")     # I2C is ESP only for now


class ST7567(ScreenType):
    """An ST7567 LCD: 128x64, written a changed page at a time."""
    platform = "st7567"
    keys = ("id", "platform", "address", "i2c_id", "contrast")
    i2c = True
    schema = {"contrast": (0, 63)}
    c_header, c_config, c_ops = "st7567.h", ("gm_screen_st7567_t", "st7567"), "gm_st7567_ops"

    def validate(self, v, dev, scr, d, p):
        scr.addr = v.integer(d, "address", p, 0x08, 0x77, default=0x3F)
        scr.width, scr.height = 128, 64
        scr.contrast = v.integer(d, "contrast", p, 0, 63, default=40)

    def c_fields(self, scr):
        return [f"        .st7567 = {{ .addr = 0x{scr.addr:02X}, .bus = {scr.bus}, .contrast = {scr.contrast} }},"]


DISPLAY_PLATFORMS = (ST7567(),)
