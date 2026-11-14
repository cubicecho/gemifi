"""tm1637: a TM1637 2-wire LED driver, bit-banged from the main loop, a platform on the led_driver base
(components/led_driver): `led_driver: [{ id: digits, platform: tm1637, ... }]`. The YAML and the C:
README.md here.
"""
from components.led_driver import LedDriver

DEPENDENCIES = ("led_driver",)


class TM1637(LedDriver):
    """A TM1637 LED driver (2-wire, ESPHome tm1637 keys), bit-banged from the main loop: pin = grid * 8
    + segment (bit 0 = SEG1 = a ... bit 7 = SEG8)."""
    key, enum, num = "tm1637", "GM_HUB_TM1637", 13
    old_keys = ("tm1637",)
    keys = ("id", "platform", "clk_pin", "dio_pin", "intensity", "length")
    lines = {"clk_pin": "tm37_clk", "dio_pin": "tm37_dio"}
    schema = {"length": (1, 6), "intensity": (0, 7)}
    c_header, c_config, c_ops = "tm1637.h", ("gm_hub_tm1637_t", "tm37"), "gm_tm1637_ops"
    c_state = ("gm_led_driver_state_t", "led_driver")

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.ram_bytes = v.integer(h, "length", p, 1, 6, default=6)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = v.integer(h, "intensity", p, 0, 7, default=7)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        for k in ("clk_pin", "dio_pin"):
            x = h.get(k)
            if isinstance(x, dict) and x.get("allow_other_uses"):
                v.err(f"{p}.{k}", h.line_of(k), "a tm1637 has no chip select: its clk_pin and dio_pin "
                                               "can't be shared")

    def range_text(self, hub):
        return f" ({hub.ram_bytes} grids x 8; pin = grid * 8 + segment)"

    def c_fields(self, hub):
        return [f"        .tm37 = {{ .clk = {hub.clock.c()}, .dio = {hub.data.c()}, .grids = {hub.ram_bytes}, "
                f".brightness = {hub.intensity} }},"]


HUB_TYPES = (TM1637(),)
