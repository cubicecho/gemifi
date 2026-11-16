"""tm1628: TM1628-class LED drivers (TM1628, TM1638, TM1620 ...), bit-banged from the main loop, a platform
on the led_driver base (components/led_driver): `led_driver: [{ id: leds, platform: tm1628, ... }]`. The
YAML and the C: README.md here.
"""
from components.hub import check_own_latch
from components.keys import Bool, Enum, Id, Int, Key, Keys, Pin
from components.led_driver import LedDriver

DEPENDENCIES = ("led_driver",)

# TM1628-class LED drivers (TM1628, TM1638, TM1620 ...): pin = display RAM bit, byte * 8 + bit.
TM_RAM_MAX = 16                 # TM1638 has 16 bytes, TM1628 14

SPEC = Keys(
    Key("id", Id(), required=True, doc="What its pins name: `{ led_driver: <id>, number: n }`."),
    Key("platform", Enum(["tm1628"]), required=True, doc="This chip family."),
    Key("clk_pin", Pin(), required=True, doc="The clock pin (CLK). Chips can share it."),
    Key("dio_pin", Pin(), required=True, doc="The data pin (DIO), push-pull or open drain with a pull-up. Chips "
                                             "can share it."),
    Key("stb_pin", Pin(), required=True, doc="The strobe pin (STB), one per chip."),
    Key("ram_bytes", Int(1, TM_RAM_MAX), default=14, doc="The display RAM bytes it writes: what the chip has "
                                                         "(TM1628 14, TM1638 16) or what the board uses."),
    Key("intensity", Int(0, 7), default=7, doc="The brightness, the chip's PWM, for every LED on it."),
    Key("grids", Int(4, 7), shown_default="not sent", doc="The grids it is set to at start-up (the display-mode "
                                                          "command). Left out, the chip keeps its own setting."),
    Key("fade", Bool(), default=False, doc="Its brightness follows the host's LED colours, up to `intensity`."),
)


class TM1628(LedDriver):
    """A TM1628-class LED driver, bit-banged from the main loop: pin = display RAM bit."""
    core_paths = True           # vpin.c handles it by number (components/hub HubType.core_paths)
    key, enum, num = "tm1628", "GM_HUB_TM1628", 7
    old_keys = ("tm1628",)
    spec = SPEC
    keys = tuple(SPEC.names)
    # TM1628 lines only pair with each other: they're bit-banged from the main loop
    lines = {"clk_pin": "tm_clk", "dio_pin": "tm_dio", "stb_pin": "tm_stb"}
    # its config (.tm) and state (.u.tm) are the core's: the fade (vpin.c vpin_fade*) reads them
    c_ops = "gm_tm1628_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.ram_bytes = SPEC.get(v, h, "ram_bytes", p)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = SPEC.get(v, h, "intensity", p)
        hub.grids = SPEC.get(v, h, "grids", p, default=0)   # 0: no display-mode command
        hub.fade = SPEC.get(v, h, "fade", p)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        hub.latch = ctl("stb_pin", "out")

    def range_text(self, hub):
        return f" ({hub.ram_bytes} RAM bytes x 8; pin = byte * 8 + bit)"

    def c_fields(self, hub):
        return [f"        .tm = {{ .clk = {hub.clock.c()}, .dio = {hub.data.c()}, .stb = {hub.latch.c()},",
                f"                .ram_bytes = {hub.ram_bytes}, .brightness = {hub.intensity}, "
                f".grids = {hub.grids}, .fade = {1 if hub.fade else 0} }},"]

    def final_validate(self, v, dev, hubs):
        check_own_latch(v, hubs, "stb_pin", "chips", "clk_pin and dio_pin")


HUB_TYPES = (TM1628(),)
