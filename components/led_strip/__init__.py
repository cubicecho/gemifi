"""led_strip: the base component of LED strips, chains of LEDs that show any colour, ESPHome style:
`led_strip: [{ id: strip, platform: ws2812, ... }]`, pins `{ led_strip: strip, number: n }`. Each
platform is a hub type in a component of its own (components/ws2812, maudio_pads) on LedStrip, with
what they share: a byte per colour in the hub RAM, gamma: and white:. The C side is led_strip.h
(the colour output); README.md.
"""
from components.hub import HubType, c_comment


class LedStrip(HubType):
    """A strip platform: LEDs of colour_pins(hub) pins each, a byte per colour in the hub RAM (pin = LED
    * colour_pins + colour), calibrated by gamma: (the C table c_tables writes) and white:."""
    domain = "led_strip"
    roles = frozenset({"out"})
    ram = True
    full_colour = True

    def moved_text(self):
        return (f"moved: an LED strip is `led_strip: [{{ id: strip, platform: {self.key}, ... }}]` now, ESPHome "
                "style, and its pins `{ led_strip: strip, number: n }`")

    def c_tables(self, hub):
        if hub.gamma == 1:
            return []
        t = rgb_curve(hub.gamma)
        return [f"/* {c_comment(hub.id)}: gamma {hub.gamma:g} ({self.domain} gamma:) */",
                f"static const uint8_t rgb_curve_{hub.index + 1}[256] = {{",
                *[f"    {', '.join(str(x) for x in t[k:k + 16])}," for k in range(0, 256, 16)], "};"]

    def c_curve(self, hub):
        """The entry's .curve: its gamma table, or 0 (straight)."""
        return f"rgb_curve_{hub.index + 1}" if hub.gamma != 1 else "0"


def validate_rgb_calibration(v, h, p, white_n=3):
    """A strip's gamma: (1.0..3.0, default 1 = straight) and white: [r, g, b] (or [r, g, b, w] with white_n
    4; 1..255 each, default 255s)."""
    gamma = 1.0
    if h.get("gamma") is not None:
        g = h["gamma"]
        if isinstance(g, bool) or not isinstance(g, (int, float)) or not 1.0 <= float(g) <= 3.0:
            v.err(f"{p}.gamma", h.line_of("gamma"), f"must be a number from 1.0 to 3.0 (1 = straight; LEDs "
                                                    f"usually look right at about 2.2), not {g!r}")
        else:
            gamma = float(g)
    white = [255] * white_n
    if h.get("white") is not None:
        w = h["white"]
        if (not isinstance(w, list) or len(w) != white_n or
                any(isinstance(x, bool) or not isinstance(x, int) or not 1 <= x <= 255 for x in w)):
            what = "[red, green, blue]" if white_n == 3 else "[red, green, blue, white]"
            v.err(f"{p}.white", h.line_of("white"), f"must be {what}, each 1..255: what full white "
                                                    "sends, e.g. [255, 200, 220] to take some green and blue out")
        else:
            white = list(w)
    return gamma, white


def rgb_curve(gamma):
    """256 entries: value v (as seen) to the LED's PWM, 255 * (v / 255) ^ gamma; above 0 stays above 0."""
    return [0] + [max(1, round(255 * (k / 255) ** gamma)) for k in range(1, 256)]
