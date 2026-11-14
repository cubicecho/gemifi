"""max7219: a chain of MAX7219 / MAX7221 LED drivers, bit-banged from the main loop, a platform on the
led_driver base (components/led_driver): `led_driver: [{ id: digits, platform: max7219, ... }]`. The
YAML and the C: README.md here.
"""
from components.led_driver import LedDriver

DEPENDENCIES = ("led_driver",)

# MAX7219 / MAX7221 LED drivers in a chain: pin = chip * 64 + digit * 8 + segment (0..6 = A..G,
# 7 = DP), so a 7-segment character is its digit's 8 pins (displays: type segment7).
MAX7219_CHIPS = 8


class MAX7219(LedDriver):
    """A chain of MAX7219 LED drivers (ESPHome max7219 keys), bit-banged from the main loop."""
    key, enum, num = "max7219", "GM_HUB_MAX7219", 12
    old_keys = ("max7219",)
    keys = ("id", "platform", "clk_pin", "din_pin", "cs_pin", "num_chips", "intensity", "scan_limit")
    # chains may share CLK and DIN, each with its own CS
    lines = {"clk_pin": "mx_clk", "din_pin": "mx_din", "cs_pin": "mx_cs"}
    schema = {"num_chips": (1, MAX7219_CHIPS), "intensity": (0, 15), "scan_limit": (1, 8)}
    c_header, c_config, c_ops = "max7219.h", ("gm_hub_max7219_t", "mx"), "gm_max7219_ops"
    c_state = ("gm_led_driver_state_t", "led_driver")

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "num_chips", p, 1, MAX7219_CHIPS, default=1)
        hub.n_pins = 64 * hub.chain
        hub.ram_bytes = 8 * hub.chain                    # a digit register per byte
        hub.intensity = v.integer(h, "intensity", p, 0, 15, default=15)
        hub.grids = v.integer(h, "scan_limit", p, 1, 8, default=8)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("din_pin", "out")
        hub.latch = ctl("cs_pin", "out")

    def range_text(self, hub):
        return (f" ({hub.chain} chip{'s' if hub.chain > 1 else ''} x 8 digits x 8; pin = chip * 64 + "
                "digit * 8 + segment, 0..6 = A..G, 7 = DP)")

    def c_fields(self, hub):
        return [f"        .mx = {{ .clk = {hub.clock.c()}, .din = {hub.data.c()}, .cs = {hub.latch.c()},",
                f"                .chips = {hub.chain}, .intensity = {hub.intensity}, "
                f".scan_limit = {hub.grids} }},"]


HUB_TYPES = (MAX7219(),)
