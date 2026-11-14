"""ht16k33: an HT16K33 LED driver on the I2C bus (the 14- and 7-segment backpacks), written from the main
loop, a platform on the led_driver base (components/led_driver): `led_driver: [{ id: alpha, platform:
ht16k33, address: 0x70 }]`. The YAML and the C: README.md here.
"""
from components.led_driver import LedDriver

DEPENDENCIES = ("led_driver",)


class HT16K33(LedDriver):
    """An HT16K33 LED driver on the I2C bus (the 14- and 7-segment backpacks), written from the main
    loop: pin = COM * 16 + ROW. ESP only for now, as the other I2C hubs."""
    key, enum, num = "ht16k33", "GM_HUB_HT16K33", 14
    old_keys = ("ht16k33",)
    keys = ("id", "platform", "address", "intensity", "i2c_id")
    i2c = True
    schema = {"intensity": (0, 15)}
    c_header, c_config, c_ops = "ht16k33.h", ("gm_hub_ht16k33_t", "ht"), "gm_ht16k33_ops"
    c_state = ("gm_led_driver_state_t", "led_driver")

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = v.integer(h, "address", p, 0x08, 0x77, default=0x70)
        if not 0x70 <= hub.addr <= 0x77:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for an ht16k33 "
                                                         "(0x70-0x77)")
        hub.ram_bytes = 16
        hub.n_pins = 128
        hub.intensity = v.integer(h, "intensity", p, 0, 15, default=15)

    def range_text(self, hub):
        return " (16 RAM bytes x 8; pin = COM * 16 + ROW)"

    def c_fields(self, hub):
        return [f"        .ht = {{ .addr = 0x{hub.addr:02X}, .bus = {hub.bus}, .brightness = {hub.intensity} }},"]


HUB_TYPES = (HT16K33(),)
