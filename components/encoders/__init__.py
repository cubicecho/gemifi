"""encoders: the rotary encoders (`encoders:`, `encoder_settings:`), a control: encoders.c counts their detents
from the scan and sends them, this file validates their YAML and writes their C (README.md here). The
builder (gemifi_device.py) calls validate() with the other controls, and c_defines() / c_tables() where
gm_device_config.h / .c have them.
"""
import difflib

from components.api import NO_PIN, RENAMED_ID, Hub, c_str, twin_c, type_name
from components.cd74hc4067 import Mux
from components.hub import c_comment

# encoder.mode (gm_settings.h ENC_*): from cc14 on they need a message kind (core/README.md "MIDI messages").
ENC_MODES = ["absolute", "relative_twos", "relative_offset", "relative_sign", "cc14", "nrpn", "nrpn14",
             "rpn", "rpn14"]
ENCODER_KEYS = ["id", "name", "pin_a", "pin_b", "cc", "steps_per_detent", "mcu", "ring", "ring_center", "daw",
                "acceleration", "mode", "nrpn", "rpn", "channel", "min", "max"]
ENCODER_NUMBER_KEYS = ("cc", "nrpn", "rpn")     # the keys that give encoder.cc, by mode
RENAMED_ENCODER = dict(RENAMED_ID, a="renamed: use `pin_a:` (as ESPHome's rotary_encoder)",
                       b="renamed: use `pin_b:` (as ESPHome's rotary_encoder)")


def in_device(dev):
    """Built when the device has encoders (GM_N_ENCODERS > 0, twins included)."""
    return bool(dev.encoders)


def validate(v, dev, data):
    """encoders: and encoder_settings: -> dev.encoders."""
    names = {}       # (kind, display name) -> path, for the duplicate-name warning
    encs = v.seq(data.get("encoders"), "encoders", data.line_of("encoders"))
    accel_default = encoder_settings(v, dev, data)
    for i, d in enumerate(encs):
        p = f"encoders[{i}]"
        d = v.mapping(d, p, encs.line_of(i), ENCODER_KEYS, renamed=RENAMED_ENCODER)
        if d is None:
            continue
        ident = v.ident(d, p, "encoder", len(dev.encoders))
        name = v.display_name(d, p, ident)
        v.unique_name(names, "encoder", name, p, d)
        enc_hubs = v.hub_keys("in") - v.hub_keys(Mux)
        why = "encoders can't be on a mux"
        pins = {}
        for key in ("pin_a", "pin_b"):
            if key not in d and key[-1] in d:
                pins[key] = None        # format 1 a:/b:, reported as renamed
                continue
            pins[key] = v.pin(d.get(key), f"{p}.{key}", d.line_of(key), "in", hubs=enc_hubs, why=why)
            pn = pins[key]
            if pn and pn.hub and dev.hubs[pn.hub - 1].kind.i2c:
                v.warn(f"{p}.{key}", d.line_of(key), "encoder on an I2C hub is polled from the main "
                                                     "loop: fast spinning may miss steps")
        vpot = v.mcu_name(dev, d, p, "vpot")
        mode = 0
        if "mode" in d:
            mode = v.enum_value(d, "mode", f"{p}.mode", ENC_MODES)
            mode = 0 if mode is None else mode
            v.mode_use(dev, ENC_MODES[mode], f"{p}.mode", d.line_of("mode"))
        for k in ("mode", "channel", "min", "max"):
            if k in d and vpot is not None:
                v.err(f"{p}.{k}", d.line_of(k), f"a Mackie Control V-Pot (mcu:) sends its own messages: drop {k}: "
                                                "or mcu:")
        cc = v.mode_number(dev, d, p, ENC_MODES[mode], vpot is None, ENCODER_NUMBER_KEYS)
        ctl = v.control_out(d, p, 16383 if v.mode_wide(dev, ENC_MODES[mode], cc) else 127)
        spd = v.integer(d, "steps_per_detent", p, 1, 255, default=4)
        ring = encoder_ring(v, dev, d, p, vpot)
        center = ring_center(v, dev, d, p, vpot)
        accel = validate_acceleration(v, d, p, accel_default)[1]
        dev.encoders.append(Hub(id=ident or "", name=name, a=pins["pin_a"] or NO_PIN, b=pins["pin_b"] or NO_PIN,
                                cc=cc, steps=spd, mcu=vpot, ring=ring, ring_center=center,
                                daw=v.daw_role(dev, d, p), accel=accel, mode=mode, ctl=ctl))


def encoder_ring(v, dev, d, p, vpot):
    """ring: [LED ids] around an MCU V-Pot (leftmost first)."""
    if d.get("ring") is None:
        return []
    line = d.line_of("ring")
    if vpot is None and not dev.host_fb:
        if dev.mcu is not None or "mcu" not in d:
            v.err(f"{p}.ring", line, "ring: is driven by Mackie Control or a DAW script: give the encoder "
                                     "`mcu: vpot_N`, or leave feedback.script on")
        return []
    lst = v.seq(d["ring"], f"{p}.ring", line)
    if not 1 <= len(lst) <= 16:
        v.err(f"{p}.ring", line, f"{len(lst)} LEDs: a ring has 1..16")
    out = []
    for k in range(len(lst)):
        idx = v.ref(lst, k, f"{p}.ring[{k}]", lst.line_of(k), "LED", "LEDs")
        if idx is not None:
            out.append(idx)
            dev.leds[idx].used_by.append(("ring", None))
    return out


def ring_center(v, dev, d, p, vpot):
    """ring_center: the LED under an MCU V-Pot (lit by bit 6 of its ring value). -> LED index or None."""
    if d.get("ring_center") is None:
        return None
    line = d.line_of("ring_center")
    if vpot is None and not dev.host_fb:
        if dev.mcu is not None or "mcu" not in d:
            v.err(f"{p}.ring_center", line, "ring_center: is driven by Mackie Control or a DAW script: give the "
                                            "encoder `mcu: vpot_N`, or leave feedback.script on")
        return None
    idx = v.ref(d, "ring_center", f"{p}.ring_center", line, "LED", "LEDs")
    if idx is not None:
        dev.leds[idx].used_by.append(("ring", None))
    return idx


# Acceleration (core/README.md "Encoder acceleration").
# A turn whose detents come at most `slow` apart moves more than one step per detent: up to `max` steps
# at `fast` apart or less, along `curve` in between. Values are (slow ms, fast ms, max, curve index);
# None is off (1 step per detent, the C table's { 0, 0, 1, linear }).
ACCEL_CURVES = ["linear", "quadratic", "cubic"]     # gm_device.h GM_ACCEL_*
ACCEL_PRESETS = {"off": None,
                 "low": (60, 15, 4, 0),
                 "medium": (80, 10, 8, 1),
                 "high": (100, 8, 16, 1)}
ACCEL_KEYS = ["slow", "fast", "max", "curve"]
ACCEL_SLOW_MS = (2, 1000)
ACCEL_FAST_MS = (1, 999)
ACCEL_MAX = (1, 32)
ENCODER_SETTINGS_KEYS = ["acceleration"]


def accel_text(a):
    """(slow, fast, max, curve) -> the preset's name, or 'slow 80ms, fast 10ms, max 8, quadratic'."""
    for name, p in ACCEL_PRESETS.items():
        if p == a:
            return name
    return f"slow {a[0]}ms, fast {a[1]}ms, max {a[2]}, {ACCEL_CURVES[a[3]]}"


def validate_acceleration(v, d, path, base):
    """`acceleration:` in d (an encoder, or encoder_settings:): off / low / medium / high, or a mapping
    { slow, fast, max, curve } whose missing keys come from base (a preset tuple; medium when base is off).
    Returns (given, value): value is a tuple or None (off); given False when d has no acceleration:."""
    if d.get("acceleration") is None:
        return False, base
    x, p, line = d["acceleration"], f"{path}.acceleration" if path else "acceleration", d.line_of("acceleration")
    names = list(ACCEL_PRESETS)
    if x is False:                  # bare `off`: YAML 1.1 reads it as false
        return True, None
    if isinstance(x, str):
        if x in ACCEL_PRESETS:
            return True, ACCEL_PRESETS[x]
        close = difflib.get_close_matches(x, names, 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(p, line, f"unknown acceleration '{x}' ({hint}valid: {', '.join(names)}, or a mapping "
                       "{ slow, fast, max, curve })")
        return True, base
    if not isinstance(x, dict):
        v.err(p, line, f"must be {', '.join(names)} or a mapping {{ slow, fast, max, curve }} (got "
                       f"{type_name(x)} {d.raw_of('acceleration')!r})")
        return True, base
    m = v.mapping(x, p, line, ACCEL_KEYS)
    slow, fast, mx, curve = base or ACCEL_PRESETS["medium"]
    slow = v.period(m, "slow", p, ACCEL_SLOW_MS[0] * 1000, ACCEL_SLOW_MS[1] * 1000, default=slow,
                    step_us=1000, example="80ms")
    fast = v.period(m, "fast", p, ACCEL_FAST_MS[0] * 1000, ACCEL_FAST_MS[1] * 1000, default=fast,
                    step_us=1000, example="10ms")
    mx = v.integer(m, "max", p, *ACCEL_MAX, default=mx)
    if m.get("curve") is not None:
        c = v.enum_value(m, "curve", f"{p}.curve", ACCEL_CURVES)
        curve = curve if c is None else c
    if fast >= slow:
        v.err(p, line, f"fast ({fast}ms) must be shorter than slow ({slow}ms): detents fast apart get max "
                       "steps, slow apart 1")
        return True, base
    if mx == 1:
        v.warn(p, m.line_of("max"), "max 1: one step per detent at any speed (the same as off)")
        return True, None
    return True, (slow, fast, mx, curve)


def encoder_settings(v, dev, data):
    """encoder_settings: what all encoders share unless they set their own: acceleration (off by default)."""
    if data.get("encoder_settings") is None:
        return None
    p, line = "encoder_settings", data.line_of("encoder_settings")
    s = v.mapping(data["encoder_settings"], p, line, ENCODER_SETTINGS_KEYS)
    if s is None:
        return None
    if not data.get("encoders"):
        v.warn(p, line, "there are no encoders: ignored")
    return validate_acceleration(v, s, p, None)[1]


def has_accel(dev):
    """Any encoder with acceleration: GM_ENC_ACCEL 1 and gm_enc_accel[]; none: the feature compiles out."""
    return any(getattr(e, "accel", None) for e in dev.encoders)


def c_defines(dev):
    """GM_ENC_ACCEL: 1 with acceleration (gm_enc_accel[])."""
    return [f"#define GM_ENC_ACCEL {1 if has_accel(dev) else 0} /* 1: gm_enc_accel[] (encoder acceleration:) */"]


def c_tables(dev):
    """gm_encoder_t encoders[] (gm_device.h) and, with acceleration, gm_enc_accel[]."""
    c = []
    if dev.encoders:
        c.append("static const gm_encoder_t encoders[GM_N_ENCODERS] = {")
        c += [f"    {{ {e.a.c()}, {e.b.c()}, {c_str(e.name)}, {e.steps}{twin_c(e)} }}, "
              f"/* {c_comment(e.id)}: {c_comment(e.a.desc)}, {c_comment(e.b.desc)} */" for e in dev.encoders]
        c += ["};", ""]
    if has_accel(dev):
        own = [e for e in dev.encoders if not getattr(e, "layer", 0)]
        c.append("/* acceleration: per scanned encoder (twins share theirs): slow ms, fast ms, max, curve */")
        c.append(f"const gm_enc_accel_t gm_enc_accel[{len(own)}] = {{")
        for e in own:
            a = getattr(e, "accel", None)
            body = (f"{a[0]}, {a[1]}, {a[2]}, GM_ACCEL_{ACCEL_CURVES[a[3]].upper()}" if a
                    else "0, 0, 1, GM_ACCEL_LINEAR")
            c.append(f"    {{ {body} }}, /* {c_comment(e.id)}: {accel_text(a) if a else 'off'} */")
        c += ["};", ""]
    return c
