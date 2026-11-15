"""pads: the velocity pads (`pads:`, `pad_settings:`), a control: pads.c reads them and plays their notes,
this file validates their YAML and writes their C (README.md here). The builder (gemifi_device.py) calls
validate() with the other controls, and c_defines() / c_tables() where gm_device_config.h / .c have them.
"""
from components.api import CURVES, NO_PIN, RENAMED_ID, Hub, c_str
from components.cd74hc4067 import Mux
from components.hub import c_comment

# pad_settings: the pad.* param defaults (core/src/settings.c GM_PAD_*_DEFAULT). Readings are 0..4095,
# after inverted:; window and retrigger in ms.
PAD_DEFAULTS = {"channel": 0, "curve": 0, "aftertouch": 0, "threshold": 200, "release": 100, "full": 3000,
                "window": 3, "retrigger": 20}
PAD_KEYS = ["id", "name", "pin", "note", "led"]
PAD_SETTINGS_KEYS = list(PAD_DEFAULTS)
MAX_PADS = 128


def in_device(dev):
    """Built when the device has pads (GM_N_PADS > 0)."""
    return bool(dev.pads)


def validate(v, dev, data):
    """pads: one entry per pad, an analog source (ADC pin or mux channel) and its default note; then
    pad_settings: (pad_settings)."""
    names = {}
    pads = v.seq(data.get("pads"), "pads", data.line_of("pads"))
    for i, d in enumerate(pads):
        p = f"pads[{i}]"
        d = v.mapping(d, p, pads.line_of(i), PAD_KEYS, renamed=RENAMED_ID)
        if d is None:
            continue
        ident = v.ident(d, p, "pad", len(dev.pads))
        name = v.display_name(d, p, ident)
        v.unique_name(names, "pad", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog", hubs=v.hub_keys(Mux),
                    why="a pad is read with the ADC")
        note = v.integer(d, "note", p, 0, 127, default=0, required=True)
        led = -1
        if d.get("led") is not None:           # lit while the pad is held
            idx = v.ref(d, "led", f"{p}.led", d.line_of("led"), "LED", "LEDs")
            if idx is not None and idx > 127:
                v.err(f"{p}.led", d.line_of("led"), "LED index over 127 can't be referenced")
            elif idx is not None:
                led = idx
                dev.leds[led].used_by.append(("pad", None))
        dev.pads.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, note=note, led=led, path=p,
                            line=d.line_of("note") if "note" in d else d.line))
    if len(dev.pads) > MAX_PADS:
        v.err("pads", data.line_of("pads"), f"{len(dev.pads)} pads: at most {MAX_PADS}")
    seen = {}
    for x in dev.pads:
        if x.note in seen:
            v.warn(f"{x.path}.note", x.line, f"note {x.note} is also the note of {seen[x.note]}: both pads "
                                              "play the same note")
        seen.setdefault(x.note, x.path)
    pad_settings(v, dev, data)


def pad_settings(v, dev, data):
    """pad_settings: what all pads share -> dev.pad_set: the sensing (global) and channel / curve /
    aftertouch (per preset)."""
    if data.get("pad_settings") is None:
        return
    p, line = "pad_settings", data.line_of("pad_settings")
    s = v.mapping(data["pad_settings"], p, line, PAD_SETTINGS_KEYS)
    if s is None:
        return
    if not dev.pads:
        v.warn(p, line, "there are no pads: ignored")
    ps = dev.pad_set

    def put(key, val):
        if val is not None:
            ps[key] = val

    put("channel", v.integer(s, "channel", p, 0, 16))
    if "curve" in s:
        put("curve", v.enum_value(s, "curve", f"{p}.curve", CURVES))
    if "aftertouch" in s:
        put("aftertouch", int(v.boolean(s, "aftertouch", p)))
    put("threshold", v.integer(s, "threshold", p, 1, 4095))
    put("release", v.integer(s, "release", p, 0, 4094))
    put("full", v.integer(s, "full", p, 2, 4095))
    put("window", v.period(s, "window", p, 1000, 50_000, step_us=1000, example="3ms"))
    put("retrigger", v.period(s, "retrigger", p, 0, 500_000, step_us=1000, example="20ms"))
    if not ps["release"] < ps["threshold"] < ps["full"]:
        v.err(p, line, f"needs release < threshold < full (got release {ps['release']}, threshold "
                       f"{ps['threshold']}, full {ps['full']}; readings 0..4095, with the defaults "
                       f"{PAD_DEFAULTS['release']}, {PAD_DEFAULTS['threshold']}, {PAD_DEFAULTS['full']} "
                       "for those not given)")


def c_defines(dev):
    """GM_PAD_TWINS (the pad layer twins, gm_pad_twins[], when there are any), GM_N_PADS, and with pads the
    pad.* param defaults from pad_settings: (core/src/settings.c)."""
    h = [f"#define GM_PAD_TWINS {len(dev.pad_twins)} /* gm_pad_twins[] */"] if dev.pad_twins else []
    h += ["", f"#define GM_N_PADS {len(dev.pads)} /* pads: (components/pads/README.md \"Pads\") */"]
    if dev.pads:
        ps = dev.pad_set
        h += [f"#define GM_PAD_CHANNEL_DEFAULT {ps['channel']} /* 0 = midi.channel */",
              f"#define GM_PAD_CURVE_DEFAULT {ps['curve']} /* {CURVES[ps['curve']]} */",
              f"#define GM_PAD_AFTERTOUCH_DEFAULT {ps['aftertouch']}",
              f"#define GM_PAD_THRESHOLD_DEFAULT {ps['threshold']}",
              f"#define GM_PAD_RELEASE_DEFAULT {ps['release']}",
              f"#define GM_PAD_FULL_DEFAULT {ps['full']}",
              f"#define GM_PAD_WINDOW_DEFAULT {ps['window']} /* ms */",
              f"#define GM_PAD_RETRIGGER_DEFAULT {ps['retrigger']} /* ms */"]
    return h


def c_tables(dev):
    """gm_pads[] (gm_device.h gm_pad_t) and the pad.note defaults; nothing without pads."""
    if not dev.pads:
        return []
    return ["const gm_pad_t gm_pads[GM_N_PADS] = {",
            *[f"    {{ {x.pin.c()}, {c_str(x.name)}, {x.led} }}, /* {c_comment(x.id)}: {c_comment(x.pin.desc)} */"
              for x in dev.pads],
            "};",
            "const int32_t gm_pad_note_defaults[GM_N_PADS] = { " + ", ".join(str(x.note) for x in dev.pads) + " };",
            *(["/* pad layer twins: { layer (mode index + 1), base pad, LED } */",
               "const gm_pad_twin_t gm_pad_twins[GM_PAD_TWINS] = {",
               *[f"    {{ {x.layer}, {x.base}, {x.led} }}, /* {c_comment(x.id)} */" for x in dev.pad_twins],
               "};"] if dev.pad_twins else []),
            ""]
