"""cd74hc4067: the CD74HC4051 / 4067 multiplexers (cd74hc4051: 3 select lines, 8 channels; cd74hc4067: 4,
16), two YAML keys of one hub type (GM_HUB_MUX) on the hub base (components/hub). The YAML and the C:
README.md here.
"""
from components.api import NO_PIN
from components.hub import HubType
from components.keys import Id, Key, Keys, Period, Pin


class Mux(HubType):
    """CD74HC4051 (3 select lines, 8 channels) / 4067 (4, 16): one common pin routed to a channel;
    analog or digital inputs."""
    enum, num = "GM_HUB_MUX", 3
    lines = {"common_pin": "common", "enable_pin": "enable",
             "pin_s0": "s0", "pin_s1": "s1", "pin_s2": "s2", "pin_s3": "s3"}
    roles = frozenset({"in", "analog"})
    what = "channel"
    # its config (.mux) is the core's: vpin_read, gm_hub_mux_select and the hardware analog scan read it
    c_ops = "gm_cd74hc4067_ops"

    def __init__(self, key, n_sel):
        self.key, self.n_sel = key, n_sel
        self.spec = Keys(
            Key("id", Id(), required=True, doc=f"What its channels name: `{{ {key}: <id>, number: n }}`."),
            *(Key(f"pin_s{i}", Pin(), required=True, doc=f"Select line S{i}: bit {i} of the channel.")
              for i in range(n_sel)),
            Key("common_pin", Pin(), required=True, doc="The common pin (SIG): on an ADC pin the channels read "
                                                        "analog, on another MCU pin digital."),
            Key("enable_pin", Pin(), doc="The enable pin (/E), driven low. Leave it out when /E is tied low."),
            Key("delay", Period(0, 0xFFFF, example="5us"), shown_default="`2us`", see="Shared select lines",
                doc="How long it waits after switching channel before a read (`0us` is the default too)."))
        self.keys = tuple(self.spec.names)

    def validate(self, v, dev, hub, h, p, line, ctl):
        hub.n_sel = self.n_sel
        hub.sel = [ctl(f"pin_s{i}", "out") for i in range(hub.n_sel)]
        hub.n_pins = 1 << hub.n_sel
        hub.common = ctl("common_pin", "common")
        if hub.common.hub == 0:
            hub.common_adc = v.plat.adc(hub.common.num)[0]
        else:
            hub.common = None
        hub.enable = ctl("enable_pin", "out", required=False)
        hub.settle_us = self.spec.get(v, h, "delay", p, default=0)   # 0: vpin.c waits 2us
        if "delay" in h and hub.settle_us == 0:
            v.warn(f"{p}.delay", h.line_of("delay"), "0us means the default, 2us (vpin.c)")

    def c_fields(self, hub):
        sel = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        return [f"        .mux = {{ .sel = {{ {', '.join(sel)} }},",
                f"                 .common = {(hub.common or NO_PIN).c()}, .enable = {hub.enable.c()}, "
                f".settle_us = {hub.settle_us}, .n_sel = {hub.n_sel} }},"]


HUB_TYPES = (Mux("cd74hc4051", 3), Mux("cd74hc4067", 4))
