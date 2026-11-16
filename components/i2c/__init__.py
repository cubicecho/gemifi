"""i2c: the base component of the I2C buses, ESPHome style. `i2c:` is one bus (`{ sda, scl }`) or a
list of them with ids; a component on I2C (an mcp23017, a pcf8574, an ht16k33, a display: ssd1306 ...)
names its bus with `i2c_id:`, which it may leave out when there is only one. No C here: the platform
drives the buses (gm_hal.h hal_i2c_*, a bus index first). README.md here.
"""
from types import SimpleNamespace

from components.keys import Hex, Id, IdRef, Key, Keys, Pin, Section

# A bus's keys: `i2c:` is one of them, or a list (then each needs its id).
BUS = Keys(
    Key("id", Id(), doc="What `i2c_id:` names. Required when `i2c:` is a list, optional on a single bus."),
    Key("sda", Pin(), required=True, doc="The data pin (SDA), an MCU pin."),
    Key("scl", Pin(), required=True, doc="The clock pin (SCL), an MCU pin."),
)
BUS_KEYS = BUS.names
KEY_SECTIONS = (Section("i2c", BUS, is_list=False),)

# The keys every component on a bus has (an I2C hub's, display's ...).
I2C_ID = Key("i2c_id", IdRef("I2C bus"), see="components/i2c/README.md",
             doc="The id of the `i2c:` bus it is on. It may be left out when the device has one bus.")


def address(default, doc, hi=0x77):
    """An I2C component's address: key, 7-bit (0x08..0x77, or up to hi), with the chip's usual default."""
    return Key("address", Hex(0x08, hi), default=default, shown_default=f"`0x{default:02X}`", doc=doc)


def validate_buses(v, dev, data, on_i2c, max_buses):
    """dev.i2c_buses from data's i2c:, and each item of on_i2c (the hubs and screens on I2C, each with .raw,
    its YAML mapping) its .bus, from its i2c_id:. max_buses: the platform's I2C controllers."""
    dev.i2c_buses = []
    if "i2c" in data:
        raw, line = data["i2c"], data.line_of("i2c")
        many = isinstance(raw, list)
        items = v.seq(raw, "i2c", line) if many else [raw]
        for i, b in enumerate(items):
            p = f"i2c[{i}]" if many else "i2c"
            b = v.mapping(b, p, items.line_of(i) if many else line, BUS_KEYS)
            if b is None:
                continue
            bid = v.ident(b, p, "i2c bus", i, allow_name=False) if "id" in b or many else None
            if many and bid is None:
                continue
            sda = v.pin(b.get("sda"), f"{p}.sda", b.line_of("sda"), "out", allow_hub=False)
            scl = v.pin(b.get("scl"), f"{p}.scl", b.line_of("scl"), "out", allow_hub=False)
            if sda and scl:
                dev.i2c_buses.append(SimpleNamespace(id=bid, index=len(dev.i2c_buses), sda=sda, scl=scl, path=p,
                                                     line=b.line))
        if on_i2c and len(dev.i2c_buses) > max_buses:
            v.err("i2c", line, f"{len(dev.i2c_buses)} I2C buses: {dev.platform.name} has {max_buses}")
        if not on_i2c:
            v.warn("i2c", line, "nothing uses it (no I2C hub: mcp23017 / pcf8574 / ht16k33, or display: ssd1306)")
    elif on_i2c:
        first = on_i2c[0]
        v.err("i2c", data.line, f"{_what(dev, first)} '{first.id}' ({first.type}) needs the I2C pins: "
                                "add i2c: { sda: ..., scl: ... }")
        return
    by_id = {b.id: b for b in dev.i2c_buses if b.id is not None}
    for x in on_i2c:
        x.bus = 0
        bid = x.raw.get("i2c_id") if x.raw is not None else None
        if bid is None:
            if len(dev.i2c_buses) > 1:
                v.err(x.path, x.line, f"{_what(dev, x)} '{x.id}': give i2c_id: (the device has "
                                      f"{len(dev.i2c_buses)} I2C buses: {', '.join(sorted(by_id))})")
            continue
        bus = by_id.get(bid) if isinstance(bid, str) else None
        if bus is None:
            known = ", ".join(sorted(by_id)) or "none: give the i2c: bus an id"
            v.err(f"{x.path}.i2c_id", x.raw.line_of("i2c_id"), f"no I2C bus with id '{bid}' (i2c: {known})")
            continue
        x.bus = bus.index
    addrs = {}
    for x in on_i2c:
        key = (x.bus, x.addr)
        if key in addrs:
            on = f" on bus '{dev.i2c_buses[x.bus].id}'" if len(dev.i2c_buses) > 1 else ""
            v.err(f"{x.path}.address", x.line, f"I2C address 0x{x.addr:02X} is already used by {addrs[key]}{on}")
        addrs[key] = f"{_what(dev, x)} '{x.id}'"


def _what(dev, x):
    return "display" if x in dev.screens else "hub"


def c_defines(dev):
    """gm_device_config.h: the buses' pins, GPIO numbers, for the platform (hal_i2c_*: bus = index)."""
    from components.hub import c_comment      # here: the hub base imports this module's keys
    if not dev.i2c_buses:
        return []
    pins = ", ".join(f"{{ {b.sda.num}, {b.scl.num} }}" for b in dev.i2c_buses)
    names = c_comment(", ".join(f"{b.id or 'i2c'}: {b.sda.desc} / {b.scl.desc}" for b in dev.i2c_buses))
    return [f"#define GM_I2C_BUSES {len(dev.i2c_buses)} /* i2c: (components/i2c) */",
            f"#define GM_I2C_PINS {{ {pins} }} /* {{ sda, scl }} each: {names} */"]
