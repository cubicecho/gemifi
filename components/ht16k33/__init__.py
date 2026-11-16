"""ht16k33: an HT16K33 LED driver on the I2C bus (the 14- and 7-segment backpacks), written from the main
loop, a platform on the led_driver base (components/led_driver): `led_driver: [{ id: alpha, platform:
ht16k33, address: 0x70 }]`. The YAML and the C: README.md here.
"""
from components.i2c import I2C_ID, address
from components.keys import Enum, Id, Int, Key, Keys
from components.led_driver import LedDriver

DEPENDENCIES = ("led_driver",)

SPEC = Keys(
    Key("id", Id(), required=True, doc="What its pins name: `{ led_driver: <id>, number: n }`."),
    Key("platform", Enum(["ht16k33"]), required=True, doc="This chip."),
    address(0x70, "Its I2C address, set by the board's address jumpers: usually 0x70-0x77."),
    Key("intensity", Int(0, 15), default=15, doc="The brightness."),
    I2C_ID,
)


class HT16K33(LedDriver):
    """An HT16K33 LED driver on the I2C bus (the 14- and 7-segment backpacks), written from the main
    loop: pin = COM * 16 + ROW. ESP only for now, as the other I2C hubs."""
    key, enum, num = "ht16k33", "GM_HUB_HT16K33", 14
    old_keys = ("ht16k33",)
    spec = SPEC
    keys = tuple(SPEC.names)
    i2c = True
    c_header, c_config, c_ops = "ht16k33.h", ("gm_hub_ht16k33_t", "ht"), "gm_ht16k33_ops"
    c_state = ("gm_led_driver_state_t", "led_driver")

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.addr = SPEC.get(v, h, "address", p)
        if not 0x70 <= hub.addr <= 0x77:
            v.warn(f"{p}.address", h.line_of("address"), f"0x{hub.addr:02X} is unusual for an ht16k33 "
                                                         "(0x70-0x77)")
        hub.ram_bytes = 16
        hub.n_pins = 128
        hub.intensity = SPEC.get(v, h, "intensity", p)

    def range_text(self, hub):
        return " (16 RAM bytes x 8; pin = COM * 16 + ROW)"

    def c_fields(self, hub):
        return [f"        .ht = {{ .addr = 0x{hub.addr:02X}, .bus = {hub.bus}, .brightness = {hub.intensity} }},"]


HUB_TYPES = (HT16K33(),)
