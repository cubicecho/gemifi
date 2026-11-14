"""tm1628: TM1628-class LED drivers (TM1628, TM1638, TM1620 ...), bit-banged from the main loop, a hub
type on the hub base (components/hub). The YAML and the C: README.md here.
"""
from components.hub import HubType

# TM1628-class LED drivers (TM1628, TM1638, TM1620 ...): pin = display RAM bit, byte * 8 + bit.
TM_RAM_MAX = 16                 # TM1638 has 16 bytes, TM1628 14


class TM1628(HubType):
    """A TM1628-class LED driver, bit-banged from the main loop: pin = display RAM bit."""
    key, enum, num = "tm1628", "GM_HUB_TM1628", 7
    keys = ("id", "clk_pin", "dio_pin", "stb_pin", "ram_bytes", "intensity", "grids", "fade")
    # TM1628 lines only pair with each other: they're bit-banged from the main loop
    lines = {"clk_pin": "tm_clk", "dio_pin": "tm_dio", "stb_pin": "tm_stb"}
    roles = frozenset({"out"})
    ram = True
    # its config (.tm) and state (.u.tm) are the core's: the fade (vpin.c vpin_fade*) reads them
    c_ops = "gm_tm1628_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.ram_bytes = v.integer(h, "ram_bytes", p, 1, TM_RAM_MAX, default=14)
        hub.n_pins = 8 * hub.ram_bytes
        hub.intensity = v.integer(h, "intensity", p, 0, 7, default=7)
        hub.grids = v.integer(h, "grids", p, 4, 7, default=0)   # 0: no display-mode command
        hub.fade = v.boolean(h, "fade", p, default=False)
        hub.clock = ctl("clk_pin", "out")
        hub.data = ctl("dio_pin", "out", open_drain=True)
        hub.latch = ctl("stb_pin", "out")

    def range_text(self, hub):
        return f" ({hub.ram_bytes} RAM bytes x 8; pin = byte * 8 + bit)"

    def c_fields(self, hub):
        return [f"        .tm = {{ .clk = {hub.clock.c()}, .dio = {hub.data.c()}, .stb = {hub.latch.c()},",
                f"                .ram_bytes = {hub.ram_bytes}, .brightness = {hub.intensity}, "
                f".grids = {hub.grids}, .fade = {1 if hub.fade else 0} }},"]


HUB_TYPES = (TM1628(),)
