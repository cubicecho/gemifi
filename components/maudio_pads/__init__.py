"""maudio_pads: the M-Audio Code pad LED board (RGB LEDs behind a serial link), a platform on the
led_strip base (components/led_strip): `led_strip: [{ id: pad_leds, platform: maudio_pads, tx_pin: PA2 }]`.
The YAML and the C: README.md here.
"""
from components.api import NO_PIN
from components.keys import Bool, Enum, Id, Int, Key, Keys, Pin
from components.led_strip import GAMMA, WHITE, LedStrip, validate_rgb_calibration

DEPENDENCIES = ("led_strip",)

# The M-Audio Code pad LED board: RGB LEDs behind a serial link, its own protocol (maudio_pads.c).
# pin = LED * 3 + colour (0 red, 1 green, 2 blue). Kept in the hub RAM, sent from the main loop.
MAUDIO_PADS_MAX = 32

SPEC = Keys(
    Key("id", Id(), required=True, doc="What its pins name: `{ led_strip: <id>, number: n }`."),
    Key("platform", Enum(["maudio_pads"]), required=True, doc="This board."),
    Key("tx_pin", Pin(), required=True, doc="The UART TX pin to the board (PA2 on the Code 61)."),
    Key("rx_pin", Pin(), doc="The UART RX pin, to hear the board's answers. Left out, the LEDs are sent blind."),
    Key("count", Int(1, MAUDIO_PADS_MAX), default=16, doc="The LEDs on the board."),
    Key("dma", Bool(), default=True, see='core/README.md "DMA channels (stm32f1)"',
        doc="Sends through a DMA channel (stm32f1); `false` sends from the interrupt."),
    GAMMA,
    WHITE,
)


class MaudioPads(LedStrip):
    """The M-Audio Code pad LED board: RGB LEDs behind a serial link, pin = LED * 3 + colour."""
    key, enum, num = "maudio_pads", "GM_HUB_MAUDIO_PADS", 9
    old_keys = ("maudio_pad_leds", "rgb_serial")
    spec = SPEC
    keys = tuple(SPEC.names)
    lines = {"tx_pin": "uart_tx", "rx_pin": "uart_rx"}
    c_header, c_ops = "maudio_pads.h", "gm_maudio_pads_ops"
    c_config, c_state = ("gm_hub_maudio_pads_t", "mpads"), ("gm_maudio_pads_state_t", "mpads")

    def validate(self, v, dev, hub, h, p, line, ctl):
        if any(x.type == self.key for x in dev.hubs if x is not hub):
            # one LED board: its UART is fixed (USART2 on stm32f1)
            v.err(p, line, f"a second {self.key} hub: the firmware drives one (one LED board)")
        hub.chain = SPEC.get(v, h, "count", p)
        hub.n_pins = 3 * hub.chain
        hub.ram_bytes = hub.n_pins                       # a byte per colour
        hub.gamma, hub.white = validate_rgb_calibration(v, h, p)
        hub.data = ctl("tx_pin", "out")
        hub.latch = ctl("rx_pin", "in", required=False)     # optional: the board's answers (acks)
        hub.dma = SPEC.get(v, h, "dma", p)
        if hub.data.hub == 0 and h.get("tx_pin") is not None:
            why = v.plat.serial_tx(hub.data.num)
            if why:
                v.err(f"{p}.tx_pin", h.line_of("tx_pin"), why)
            elif h.get("rx_pin") is not None and hub.latch is not NO_PIN:
                why = v.plat.serial_rx(hub.data.num, hub.latch.num)
                if why:
                    v.err(f"{p}.rx_pin", h.line_of("rx_pin"), why)

    def range_text(self, hub):
        return f" ({hub.chain} LEDs x 3; pin = LED * 3 + colour, 0 red 1 green 2 blue)"

    def c_fields(self, hub):
        return [f"        .mpads = {{ .tx = {hub.data.c()}, .rx = {hub.latch.c()}, .count = {hub.chain}, "
                f".dma = {1 if hub.dma else 0},",
                f"                 .white = {{ {', '.join(str(x) for x in hub.white)} }}, "
                f".curve = {self.c_curve(hub)} }},"]


HUB_TYPES = (MaudioPads(),)
