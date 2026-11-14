"""maudio_pad_leds: the M-Audio Code pad LED board (RGB LEDs behind a serial link), a hub type on the hub
base (components/hub). The YAML and the C: README.md here.
"""
import gemifi_device as gd  # the builder, which loads the components: used at call time only
from components.hub import HubType, c_comment, rgb_curve, validate_rgb_calibration

# The M-Audio Code pad LED board (maudio_pad_leds, formerly rgb_serial): RGB LEDs behind a serial
# link, its own protocol (maudio_pad_leds.c). pin = LED * 3 + colour (0 red, 1 green, 2 blue). Kept in
# the hub RAM, sent from the main loop.
MAUDIO_PADS_MAX = 32


class MaudioPadLeds(HubType):
    """The M-Audio Code pad LED board: RGB LEDs behind a serial link, pin = LED * 3 + colour."""
    key, enum, num = "maudio_pad_leds", "GM_HUB_MAUDIO_PADS", 9
    aliases = ("rgb_serial",)
    keys = ("id", "tx_pin", "rx_pin", "count", "dma", "gamma", "white")
    lines = {"tx_pin": "uart_tx", "rx_pin": "uart_rx"}
    roles = frozenset({"out"})
    ram = True
    full_colour = True
    c_header, c_ops = "maudio_pad_leds.h", "gm_maudio_pad_leds_ops"
    c_config, c_state = ("gm_hub_maudio_pads_t", "mpads"), ("gm_maudio_pad_leds_state_t", "mpads")

    def validate(self, v, dev, hub, h, p, line, ctl):
        if any(x.type == self.key for x in dev.hubs if x is not hub):
            # one LED board: its UART is fixed (USART2 on stm32f1)
            v.err(p, line, f"a second {self.key} hub: the firmware drives one (one LED board)")
        hub.chain = v.integer(h, "count", p, 1, MAUDIO_PADS_MAX, default=16)
        hub.n_pins = 3 * hub.chain
        hub.ram_bytes = hub.n_pins                       # a byte per colour
        hub.gamma, hub.white = validate_rgb_calibration(v, h, p)
        hub.data = ctl("tx_pin", "out")
        hub.latch = ctl("rx_pin", "in", required=False)     # optional: the board's answers (acks)
        hub.dma = v.boolean(h, "dma", p, default=True)
        if hub.data.hub == 0 and h.get("tx_pin") is not None:
            why = v.plat.serial_tx(hub.data.num)
            if why:
                v.err(f"{p}.tx_pin", h.line_of("tx_pin"), why)
            elif h.get("rx_pin") is not None and hub.latch is not gd.NO_PIN:
                why = v.plat.serial_rx(hub.data.num, hub.latch.num)
                if why:
                    v.err(f"{p}.rx_pin", h.line_of("rx_pin"), why)

    def range_text(self, hub):
        return f" ({hub.chain} LEDs x 3; pin = LED * 3 + colour, 0 red 1 green 2 blue)"

    def c_tables(self, hub):
        if hub.gamma == 1:
            return []
        t = rgb_curve(hub.gamma)
        return [f"/* {c_comment(hub.id)}: gamma {hub.gamma:g} ({self.key} gamma:) */",
                f"static const uint8_t rgb_curve_{hub.index + 1}[256] = {{",
                *[f"    {', '.join(str(x) for x in t[k:k + 16])}," for k in range(0, 256, 16)], "};"]

    def c_fields(self, hub):
        return [f"        .mpads = {{ .tx = {hub.data.c()}, .rx = {hub.latch.c()}, .count = {hub.chain}, "
                f".dma = {1 if hub.dma else 0},",
                f"                 .white = {{ {', '.join(str(x) for x in hub.white)} }}, "
                f".curve = {f'rgb_curve_{hub.index + 1}' if hub.gamma != 1 else '0'} }},"]


HUB_TYPES = (MaudioPadLeds(),)
