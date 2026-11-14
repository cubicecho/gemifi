"""ws2812: addressable LED strips (WS2812B, SK6812 RGB / RGBW) on one MCU pin, a hub type on the hub
base (components/hub). The YAML, the C and the platforms: README.md here.
"""
import difflib
import itertools

from components.hub import HubType, c_comment, rgb_curve, validate_rgb_calibration

# Addressable LED chains (WS2812B, SK6812 RGB / RGBW) on one MCU pin: pin = LED * channels + colour (0 red,
# 1 green, 2 blue, 3 white). A byte per colour in the hub RAM; the platform sends frames (hal_ledstrip_*).
WS2812_MAX = 256
WS2812_ORDERS = sorted({"".join(p) for p in itertools.permutations("RGB")}) + \
    sorted({"".join(p) for p in itertools.permutations("RGBW")})


class WS2812(HubType):
    """An addressable LED strip (WS2812B / SK6812, RGB or RGBW): pin = LED * 3 (4) + colour."""
    key, enum, num = "ws2812", "GM_HUB_WS2812", 11
    keys = ("id", "pin", "count", "order", "gamma", "white")
    lines = {"pin": "ledstrip"}
    roles = frozenset({"out"})
    ram = True
    full_colour = True
    c_header, c_config, c_state, c_ops = "ws2812.h", ("gm_hub_ws2812_t", "ws"), ("gm_ws2812_state_t", "ws"), "gm_ws2812_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.chain = v.integer(h, "count", p, 1, WS2812_MAX, required=True) or 1
        order = h.get("order", "GRB")
        order = order.strip().upper() if isinstance(order, str) else order
        if order not in WS2812_ORDERS:
            close = difflib.get_close_matches(str(order).upper(), WS2812_ORDERS, 1)
            hint = f"did you mean {close[0]}? " if close else ""
            v.err(f"{p}.order", h.line_of("order"), f"unknown order '{h.raw_of('order')}' ({hint}the colours as the "
                                                    "LED takes them: GRB (WS2812B, the default), RGB, BRG, ...; "
                                                    "GRBW / RGBW for RGBW LEDs, e.g. SK6812 RGBW)")
            order = "GRB"
        hub.order = order
        hub.channels = len(order)
        hub.n_pins = hub.channels * hub.chain
        hub.ram_bytes = hub.n_pins                       # a byte per colour
        hub.gamma, hub.white = validate_rgb_calibration(v, h, p, white_n=hub.channels)
        hub.data = ctl("pin", "out")
        if hub.data.hub == 0 and h.get("pin") is not None:
            why = v.plat.ledstrip(hub.data.num, [x.data.num for x in dev.hubs if x.type == self.key and
                                                 x is not hub and x.data.hub == 0])
            if why:
                v.err(f"{p}.pin", h.line_of("pin"), why)

    def colour_pins(self, hub):
        return hub.channels

    def range_text(self, hub):
        names = " ".join(f"{k} {c}" for k, c in enumerate(("red", "green", "blue", "white")[:hub.channels]))
        return f" ({hub.chain} LEDs x {hub.channels}; pin = LED * {hub.channels} + colour, {names})"

    def wire_order(self, hub):
        """gm_hub_ws2812_t.order: byte k of an LED is colour (order >> 2k) & 3."""
        return sum("RGBW".index(ch) << (2 * k) for k, ch in enumerate(hub.order))

    def c_tables(self, hub):
        if hub.gamma == 1:
            return []
        t = rgb_curve(hub.gamma)
        return [f"/* {c_comment(hub.id)}: gamma {hub.gamma:g} ({self.key} gamma:) */",
                f"static const uint8_t rgb_curve_{hub.index + 1}[256] = {{",
                *[f"    {', '.join(str(x) for x in t[k:k + 16])}," for k in range(0, 256, 16)], "};"]

    def c_fields(self, hub):
        white = list(hub.white) + [255] * (4 - len(hub.white))
        return [f"        .ws = {{ .pin = {hub.data.c()}, .count = {hub.chain}, .channels = {hub.channels}, "
                f".order = 0x{self.wire_order(hub):02X} /* {hub.order} */,",
                f"                .white = {{ {', '.join(str(x) for x in white)} }}, "
                f".curve = {f'rgb_curve_{hub.index + 1}' if hub.gamma != 1 else '0'} }},"]


HUB_TYPES = (WS2812(),)
