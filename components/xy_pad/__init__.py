"""xy_pad: a 4-wire resistive XY pad read in software, a hub type on the hub base (components/hub). The
YAML and the C: README.md here.
"""
import gemifi_device as gd  # the builder, which loads the components: used at call time only
from components.hub import HubType

# A 4-wire resistive XY pad: two analog channels, 0 = x, 1 = y (xy_pad.c reads it in software).
XY_TOUCH = (570, 3430)          # stock Code 61: 0x250 - 22 .. 0xD50 + 22


class XYPad(HubType):
    key, enum, num = "xy_pad", "GM_HUB_XY", 10
    keys = ("id", "x_plus_pin", "x_minus_pin", "y_plus_pin", "y_minus_pin", "touch_min", "touch_max")
    lines = {"x_plus_pin": "xy_xp", "x_minus_pin": "xy_xm", "y_plus_pin": "xy_yp", "y_minus_pin": "xy_ym"}
    roles = frozenset({"analog"})
    c_header, c_config, c_ops = "xy_pad.h", ("gm_hub_xy_t", "xy"), "gm_xy_pad_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        validate_xy_pad(v, hub, h, p, ctl)

    def pin_hint(self, hub):
        return "x or y"

    def c_fields(self, hub):
        drive = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        return [f"        .xy = {{ .drive = {{ {', '.join(drive)} }},",
                f"                .sense = {{ {hub.data.c()}, {hub.clock.c()} }}, "
                f".touch_min = {hub.touch_min}, .touch_max = {hub.touch_max} }},"]


def validate_xy_pad(v, hub, h, p, ctl):
    """xy_pad: a 4-wire resistive panel on four MCU pins. Reading X drives x_plus high and x_minus low,
    and reads the Y plate; reading Y the other way round. Each axis is read on the other plate's minus
    pin if it has an ADC, else its plus pin."""
    hub.n_pins = 2
    hub.sel = [ctl(k, "out") for k in ("x_plus_pin", "x_minus_pin", "y_plus_pin", "y_minus_pin")]
    hub.touch_min = v.integer(h, "touch_min", p, 0, 4095, default=XY_TOUCH[0])
    hub.touch_max = v.integer(h, "touch_max", p, 0, 4095, default=XY_TOUCH[1])
    if hub.touch_min >= hub.touch_max:
        v.err(p, h.line, f"touch_min {hub.touch_min} must be below touch_max {hub.touch_max}")

    def sense(minus, plus, axis, keys):
        for pn in (minus, plus):
            if pn.hub == 0 and pn.num >= 0 and v.plat.adc(pn.num)[0]:
                return pn
        if all(pn.hub == 0 for pn in (minus, plus)) and all(h.get(k) is not None for k in keys):
            v.err(p, h.line, f"{axis} is read on {keys[0]} or {keys[1]}: neither has an ADC "
                             f"({gd.adc_text(v.plat)})")
        return gd.NO_PIN

    xp, xm, yp, ym = hub.sel
    hub.data = sense(ym, yp, "X", ("y_minus_pin", "y_plus_pin"))
    hub.clock = sense(xm, xp, "Y", ("x_minus_pin", "x_plus_pin"))


HUB_TYPES = (XYPad(),)
