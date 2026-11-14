"""ssd1306: SSD1306 and SH1106 OLEDs on the I2C bus, a display platform on the display base
(components/display): `display: [{ id: oled, platform: ssd1306, address: 0x3C }]`. The YAML, the C and
the wiring: README.md here.
"""
from components.display import ScreenType

OLED_MODELS = {"ssd1306": 0, "sh1106": 1}       # ssd1306.h GM_OLED_SH1106
OLED_SIZES = {"ssd1306": [(128, 64), (128, 32), (96, 16), (64, 48), (64, 32)], "sh1106": [(128, 64)]}
OLED_8BIT = {0x78: 0x3C, 0x7A: 0x3D}           # the 8-bit write addresses printed on many modules

DEPENDENCIES = ("display",)


class SSD1306(ScreenType):
    """An SSD1306 or SH1106 OLED on the I2C bus, written from the main loop a run of changed bytes at a
    time. ESP only for now, as the I2C hubs."""
    platform, enum, num = "ssd1306", "GM_SCREEN_SSD1306", 1
    keys = ("id", "platform", "address", "model", "width", "height", "contrast", "rotate")
    i2c = True
    schema = {"width": (64, 128), "height": (16, 64), "contrast": (0, 255)}
    enums = {"model": list(OLED_MODELS), "rotate": [0, 180]}
    c_header, c_config, c_ops = "ssd1306.h", ("gm_screen_ssd1306_t", "ssd1306"), "gm_ssd1306_ops"

    def validate(self, v, dev, scr, d, p):
        model = d.get("model", "ssd1306")
        model = model.lower() if isinstance(model, str) else model
        if model not in OLED_MODELS:
            v.err(f"{p}.model", d.line_of("model"), f"unknown model '{d.raw_of('model')}' "
                                                    f"(use {', '.join(OLED_MODELS)})")
            model = "ssd1306"
        scr.model = model
        addr = v.integer(d, "address", p, 0x08, 0x7A, default=0x3C)
        if addr in OLED_8BIT:
            v.warn(f"{p}.address", d.line_of("address"), f"0x{addr:02X} is the 8-bit form (as printed on many "
                                                         f"boards) of I2C address 0x{OLED_8BIT[addr]:02X}: using "
                                                         f"0x{OLED_8BIT[addr]:02X} (write that to drop this warning)")
            addr = OLED_8BIT[addr]
        elif addr > 0x77:
            v.err(f"{p}.address", d.line_of("address"), f"0x{addr:02X} is reserved on I2C (an OLED is 0x3C or "
                                                        "0x3D; 0x78 / 0x7A are those as 8-bit addresses)")
        elif addr not in (0x3C, 0x3D):
            v.warn(f"{p}.address", d.line_of("address"), f"0x{addr:02X} is unusual for an OLED (0x3C or 0x3D)")
        scr.addr = addr
        sizes = OLED_SIZES[model]
        scr.width = v.integer(d, "width", p, 64, 128, default=sizes[0][0])
        scr.height = v.integer(d, "height", p, 16, 64, default=sizes[0][1])
        if (scr.width, scr.height) not in sizes:
            v.err(p, d.line_of("height") or d.line_of("width") or d.line,
                  f"{scr.width}x{scr.height}: a {model} is " + ", ".join(f"{w}x{ht}" for w, ht in sizes))
            scr.width, scr.height = sizes[0]
        scr.contrast = v.integer(d, "contrast", p, 0, 255, default=0xCF)
        rot = d.get("rotate", 0)
        if rot not in (0, 180):
            v.err(f"{p}.rotate", d.line_of("rotate"), f"rotate: {d.raw_of('rotate')}: 0 or 180 (degrees)")
            rot = 0
        scr.rotate = rot

    def c_fields(self, scr):
        flags = " | ".join(f for f, on in (("GM_OLED_SH1106", scr.model == "sh1106"),
                                          ("GM_OLED_ROTATE", scr.rotate == 180)) if on) or "0"
        return [f"        .ssd1306 = {{ .addr = 0x{scr.addr:02X}, .contrast = {scr.contrast}, .flags = {flags} }},"]


DISPLAY_PLATFORMS = (SSD1306(),)
