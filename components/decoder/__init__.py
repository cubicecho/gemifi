"""decoder: 74HC138-style one-of-N decoders driving matrix rows, a hub type on the hub base
(components/hub). The YAML and the C: README.md here.
"""
from components.api import NO_PIN
from components.hub import HubType
from components.keys import Id, Key, Keys, List, Pin, T, Type, one_of


class Levels(Type):
    """A group: enable pin -> its level, 0 or 1."""
    text = "mapping of pin to 0 or 1"

    def schema(self):
        return {"type": "object", "additionalProperties": one_of(T("integer", minimum=0, maximum=1), T("boolean"))}


SPEC = Keys(
    Key("id", Id(), required=True, doc="What its outputs name: `rows: { decoder: <id>, first: n, count: n }`, or "
                                       "a pin `{ decoder: <id>, number: n }`."),
    Key("address", List(Pin()), required=True, doc="The address pins (1..4), A0 first, shared by its chips."),
    Key("groups", List(Levels()), shown_default="one chip, no enable pins",
        doc="Its chips (1..4): each the levels of the enable pins that select it, e.g. `{ PB6: 1, PB7: 0 }`. "
            "At most 3 enable pins, the same ones in each."),
)


class Decoder(HubType):
    """A 74HC138-style one-of-N decoder: outputs only, and only as matrix rows."""
    key, enum, num = "decoder", "GM_HUB_DECODER", 6
    spec = SPEC
    keys = tuple(SPEC.names)
    roles = frozenset({"row"})
    # its config (.dec) is the core's: vpin_write selects an output itself (the scan's path)
    c_ops = "gm_decoder_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        validate_decoder(v, hub, h, p, hub.index)

    def c_fields(self, hub):
        addr = [x.c() for x in hub.sel] + ["GM_NO_PIN"] * (4 - len(hub.sel))
        en = [x.c() for x in hub.en] + ["GM_NO_PIN"] * (3 - len(hub.en))
        lv = [str(x) for x in hub.group_lv] + ["0"] * (4 - len(hub.group_lv))
        return [f"        .dec = {{ .addr = {{ {', '.join(addr)} }},",
                f"                 .en = {{ {', '.join(en)} }},",
                f"                 .n_addr = {hub.n_sel}, .n_en = {len(hub.en)}, .n_groups = {len(hub.group_lv)}, "
                f".group_lv = {{ {', '.join(lv)} }} }},"]


def validate_decoder(v, hub, h, p, index):
    """decoder: { address: [A0, A1, A2], groups: [{ PB6: 1, PB7: 0 }, ...] }. Output n = address
    n % 2^len(address) in group n // 2^len(address); a group is the levels of the enable pins."""
    addr = h.get("address")
    if not isinstance(addr, list) or not 1 <= len(addr) <= 4:
        v.err(f"{p}.address", h.line_of("address") if "address" in h else h.line,
              "needs a list of 1-4 address pins, A0 first")
        addr = []
    hub.sel = [v.pin(x, f"{p}.address[{i}]", addr.line_of(i), "out", allow_hub=False, line_kind="address",
                     owner=index) or NO_PIN for i, x in enumerate(addr)]
    hub.n_sel = len(hub.sel)
    groups = h.get("groups")
    if groups is None:
        hub.group_lv = [0]
    else:
        groups = v.seq(groups, f"{p}.groups", h.line_of("groups"))
        if not 1 <= len(groups) <= 4:
            v.err(f"{p}.groups", h.line_of("groups"), f"{len(groups)} groups: 1-4 (each a mapping of "
                                                      "enable pin to level, e.g. { PB6: 1, PB7: 0 })")
        names = None
        seen = {}
        for g, grp in enumerate(groups[:4]):
            gp = f"{p}.groups[{g}]"
            if not isinstance(grp, dict) or not grp:
                v.err(gp, groups.line_of(g), "must be a mapping of enable pin to level, e.g. { PB6: 1, PB7: 0 }")
                continue
            nums = {}
            for k in grp:
                try:
                    nums[v.plat.parse_pin(k)] = k
                except ValueError as e:
                    v.err(f"{gp}.{k}", grp.line_of(k), str(e))
            if names is None:
                if len(nums) > 3:
                    v.err(gp, groups.line_of(g), f"{len(nums)} enable pins: at most 3")
                names = list(nums)[:3]
                hub.en = [v.pin(nums[n], f"{gp}.{nums[n]}", grp.line_of(nums[n]), "out", allow_hub=False,
                                line_kind="enable", owner=index) or NO_PIN for n in names]
            elif set(nums) != set(names):
                v.err(gp, groups.line_of(g), "every group must give a level for the same enable pins "
                                             f"({', '.join(v.plat.pin_name(n) for n in names)})")
                continue
            lv = 0
            for e, n in enumerate(names):
                x = grp[nums[n]]
                if isinstance(x, bool):
                    x = int(x)
                if x not in (0, 1):
                    v.err(f"{gp}.{nums[n]}", grp.line_of(nums[n]), f"level must be 0 or 1 (got {x!r})")
                    x = 0
                lv |= x << e
            if lv in seen:
                v.err(gp, groups.line_of(g), f"same enable levels as groups[{seen[lv]}]: the two can't be told apart")
            seen.setdefault(lv, g)
            hub.group_lv.append(lv)
        if not hub.group_lv:
            hub.group_lv = [0]
    hub.n_pins = len(hub.group_lv) << hub.n_sel


HUB_TYPES = (Decoder(),)
