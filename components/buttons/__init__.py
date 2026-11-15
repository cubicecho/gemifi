"""buttons: the buttons (`buttons:`), a control: buttons.c samples them and runs their actions, this file
validates their YAML (and a preset's or a layer's button action) and writes their C (README.md here). The
builder (gemifi_device.py) calls validate() after the LEDs, and c_defines(), c_param_defaults() and
c_tables() where gm_device_config.h / .c have them.
"""
import difflib

from components.api import NO_PIN, RENAMED_ID, Hub, c_param_array, c_str, sysex_wide, twin_c
from components.hub import c_comment
from components.leds import led_ref

# components/buttons/README.md "Button actions": YAML name -> button.action value (gm_settings.h ACT_*). The preset
# actions (13-15) need 2+ preset slots; `mcu` (16) only exists on devices with an `mcu:` section; the
# MIDI message actions (17-20) are compiled in only on devices that use them (MSG_ACTIONS).
ACTION_IDS = {"none": 0, "cc_momentary": 1, "cc_toggle": 2, "cc_ramp": 3, "note": 4, "octave_down": 5,
              "octave_up": 6, "transpose_down": 7, "transpose_up": 8, "bend_down": 9, "bend_up": 10,
              "program_next": 11, "program_prev": 12, "preset_next": 13, "preset_prev": 14,
              "preset_select": 15, "mcu": 16, "program": 17, "panic": 18, "mmc": 19, "sysex": 20,
              "pad_octave_down": 21, "pad_octave_up": 22}
ACTIONS = list(ACTION_IDS)
CC_ACTIONS = {"cc_momentary", "cc_toggle", "cc_ramp"}
PRESET_ACTIONS = {"preset_next", "preset_prev", "preset_select"}
PROGRAM_ACTIONS = {"program_next", "program_prev", "program"}     # these take bank:
PAD_ACTIONS = {"pad_octave_down", "pad_octave_up"}                  # only on devices with pads
MSG_ACTIONS = {"program": "program", "panic": "panic", "mmc": "mmc", "sysex": "sysex"}   # action -> its MIDI message kind (MSG_FEATURES)
# the YAML keys that give button.number (mcu: aside)
NUMBER_KEYS = ("cc", "note", "preset", "program", "mmc", "sysex")
# MMC commands (MIDI 1.0 MMC, `F0 7F dev 06 cmd F7`): the YAML's mmc: names -> button.number
MMC_COMMANDS = {"stop": 1, "play": 2, "deferred_play": 3, "fast_forward": 4, "rewind": 5, "record": 6,
                "record_exit": 7, "record_pause": 8, "pause": 9, "eject": 10, "chase": 11, "reset": 13}

BUTTON_KEYS = ["id", "name", "pin", "action", "cc", "note", "preset", "led", "mcu", "daw", "program", "mmc", "sysex",
               "bank", "channel", "min", "max"]
RENAMED_BUTTON = dict(RENAMED_ID, number="renamed: use `cc:` (cc_momentary / cc_toggle / cc_ramp) or "
                                         "`note:` (action note)")


def in_device(dev):
    """Built when the device has buttons (GM_N_BUTTONS > 0, twins included)."""
    return bool(dev.buttons)


def validate(v, dev, data):
    """buttons: -> dev.buttons (after the LEDs: a button names its LED)."""
    names = {}       # (kind, display name) -> path, for the duplicate-name warning
    buttons = v.seq(data.get("buttons"), "buttons", data.line_of("buttons"))
    for i, d in enumerate(buttons):
        p = f"buttons[{i}]"
        d = v.mapping(d, p, buttons.line_of(i), BUTTON_KEYS, renamed=RENAMED_BUTTON)
        if d is None:
            continue
        ident = v.ident(d, p, "button", len(dev.buttons))
        name = v.display_name(d, p, ident)
        v.unique_name(names, "button", name, p, d)
        pin = v.pin(d.get("pin"), f"{p}.pin", d.line_of("pin"), "in")
        act, num = button_action(v, dev, d, p)
        led = led_ref(v, dev, d, p, ("button", act))
        top = 16383 if act == "sysex" and sysex_wide(dev, num) else 127
        dev.buttons.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, action=ACTION_IDS[act],
                               action_name=act, number=num, led=led, daw=v.daw_role(dev, d, p),
                               ctl=v.control_out(d, p, top), bank=button_bank(v, dev, d, p, act)))


def button_action(v, dev, d, p, base=None):
    """A button's action and default button.number, from action: and cc: / note: / preset: / mcu:.
    base: (action, number) of the component, for a preset's override of it: what the override leaves
    out stays as the component has it. Returns (action name, number)."""
    if "action" in d:
        act = d["action"]
        if act not in ACTIONS:
            close = difflib.get_close_matches(str(act), ACTIONS, 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.action", d.line_of("action"), f"unknown action '{d.raw_of('action')}' ({hint}valid: "
                                                       f"{', '.join(ACTIONS)})")
            return base or ("none", 0)
    else:
        act = base[0] if base else "mcu" if "mcu" in d else "none"
    if act in MSG_ACTIONS:
        if act == "sysex" and not dev.sysex:
            v.err(f"{p}.action", d.line_of("action") if "action" in d else d.line,
                  "action sysex sends one of the midi_messages.sysex strings, and there are none")
        dev.msg_used.add(MSG_ACTIONS[act])
    if act in PAD_ACTIONS:
        dev.pad_action_uses.append((f"{p}.action", d.line_of("action") if "action" in d else d.line, act))
    if act in PRESET_ACTIONS and dev.n_presets < 2:
        v.err(f"{p}.action", d.line_of("action") if "action" in d else d.line,
              f"action {act} switches presets, but this device has {dev.n_presets} preset slot: add "
              "presets: (with 2 or more slots)")
        return act, (base[1] if base else 0)
    if act == "mcu" or "mcu" in d:
        if base and act == base[0] == "mcu" and "mcu" not in d:
            return act, base[1]
        num = button_mcu(v, dev, d, p, act)
        return (act if act == "mcu" else base[0] if base else "none"), num
    return act, button_number(v, dev, d, p, act, base)


def number_key(act):
    """The key that gives button.number for an action (None: the action takes no number)."""
    if act in ("program", "mmc", "sysex"):
        return act
    return "note" if act == "note" else "cc" if act in CC_ACTIONS else "preset" if act == "preset_select" else None


def button_number(v, dev, d, p, act, base=None):
    """The default button.number: cc: for the CC actions, note: for action note, preset: (the slot) for
    preset_select. With base (a preset override), a number left out keeps the component's, and a
    number on an action that takes none is an error (in a component it only warns)."""
    want = number_key(act)
    keep = base[1] if base else 0
    given = [k for k in NUMBER_KEYS if k in d]
    if "cc" in d and "note" in d:
        v.err(p, d.line, "give cc: or note:, not both")
    elif len(given) > 1:
        v.err(p, d.line, f"give one of {', '.join(k + ':' for k in given)}, not {len(given)}")
    if want is None:
        if given:
            k = given[0]
            if base or k in ("preset", "mmc", "sysex", "program"):
                what = "a preset number" if k == "preset" else "a CC or note"
                v.err(f"{p}.{k}", d.line_of(k), f"action {act} takes no {what}: drop {k}:")
                return keep
            v.warn(f"{p}.{k}", d.line_of(k), f"action {act} sends no CC or note; {k}: only "
                                             "sets the default button.number for when the action is "
                                             "changed in the config")
            return v.integer(d, k, p, 0, 127, default=0)
        return keep
    other = [k for k in given if k != want]
    if other:
        v.err(f"{p}.{other[0]}", d.line_of(other[0]), f"action {act} takes {want}:, not {other[0]}:")
        return v.integer(d, other[0], p, 0, 127, default=0) if other[0] in ("cc", "note") else keep
    if "number" in d:          # format 1; reported as renamed
        return 0
    lo, hi = (1, dev.n_presets) if want == "preset" else (0, 127)
    if want not in d:
        if base and act == base[0]:
            return keep
        what = {"mmc": f"an MMC command: {', '.join(MMC_COMMANDS)}, or 1..127",
                "sysex": "the id of a midi_messages.sysex string"}.get(want)
        v.err(p, d.line, f"action {act} needs {want}: (" + (what or f"{lo}..{hi}" +
              (", the preset slot" if want == "preset" else "")) + ")")
        return 0
    if want == "mmc":
        return mmc_command(v, d, p)
    if want == "sysex":
        return v.sysex_ref(dev, d, "sysex", p)
    return v.integer(d, want, p, lo, hi, default=0)


def button_mcu(v, dev, d, p, act):
    """action: mcu + mcu: <button> -> the default button.number (the MCU note)."""
    line = d.line_of("mcu") if "mcu" in d else d.line
    if act != "mcu":
        v.err(f"{p}.mcu", line, f"mcu: is for action: mcu (this button's action is {act}; leave action: out "
                                "or set it to mcu)")
        return 0
    if not v.mcu_needed(dev, f"{p}.action", d.line_of("action") if "action" in d else line):
        return 0
    for k in NUMBER_KEYS:
        if k in d:
            v.err(f"{p}.{k}", d.line_of(k), f"action mcu takes mcu: (e.g. `mcu: play`), not {k}:")
    if d.get("mcu") is None:
        v.err(p, d.line, "action mcu needs mcu: (the MCU button, e.g. `mcu: play`, `rec_1`, `f1`)")
        return 0
    notes = v.mcu_table("button")
    if d["mcu"] not in notes:
        close = difflib.get_close_matches(d.raw_of("mcu"), list(notes), 1)
        hint = f"; did you mean '{close[0]}'?" if close else " (core/README.md \"Mackie Control\" has the list)"
        v.err(f"{p}.mcu", line, f"unknown MCU button '{d.raw_of('mcu')}'{hint}")
        return 0
    return notes[d["mcu"]]


def mmc_command(v, d, path):
    """mmc: play | stop | ... | 1..127 -> the MMC command byte (button.number)."""
    x = d["mmc"]
    if isinstance(x, int) and not isinstance(x, bool):
        if 1 <= x <= 127:
            return x
        v.err(f"{path}.mmc", d.line_of("mmc"), f"{x} is out of range (1..127)")
        return 0
    if isinstance(x, str) and x in MMC_COMMANDS:
        return MMC_COMMANDS[x]
    close = difflib.get_close_matches(str(d.raw_of("mmc")), list(MMC_COMMANDS), 1)
    hint = f"did you mean '{close[0]}'? " if close else ""
    v.err(f"{path}.mmc", d.line_of("mmc"), f"unknown MMC command '{d.raw_of('mmc')}' ({hint}valid: "
                                           f"{', '.join(MMC_COMMANDS)}, or a number 1..127)")
    return 0


def button_bank(v, dev, d, p, act):
    """bank: 0..16383 (or { msb, lsb }) on a program action -> its Bank Select, or None."""
    if "bank" not in d:
        return None
    if act not in PROGRAM_ACTIONS:
        v.err(f"{p}.bank", d.line_of("bank"), f"bank: goes with the actions {', '.join(sorted(PROGRAM_ACTIONS))}, "
                                             f"not {act}")
        return None
    bank = v.bank_value(d, p)
    if bank is not None:
        dev.msg_used.add("program")
    return bank


def c_defines(dev):
    """GM_BTN_TWINS: the layer twins at the end of buttons[], when there are any."""
    twins = sum(1 for b in dev.buttons if getattr(b, "layer", 0))
    return [f"#define GM_BTN_TWINS {twins} /* the last buttons: layer twins (layer.button.*) */"] if twins else []


def c_param_defaults(dev):
    """gm_button_action_defaults[] and gm_button_number_defaults[] (core/src/settings.c), even without
    buttons."""
    acts = ", ".join(b.action_name for b in dev.buttons)
    return [c_param_array("gm_button_action_defaults", "GM_N_BUTTONS", [b.action for b in dev.buttons],
                          f" /* {acts} */" if acts else ""),
            c_param_array("gm_button_number_defaults", "GM_N_BUTTONS", [b.number for b in dev.buttons])]


def c_tables(dev):
    """gm_button_t buttons[] (gm_device.h)."""
    c = []
    if dev.buttons:
        c.append("static const gm_button_t buttons[GM_N_BUTTONS] = {")
        c += [f"    {{ {b.pin.c()}, {c_str(b.name)}, {b.led}{twin_c(b)} }}, "
              f"/* {c_comment(b.id)}: {c_comment(b.pin.desc)} */" for b in dev.buttons]
        c += ["};", ""]
    return c
