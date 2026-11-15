"""leds: the LEDs (`leds:`, `led_defaults:`), a control: leds.c draws them, this file validates their YAML (pins,
looks, their owner, timeout and feedback source) and writes their C (README.md here). The builder
(gemifi_device.py) calls validate() before the buttons (they name their LEDs), and c_tables() where
gm_device_config.c has it; the modes and host feedback use the looks and the owners from here.
"""
import difflib
import re

import components
from components.api import NO_PIN, RENAMED_ID, Hub, f1_off_why, f1_on, twin_c
from components.hub import c_comment

LED_DEFAULTS_KEYS = ["idle", "active"]
LED_KEYS = ["id", "name", "pin", "pin2", "pin3", "idle", "active", "feedback", "owner", "timeout", "daw"]
# LED owners (gm_device.h GM_OWNER_*): who drives an LED that both the host and the device want.
LED_OWNERS = {"host_then_local": 0, "host": 1, "local": 2}
LED_SOURCES = {"note": 1, "cc": 2, "clock": 3, "transport": 4, "mcu": 5}   # gm_device.h GM_FB_*
LED_SOURCE_VALUES = {"clock": ["beat"], "transport": ["playing"]}
TIMEOUT_NEVER = 0xFFFF            # gm_device.h GM_TIMEOUT_NEVER (10 ms units otherwise)

# LED looks (idle: / active:): which of the LED's pins are lit, bit 0 = pin, 1 = pin2, 2 = pin3. Names per
# kind of LED; "on" is the LED's default active look on every kind.
LED_LOOKS = {
    "single": {"off": 0, "on": 1},
    "bicolour": {"off": 0, "on": 1, "colour1": 1, "colour2": 2, "both": 3},
    "rgb": {"off": 0, "on": 7, "red": 1, "green": 2, "yellow": 3, "blue": 4, "magenta": 5, "cyan": 6,
            "white": 7},
}
LED_LOOK_NAMES = {n: m for k in ("rgb", "bicolour", "single") for n, m in LED_LOOKS[k].items() if n != "on"}
LED_DEFAULT_LOOK = {"single": (0, 1), "bicolour": (0, 1), "rgb": (0, 7)}   # (idle, active)
LED_KIND_PINS = {"single": 1, "bicolour": 3, "rgb": 7}    # the pins each kind of LED has, as look bits
# Full-colour looks, for LEDs that can show any colour (one LED of a full-colour hub, full_colour_hubs()):
# "#rrggbb", or an RGB look name with a brightness, "blue 25%". Elsewhere (led_defaults, a cycle step's
# colour) they show as the nearest on/off look.
RGB_LOOK_COLOURS = {"red": 0xFF0000, "green": 0x00FF00, "yellow": 0xFFFF00, "blue": 0x0000FF,
                    "magenta": 0xFF00FF, "cyan": 0x00FFFF, "white": 0xFFFFFF}
RGB_LOOK_RE = re.compile(r"^#([0-9a-f]{6})$|^([a-z]+)\s+(\d{1,3})\s*%$")


# Always built: the status LED and the lamp test use it on any device.
REQUIRED = True


def validate(v, dev, data):
    """led_defaults: and leds: -> dev.leds (before the buttons: they name their LEDs)."""
    led_defaults = validate_led_defaults(v, data)
    leds = v.seq(data.get("leds"), "leds", data.line_of("leds"))
    for i, d in enumerate(leds):
        p = f"leds[{i}]"
        d = v.mapping(d, p, leds.line_of(i), LED_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "LED", len(dev.leds))
        name = v.display_name(d, p, ident)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "out")
        pin2 = v.pin(d["pin2"], f"{p}.pin2", d.line_of("pin2"), "out") if d.get("pin2") is not None else NO_PIN
        pin3 = v.pin(d["pin3"], f"{p}.pin3", d.line_of("pin3"), "out") if d.get("pin3") is not None else NO_PIN
        if d.get("pin3") is not None and d.get("pin2") is None:
            v.err(f"{p}.pin3", d.line_of("pin3"), "an RGB LED needs pin2 too (pin red, pin2 green, pin3 blue)")
        looks, rgb = validate_led_looks(v, d, p, led_kind(pin2 or NO_PIN, pin3 or NO_PIN), led_defaults,
                                        led_full_colour(dev, pin, pin2, pin3))
        src = validate_led_source(v, dev, d, p)
        owner = validate_owner(v, d, p)
        timeout = validate_timeout(v, d, "timeout", p, None)
        if timeout is not None and owner in (LED_OWNERS["host"], LED_OWNERS["local"]):
            v.warn(f"{p}.timeout", d.line_of("timeout"), "only used with owner: host_then_local")
        led_owner_line = d.line_of("owner") if owner == LED_OWNERS["local"] else None
        dev.leds.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, pin2=pin2 or NO_PIN,
                            pin3=pin3 or NO_PIN, looks=looks, rgb=rgb, source=src,
                            owner=owner, timeout=timeout, path=p, line=d.line, used_by=[],
                            daw=v.daw_role(dev, d, p), local_line=led_owner_line))


def colour_look(word):
    """'#rrggbb' or '<colour> <n>%' -> 0xRRGGBB, else None."""
    m = RGB_LOOK_RE.match(word)
    if not m:
        return None
    if m.group(1):
        return int(m.group(1), 16)
    base, pct = RGB_LOOK_COLOURS.get(m.group(2)), int(m.group(3))
    if base is None or pct > 100:
        return None
    return sum((((base >> sh) & 0xFF) * pct + 50) // 100 << sh for sh in (16, 8, 0))


def colour_mask(c):
    """The pins an on/off RGB LED lights for colour c: those at least half the brightest (as FB_LED_RGB)."""
    ch = [(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF]
    top = max(ch)
    return sum(1 << k for k in range(3) if top and 2 * ch[k] >= top)


def led_full_colour(dev, pin, pin2, pin3):
    """The LED's pins are one LED of a full-colour hub (its red, green and blue in a row, from LED *
    colour_pins): it can show any colour (controller.c led_full_colour, vpin_rgb)."""
    if pin in (None, NO_PIN) or pin.hub in (0, 0xFF) or pin.hub > len(dev.hubs):
        return False
    hub = dev.hubs[pin.hub - 1]
    return (hub.kind.full_colour and pin.num % hub.kind.colour_pins(hub) == 0 and
            pin2 is not None and pin2.hub == pin.hub and pin2.num == pin.num + 1 and
            pin3 is not None and pin3.hub == pin.hub and pin3.num == pin.num + 2)


def full_colour_hubs():
    """The hub types whose LEDs can show any colour (the LED strips), in GM_HUB_* order."""
    return [t.key for t in sorted(components.hub_types(), key=lambda t: (t.num, t.key)) if t.full_colour]


def colour_look_needs():
    """Said after a full-colour look an LED can't show."""
    return ("needs an LED that can show any colour: an RGB LED whose pin, pin2 and pin3 are one LED "
            f"of an LED strip (led_strip: {', '.join(full_colour_hubs())}; numbers LED * 3, + 1, + 2; "
            "LED * 4 on an RGBW strip)")


def colour_nearest(kind, c):
    """Full colour c on an on/off LED of this kind: the pins of its nearest look (as FB_LED_RGB: each channel
    lit if at least half the brightest), on a bicolour or single LED the pins of that it has, else its first
    colour; black is off."""
    want = colour_mask(c)
    if not want:
        return 0
    return (want & LED_KIND_PINS[kind]) or 1


def colour_exact(kind, c):
    """An on/off LED of this kind shows colour c as it is (each channel 0 or 255, and one it has)."""
    chans = [(c >> sh) & 0xFF for sh in (16, 8, 0)]
    return kind == "rgb" and all(x in (0, 255) for x in chans)


def led_kind(pin2, pin3):
    return "rgb" if pin3 is not NO_PIN else "bicolour" if pin2 is not NO_PIN else "single"


def look_word(d, key):
    """The look as written: `on` / `off` read as booleans unquoted, so take the raw text then."""
    x = d.get(key)
    return str(d.raw_of(key) if isinstance(x, bool) else x).strip().lower()


def validate_led_defaults(v, data):
    """led_defaults: { idle, active }: every LED's looks unless it sets its own. Any name of any kind of LED;
    each LED shows the nearest it can (resolve_led_default)."""
    out = {}
    if data.get("led_defaults") is None:
        return out
    d = v.mapping(data["led_defaults"], "led_defaults", data.line_of("led_defaults"), LED_DEFAULTS_KEYS)
    if d is None:
        return out
    for key in ("idle", "active"):
        if d.get(key) is None:
            continue
        w = look_word(d, key)
        if w != "on" and w not in LED_LOOK_NAMES and colour_look(w) is None:
            close = difflib.get_close_matches(w, ["on", *LED_LOOK_NAMES], 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"led_defaults.{key}", d.line_of(key), f"unknown look '{w}' ({hint}valid: on, "
                                                          f"{', '.join(LED_LOOK_NAMES)}, or a full colour: "
                                                          "\"#rrggbb\", blue 20%)")
            continue
        out[key] = w
    return out


def resolve_led_default(kind, word, full=False):
    """A led_defaults look (or a cycle step's colour) on an LED of this kind -> (pin mask, full colour or
    None): its own name if it has it, else the pins of that look it has, else (a colour it can't show) its
    first colour. A full colour ("#rrggbb", "blue 20%") shows as it is on an LED that can show any colour
    (full), elsewhere as the nearest on/off look (colour_nearest)."""
    col = colour_look(word) if word not in LED_LOOKS[kind] and word not in LED_LOOK_NAMES else None
    if col is not None:
        if full and kind == "rgb":
            return colour_mask(col), col
        return colour_nearest(kind, col), None
    if word in LED_LOOKS[kind]:
        return LED_LOOKS[kind][word], None
    want = LED_LOOK_NAMES[word]
    return (want & LED_KIND_PINS[kind]) or 1, None


def validate_led_looks(v, d, p, kind, defaults, full=False):
    """((idle, active) pin masks, or None for the defaults (generated as 0); (idle, active) full colours,
    None each for none). full: the LED can show any colour (led_full_colour)."""
    looks = list(LED_DEFAULT_LOOK[kind])
    rgb = [None, None]
    given = False
    set_active = False
    for k, key in enumerate(("idle", "active")):
        if d.get(key) is not None:
            w = look_word(d, key)
            col = colour_look(w) if kind == "rgb" and w not in LED_LOOKS[kind] else None
            if col is not None:
                if not full:
                    v.err(f"{p}.{key}", d.line_of(key), f"'{w}' {colour_look_needs()}; this one takes "
                                                          f"{', '.join(LED_LOOKS[kind])}")
                    continue
                looks[k], rgb[k] = colour_mask(col), col
                given = True
                set_active = set_active or k == 1
                continue
            if w not in LED_LOOKS[kind]:
                other = [x for x in ("rgb", "bicolour") if w in LED_LOOKS[x]]
                what = {"single": "a single-colour LED (pin only)", "bicolour": "a bicolour LED (pin, pin2)",
                        "rgb": "an RGB LED (pin, pin2, pin3)"}[kind]
                if other:
                    msg = f"'{w}' is a look for {'an RGB' if other[0] == 'rgb' else 'a bicolour'} LED; {what} takes"
                else:
                    close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
                    msg = (f"unknown look '{w}' (did you mean '{close[0]}'?);" if close else f"unknown look '{w}';") + \
                          f" {what} takes"
                v.err(f"{p}.{key}", d.line_of(key), f"{msg} {', '.join(LED_LOOKS[kind])}")
                continue
            looks[k] = LED_LOOKS[kind][w]
            given = True
            set_active = set_active or k == 1
        elif key in defaults:
            looks[k], rgb[k] = resolve_led_default(kind, defaults[key], full)
            given = True
    if not given:
        return None, None
    if looks[0] == looks[1] and rgb[0] == rgb[1]:
        if not set_active:
            looks[1] = 0            # lit when idle and no active: of its own: dark when active
        else:
            v.warn(p, d.line, "idle and active look the same: the LED never shows its state")
    return tuple(looks), (tuple(rgb) if any(c is not None for c in rgb) else None)


def validate_timeout(v, d, key, path, default):
    """An LED timeout: a time period (10 ms steps, 10ms..655s) or `never`. -> 10 ms units."""
    if key not in d or d[key] is None:
        return default
    if isinstance(d[key], str) and d[key].strip().lower() == "never":
        return TIMEOUT_NEVER
    t = v.period(d, key, path, 10_000, 655_340_000, step_us=10_000, example="2s")
    return default if t is None else t


def validate_owner(v, d, path):
    if d.get("owner") is None:
        return None
    o = d["owner"]
    if o not in LED_OWNERS:
        close = difflib.get_close_matches(str(o), list(LED_OWNERS), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(f"{path}.owner", d.line_of("owner"), f"unknown owner '{d.raw_of('owner')}' ({hint}valid: "
                                                   f"{', '.join(LED_OWNERS)})")
        return None
    return LED_OWNERS[o]


def validate_led_source(v, dev, d, p):
    """LED feedback: { note | cc | clock | transport | mcu, channel } -> (source, num, channel) or None."""
    if d.get("feedback") is None:
        return None
    fp, line = f"{p}.feedback", d.line_of("feedback")
    f = v.mapping(d["feedback"], fp, line, [*LED_SOURCES, "channel"])
    if f is None:
        return None
    kinds = [k for k in LED_SOURCES if k in f]
    if len(kinds) != 1:
        v.err(fp, line, ("give one of " if not kinds else f"only one of {', '.join(kinds)}; one of ") +
              "note: / cc: (a number), clock: beat, transport: playing, mcu: <MCU LED>")
        return None
    k = kinds[0]
    kl = f.line_of(k)
    ch = 0
    if "channel" in f:
        if k in ("note", "cc"):
            ch = v.integer(f, "channel", fp, 1, 16, default=0)
        else:
            v.err(f"{fp}.channel", f.line_of("channel"), f"only for note: / cc: (a {k} source has no channel)")
    if k == "mcu":
        if not v.mcu_needed(dev, f"{fp}.mcu", kl):
            return None
        notes = v.mcu_table("button")
        if f["mcu"] not in notes:
            close = difflib.get_close_matches(f.raw_of("mcu"), list(notes), 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            v.err(f"{fp}.mcu", kl, f"unknown MCU LED '{f.raw_of('mcu')}'{hint}")
            return None
        return (LED_SOURCES[k], notes[f["mcu"]], 0)
    if not f1_on(dev):
        v.err(f"{fp}.{k}", kl, f"{k}: is plain feedback, but " + f1_off_why(dev))
        return None
    if k in LED_SOURCE_VALUES:
        if f[k] not in LED_SOURCE_VALUES[k]:
            v.err(f"{fp}.{k}", kl, f"'{f.raw_of(k)}' isn't a {k} source (valid: {', '.join(LED_SOURCE_VALUES[k])})")
            return None
        return (LED_SOURCES[k], 0, 0)
    num = v.integer(f, k, fp, 0, 127, default=None)
    if num is None:
        return None
    return (LED_SOURCES[k], num, ch)


def led_rgb_c(x):
    """gm_led_t.rgb: full-colour idle / active looks, 0 each for none."""
    rgb = getattr(x, "rgb", None)
    if not rgb or not any(c is not None for c in rgb):
        return ", { 0, 0 }"
    return ", { " + ", ".join("0" if c is None else f"GM_LED_RGB(0x{c:06X})" for c in rgb) + " }"


def led_looks_c(x):
    """gm_led_t.looks: 0 for the defaults, else GM_LED_LOOKS(idle, active)."""
    looks = getattr(x, "looks", None)
    return ", 0" if looks is None else f", GM_LED_LOOKS({looks[0]}, {looks[1]})"


def c_tables(dev):
    """gm_led_t leds[] (gm_device.h)."""
    c = []
    if dev.leds:
        c.append("static const gm_led_t leds[GM_N_LEDS] = {")
        c += [f"    {{ {x.pin.c()}, {x.pin2.c()}, {x.pin3.c()}{led_looks_c(x)}{twin_c(x)}{led_rgb_c(x)} }}, "
              f"/* {c_comment(x.id)}: {c_comment(x.pin.desc)} */" for x in dev.leds]
        c += ["};", ""]
    return c
