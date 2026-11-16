"""analog: the analog inputs (`analog:`: pots, faders, wheels, pressure strips), a control: analog.c reads them
and sends what they're set to, this file validates their YAML and writes their C (README.md here). The
builder (gemifi_device.py) calls validate() with the other controls, and c_param_defaults() and c_tables()
where gm_device_config.c has them.
"""
from components.api import NO_PIN, RENAMED_ID, Hub, c_param_array, c_str, f1_off_why, f1_on, mode_wide
from components.hub import c_comment
from components.keys import (CONTROL_DAW, CONTROL_ID, CONTROL_NAME, CONTROL_NRPN, CONTROL_OUT, CONTROL_RPN, Enum,
                             IdRef, Int, Key, Keys, Pin, Section, Str)
from components.xy_pad import XYPad

# analog.mode (gm_settings.h AN_MODE_*): from cc14 on they need a message kind (core/README.md "MIDI messages").
ANALOG_MODES = ["cc", "pitch_bend", "channel_pressure", "cc14", "nrpn", "nrpn14", "rpn", "rpn14", "sysex"]
ANALOG_NUMBER_KEYS = ("cc", "nrpn", "rpn", "sysex")     # the keys that give analog.cc, by mode
RENAMED_ANALOG = dict(RENAMED_ID, raw_min="renamed: use `adc_min:`", raw_max="renamed: use `adc_max:`")
TAKEOVER = ["none", "pickup"]
TOUCH = {"none": 0, "movement": 2}  # analog touch: -> gm_device.h GM_TOUCH_* (1 = a sensor, reserved)

# One analog input's keys (core/README.md "Controls" has what the controls share), checked by validate().
ANALOG = Keys(
    CONTROL_ID, CONTROL_NAME,
    Key("pin", Pin(), required=True, doc="What it reads: an ADC pin, a multiplexer channel or an XY pad axis."),
    Key("cc", Int(0, 127), doc="The CC it sends (0..31 with mode `cc14`). Needed with mode `cc` unless `mcu:` is "
                               "set.", see='core/README.md "MIDI messages"', param="analog.cc"),
    Key("mode", Enum(ANALOG_MODES), default="cc", param="analog.mode", see="Pitch wheels and aftertouch",
        doc="What it sends: a CC, pitch bend, channel pressure, or a 14-bit CC, NRPN, RPN or SysEx string "
            "(core/README.md \"MIDI messages\")."),
    Key("adc_min", Int(0, 4095), default=0, doc="The raw reading at the bottom of its travel. Greater than "
                                                "`adc_max` turns it around."),
    Key("adc_max", Int(0, 4095), default=4095, doc="The raw reading at the top of its travel."),
    Key("takeover", Enum(TAKEOVER), default="none", shown_default="`none`; `pickup` with `mcu:`",
        see='core/README.md "Plain feedback (F1, every DAW)"',
        doc="`pickup`: after the host sets a value, it stays silent until it reaches or crosses it. Needs DAW "
            "feedback."),
    Key("touch", Enum(TOUCH), default="none", see='core/README.md "DAW scripts (F3)"',
        doc="`movement`: it tells a DAW script when it is touched (moving) and let go."),
    Key("mcu", Str(), doc="Makes it a Mackie Control fader (`fader_1`..`fader_8`, `master`), which sends its own "
                          "messages: no `mode:`, `channel:`, `min:` or `max:` then.",
        see='core/README.md "Second USB port and Mackie Control"'),
    CONTROL_DAW, CONTROL_NRPN.but(param="analog.cc"), CONTROL_RPN.but(param="analog.cc"),
    Key("sysex", IdRef("SysEx string"), param="analog.cc", see='core/README.md "MIDI messages"',
        doc="The id of the `midi_messages.sysex` string it sends with mode `sysex`, instead of `cc:`."),
    *CONTROL_OUT,
)
ANALOG_KEYS = ANALOG.names
KEY_SECTIONS = (Section("analog", ANALOG),)


def in_device(dev):
    """Built when the device has analog inputs (GM_N_ANALOGS > 0)."""
    return bool(dev.analogs)


def validate(v, dev, data):
    """analog: -> dev.analogs."""
    names = {}       # (kind, display name) -> path, for the duplicate-name warning
    analogs = v.seq(data.get("analog"), "analog", data.line_of("analog"))
    for i, d in enumerate(analogs):
        p = f"analog[{i}]"
        d = v.mapping(d, p, analogs.line_of(i), ANALOG_KEYS, renamed=RENAMED_ANALOG)
        if d is None:
            continue
        ident = v.ident(d, p, "analog input", len(dev.analogs))
        name = v.display_name(d, p, ident)
        v.unique_name(names, "analog input", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "analog")
        fader = v.mcu_name(dev, d, p, "fader")
        mode = v.control_mode(dev, d, p, ANALOG_MODES) or 0
        if mode != 0 and fader is not None:
            v.err(f"{p}.mode", d.line_of("mode"), "a Mackie Control fader (mcu:) sends its own messages: "
                                                  f"drop mode: {ANALOG_MODES[mode]} or mcu:")
        if ANALOG_MODES[mode] == "pitch_bend" and pin is not None and pin.hub and isinstance(dev.hubs[pin.hub - 1].kind, XYPad):
            v.err(f"{p}.mode", d.line_of("mode"), "pitch_bend centres on the power-up reading, and an XY "
                                                  "pad reads nothing until touched: use cc or channel_pressure")
        cc = v.mode_number(dev, d, p, ANALOG_MODES[mode], fader is None and mode == 0, ANALOG_NUMBER_KEYS)
        if fader is not None:
            for k in ("channel", "min", "max"):
                if k in d:
                    v.err(f"{p}.{k}", d.line_of(k), "a Mackie Control fader (mcu:) sends its own messages: "
                                                    f"drop {k}: or mcu:")
        ctl = v.control_out(d, p, 16383 if mode_wide(dev, ANALOG_MODES[mode], cc) else 127)
        lo = ANALOG.get(v, d, "adc_min", p)
        hi = ANALOG.get(v, d, "adc_max", p)
        if lo == hi:
            v.err(p, d.line, f"adc_min and adc_max are both {lo}")
        pickup = validate_takeover(v, dev, d, p, fader)
        dev.analogs.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, cc=cc, mode=mode, raw_min=lo,
                               raw_max=hi, pickup=pickup, touch=validate_touch(v, d, p), mcu=fader,
                               daw=v.daw_role(dev, d, p), ctl=ctl))


def validate_takeover(v, dev, d, p, fader):
    """analog takeover: none | pickup. MCU faders default to pickup (no motors to follow the host)."""
    if d.get("takeover") is None:
        return fader is not None
    t = d["takeover"]
    if t not in TAKEOVER:
        v.err(f"{p}.takeover", d.line_of("takeover"), f"unknown takeover '{d.raw_of('takeover')}' "
                                                      f"(valid: {', '.join(TAKEOVER)})")
        return False
    if t == "pickup" and fader is None and not f1_on(dev):
        v.err(f"{p}.takeover", d.line_of("takeover"), "pickup needs the host's value: " + f1_off_why(dev))
        return False
    return t == "pickup"


def validate_touch(v, d, p):
    """analog touch: none | movement (EVENT touch while it moves) -> GET_COMPONENT's touch byte. A touch
    sensor, touch: { pin: ... }, is reserved."""
    if d.get("touch") is None:
        return 0
    t, line = d["touch"], d.line_of("touch")
    if isinstance(t, dict):
        v.err(f"{p}.touch", line, "a touch sensor isn't supported yet (use touch: movement)")
        return 0
    if t not in TOUCH:
        v.err(f"{p}.touch", line, f"unknown touch '{d.raw_of('touch')}' (valid: {', '.join(TOUCH)})")
        return 0
    return TOUCH[t]


def c_param_defaults(dev):
    """gm_analog_cc_defaults[] and gm_analog_mode_defaults[] (core/src/settings.c), even without analog
    inputs."""
    modes = " /* " + ", ".join(ANALOG_MODES[a.mode] for a in dev.analogs) + " */" if dev.analogs else ""
    return [c_param_array("gm_analog_cc_defaults", "GM_N_ANALOGS", [a.cc for a in dev.analogs]),
            c_param_array("gm_analog_mode_defaults", "GM_N_ANALOGS", [a.mode for a in dev.analogs], modes)]


def c_tables(dev):
    """gm_analog_t analogs[] (gm_device.h)."""
    c = []
    if dev.analogs:
        c.append("static const gm_analog_t analogs[GM_N_ANALOGS] = {")
        c += [f"    {{ {a.pin.c()}, {c_str(a.name)}, {a.raw_min}, {a.raw_max} }}, "
              f"/* {c_comment(a.id)}: {c_comment(a.pin.desc)} */"
              for a in dev.analogs]
        c += ["};", ""]
    return c
