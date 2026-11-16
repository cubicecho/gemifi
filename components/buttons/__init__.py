"""buttons: the buttons (`buttons:`, `button_settings:`), a control: buttons.c samples them and runs their
actions and gestures, this file validates their YAML (and a preset's or a layer's button action) and writes
their C (README.md here). The builder (gemifi_device.py) calls validate() after the LEDs,
validate_gestures() after the modes, and c_defines(), c_param_defaults() and c_tables() where
gm_device_config.h / .c have them.
"""
import difflib

from components.api import NO_PIN, PRESETS_OFF, RENAMED_ID, Hub, LDict, c_param_array, c_str, sysex_wide, twin_c
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
               "bank", "channel", "min", "max", "hold", "double_tap", "repeat"]
# Gestures (README.md "Gestures"): a button's hold: and double_tap: take the keys of its action; what they
# leave out is the button's.
GESTURES = ("hold", "double_tap")
GESTURE_KEYS = ["action", "cc", "note", "preset", "program", "mmc", "sysex", "mcu"]
# Actions that work for as long as the button is held: a gesture can't run them, and a button with one can't
# have a gesture (its press would wait to be told apart).
HELD_ACTIONS = {"cc_ramp", "bend_down", "bend_up"}
# ... nor cc_toggle: its on/off state is the button's own, which the button's LED and host echoes show.
NOT_GESTURE_ACTIONS = HELD_ACTIONS | {"cc_toggle"}
# repeat: true (README.md "Auto-repeat"): the actions that repeat while the button is held.
REPEAT_ACTIONS = {"octave_down", "octave_up", "transpose_down", "transpose_up", "program_next", "program_prev",
                  "preset_next", "preset_prev", "pad_octave_down", "pad_octave_up"}
# combos: (README.md "Combos"): a combo's buttons and its action, with the keys of a gesture.
COMBO_KEYS = ["buttons", *GESTURE_KEYS]
MAX_COMBOS = 32                 # buttons.c combo_down: one bit each
# button_settings: the gesture timing params' defaults, in ms (core/src/settings.c GM_BTN_*_DEFAULT):
# key -> (default, lo, hi, the gesture that uses it: a button key, or combos).
BUTTON_SETTINGS = {"tap_time": (200, 50, 1000, "double_tap"), "hold_time": (200, 100, 5000, "hold"),
                   "combo_time": (50, 10, 500, "combos"), "repeat_delay": (500, 100, 2000, "repeat"),
                   "repeat_rate": (50, 20, 1000, "repeat")}
BUTTON_SETTINGS_KEYS = list(BUTTON_SETTINGS)
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
        gestures = {g: gesture_action(v, dev, d, p, g, (act, num)) for g in GESTURES if d.get(g) is not None}
        dev.buttons.append(Hub(id=ident or "", name=name, pin=pin or NO_PIN, action=ACTION_IDS[act],
                               action_name=act, number=num, led=led, daw=v.daw_role(dev, d, p),
                               ctl=v.control_out(d, p, top), bank=button_bank(v, dev, d, p, act),
                               gestures={g: a for g, a in gestures.items() if a is not None},
                               repeat=button_repeat(v, d, p, act, gestures), path=p, line=d.line))


def button_repeat(v, d, p, act, gestures):
    """repeat: true (README.md "Auto-repeat"): the button's action again while it is held. Not with a
    gesture, which waits to tell a press apart; an action that doesn't repeat is a warning (a preset or
    the host may give it one that does)."""
    if not v.boolean(d, "repeat", p):
        return False
    if gestures:
        v.err(f"{p}.repeat", d.line_of("repeat"), f"repeat: runs the action again while the button is held, and "
                                                  f"{' and '.join(g + ':' for g in gestures)} wait to tell a press "
                                                  "apart: give the button one or the other")
        return False
    if act not in REPEAT_ACTIONS:
        v.warn(f"{p}.repeat", d.line_of("repeat"), f"action {act} doesn't repeat (only {', '.join(sorted(REPEAT_ACTIONS))} "
                                                   "do): repeat: only works where a preset or the host gives the "
                                                   "button one of those")
    return True


def gesture_action(v, dev, d, p, key, base):
    """A button's hold: or double_tap: (README.md "Gestures"): the action it runs instead of the button's own, as
    (action name, button.number), or None. A string names the action; a mapping takes the keys of the
    button's action, and what it leaves out is the button's (base: its (action, number))."""
    path, line = f"{p}.{key}", d.line_of(key)
    g = d[key]
    if isinstance(g, str):
        g = LDict(action=g)
        g.line = line
    g = v.mapping(g, path, line, GESTURE_KEYS)
    if g is None:
        return None
    if not g:
        v.err(path, line, f"give the action it runs: `{key}: panic`, or `{key}: {{ cc: 21 }}` for the "
                          "button's own action with another number")
        return None
    if base[0] in HELD_ACTIONS:
        v.err(path, line, f"action {base[0]} works for as long as the button is held, so its press can't wait "
                          f"to be told apart from a {key}: give the {key}: to another button")
        return None
    act, num = button_action(v, dev, g, path, base=base)
    if number_key(act) is None and act != "mcu":
        num = 0                     # the button's number is for its own action, not this one
    if act in NOT_GESTURE_ACTIONS:
        why = ("its on/off state is the button's own" if act == "cc_toggle" else
               "it works for as long as the button is held, and a gesture is only known later")
        v.err(f"{path}.action" if "action" in g else path, g.line_of("action"),
              f"a gesture can't run action {act}: {why}")
        return None
    return act, num


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
    if act in PRESET_ACTIONS and not dev.has_presets:
        v.err(f"{p}.action", d.line_of("action") if "action" in d else d.line,
              f"action {act} switches presets, but {PRESETS_OFF}")
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


def validate_gestures(v, dev, data):
    """After the modes (they say which buttons switch them): a button a mode switches with has no
    gestures; then combos: -> dev.combos and button_settings: -> dev.btn_set, the timing params'
    defaults."""
    for idx, b in enumerate(dev.buttons):
        for g in [*getattr(b, "gestures", {}), *(["repeat"] if getattr(b, "repeat", False) else [])]:
            why = mode_use(v, dev, idx)
            if why:
                v.err(f"{b.path}.{g}", b.line, f"button '{b.id}' {why}, so it can't have {g}:")
    validate_combos(v, dev, data)
    button_settings(v, dev, data)


def validate_combos(v, dev, data):
    """combos: [{ buttons: [ids], action ... }] (README.md "Combos") -> dev.combos, one Hub each: mask (one
    bit per button), action name, number, path."""
    combos = v.seq(data.get("combos"), "combos", data.line_of("combos"))
    if len(combos) > MAX_COMBOS:
        v.err("combos", data.line_of("combos"), f"{len(combos)} combos: at most {MAX_COMBOS}")
        return
    seen = {}
    for i, d in enumerate(combos):
        p = f"combos[{i}]"
        d = v.mapping(d, p, combos.line_of(i), COMBO_KEYS)
        if d is None:
            continue
        mask = combo_buttons(v, dev, d, p)
        if "action" not in d:
            v.err(p, d.line, "needs action: (what the buttons pressed together do, e.g. `action: panic`)")
            continue
        act, num = button_action(v, dev, d, p)
        if number_key(act) is None and act != "mcu":
            num = 0
        if act in NOT_GESTURE_ACTIONS:
            v.err(f"{p}.action", d.line_of("action"), f"a combo can't run action {act}: it works for as long as "
                                                      "the button is held, or keeps an on/off state of its own")
            continue
        if mask is None:
            continue
        if mask in seen:
            v.err(f"{p}.buttons", d.line_of("buttons"), f"{seen[mask]} has the same buttons")
            continue
        seen[mask] = p
        dev.combos.append(Hub(mask=mask, action_name=act, number=num, path=p))


def combo_buttons(v, dev, d, p):
    """A combo's buttons: 2 or more of the YAML's own, each once, none a mode uses and none whose action
    works while held. Returns the mask, or None."""
    lst = v.seq(d.get("buttons"), f"{p}.buttons", d.line_of("buttons"))
    if len(lst) < 2:
        v.err(f"{p}.buttons", d.line_of("buttons"), "needs 2 or more buttons (e.g. `buttons: [b1, b2]`)")
        return None
    mask, ok = 0, True
    for k in range(len(lst)):
        bp, bl = f"{p}.buttons[{k}]", lst.line_of(k)
        idx = v.ref(lst, k, bp, bl, "button", "buttons")
        if idx is None:
            ok = False
            continue
        b = dev.buttons[idx]
        why = ("is a layer twin: give the combo the button itself" if getattr(b, "layer", 0) else
               mode_use(v, dev, idx) or
               (f"has action {b.action_name}, which works for as long as the button is held"
                if b.action_name in HELD_ACTIONS else None))
        if (mask >> idx) & 1:
            why = "is listed twice"
        if why:
            v.err(bp, bl, f"button '{b.id}' {why}, so it can't be in a combo")
            ok = False
        mask |= 1 << idx
    return mask if ok else None


def mode_use(v, dev, idx):
    """Why button idx can't have gestures or be in a combo: the way a mode uses it (or None). A mode's
    toggle, hold or cycle button has no action of its own, and a modifier or a `with:` button is held for
    the others' presses, which go to the mode at once (components/modes/README.md)."""
    if idx in v.mode_toggles:
        return f"switches mode '{v.mode_toggles[idx]}' and has no action of its own"
    if idx in v.mode_modifiers:
        return (f"is held for mode '{v.mode_modifiers[idx]}' (a modifier: its layer is \"Shift + a button\"), "
                "and a held modifier can't wait to be told apart")
    if idx in v.mode_with_buttons:
        return (f"is the with: button of mode '{v.mode_with_buttons[idx]}' (held for \"Shift + a button\"), "
                "and a held with: button can't wait to be told apart")
    return None


def held_action_problem(dev, idx, act):
    """Why button idx can't take action act from a preset (or None): act works for as long as the
    button is held (HELD_ACTIONS), and the button's presses wait for a gesture."""
    uses = [g + ":" for g in getattr(dev.buttons[idx], "gestures", {})]
    uses += ["a combo"] if any((c.mask >> idx) & 1 for c in getattr(dev, "combos", [])) else []
    if act not in HELD_ACTIONS or not uses:
        return None
    return (f"action {act} works for as long as the button is held, and button '{dev.buttons[idx].id}' has "
            f"{' and '.join(uses)}, so its presses wait to be told apart")


def gestures_used(dev):
    """The gestures the device's buttons have (README.md "Gestures"), each once."""
    return {g for b in dev.buttons for g in getattr(b, "gestures", {})}


def button_settings(v, dev, data):
    """button_settings: the gestures' timing (README.md "Gestures") -> dev.btn_set {key: ms}. A key for a
    gesture no button has is a warning: its param isn't there."""
    dev.btn_set = {k: x[0] for k, x in BUTTON_SETTINGS.items()}
    if data.get("button_settings") is None:
        return
    p, line = "button_settings", data.line_of("button_settings")
    s = v.mapping(data["button_settings"], p, line, BUTTON_SETTINGS_KEYS)
    if s is None:
        return
    used = (gestures_used(dev) | ({"combos"} if dev.combos else set()) |
            ({"repeat"} if repeat_mask(dev) else set()))
    for k, (default, lo, hi, gesture) in BUTTON_SETTINGS.items():
        ms = v.period(s, k, p, lo * 1000, hi * 1000, step_us=1000, example=f"{default}ms")
        if ms is None:
            continue
        dev.btn_set[k] = ms
        if gesture not in used:
            what = "there are no combos:" if gesture == "combos" else f"no button has {gesture}:"
            v.warn(f"{p}.{k}", s.line_of(k), f"{what}, so {k} does nothing (it isn't a param on this device)")


def gesture_mask(dev, gesture):
    """The buttons with a gesture, one bit each (gm_settings.h GM_BTN_HOLD, GM_BTN_DOUBLE)."""
    return sum(1 << i for i, b in enumerate(dev.buttons) if gesture in getattr(b, "gestures", {}))


def repeat_mask(dev):
    """The buttons with repeat: true, one bit each (gm_settings.h GM_BTN_REPEAT)."""
    return sum(1 << i for i, b in enumerate(dev.buttons) if getattr(b, "repeat", False))


def gesture_names(dev, mask):
    return ", ".join(dev.buttons[i].id for i in range(len(dev.buttons)) if (mask >> i) & 1)


def c_defines(dev):
    """GM_BTN_TWINS: the layer twins at the end of buttons[], when there are any; the gesture masks and
    their timing defaults (button_settings:), when a button has one."""
    twins = sum(1 for b in dev.buttons if getattr(b, "layer", 0))
    h = [f"#define GM_BTN_TWINS {twins} /* the last buttons: layer twins (layer.button.*) */"] if twins else []
    combos = getattr(dev, "combos", [])
    repeat = repeat_mask(dev)
    if not gestures_used(dev) and not combos and not repeat:
        return h
    s = getattr(dev, "btn_set", {})

    def default(setting):
        return (f"#define GM_BTN_{setting.upper()}_DEFAULT {s.get(setting, BUTTON_SETTINGS[setting][0])} "
                f"/* ms, button_settings.{setting} */")

    h.append("/* button gestures (components/buttons/README.md \"Gestures\"): one bit per button */")
    for gesture, define, setting in (("hold", "GM_BTN_HOLD", "hold_time"), ("double_tap", "GM_BTN_DOUBLE", "tap_time")):
        mask = gesture_mask(dev, gesture)
        if mask:
            h += [f"#define {define} 0x{mask:X}ull /* {gesture}: {c_comment(gesture_names(dev, mask))} */",
                  default(setting)]
    if combos:
        members = 0
        for c in combos:
            members |= c.mask
        h += [f"#define GM_N_COMBOS {len(combos)} /* combos: gm_btn_combos[] */",
              f"#define GM_COMBO_BUTTONS 0x{members:X}ull /* in a combo: {c_comment(gesture_names(dev, members))} */",
              f"#define GM_N_COMBO_BUTTONS {bin(members).count('1')}", default("combo_time")]
    if repeat:
        h += [f"#define GM_BTN_REPEAT 0x{repeat:X}ull /* repeat: {c_comment(gesture_names(dev, repeat))} */",
              default("repeat_delay"), default("repeat_rate")]
    return h


def c_param_defaults(dev):
    """gm_button_action_defaults[] and gm_button_number_defaults[] (core/src/settings.c), even without
    buttons."""
    acts = ", ".join(b.action_name for b in dev.buttons)
    return [c_param_array("gm_button_action_defaults", "GM_N_BUTTONS", [b.action for b in dev.buttons],
                          f" /* {acts} */" if acts else ""),
            c_param_array("gm_button_number_defaults", "GM_N_BUTTONS", [b.number for b in dev.buttons])]


def c_tables(dev):
    """gm_button_t buttons[] (gm_device.h), and gm_btn_gestures[] when a button has a gesture."""
    c = []
    if dev.buttons:
        c.append("static const gm_button_t buttons[GM_N_BUTTONS] = {")
        c += [f"    {{ {b.pin.c()}, {c_str(b.name)}, {b.led}{twin_c(b)} }}, "
              f"/* {c_comment(b.id)}: {c_comment(b.pin.desc)} */" for b in dev.buttons]
        c += ["};", ""]
    if getattr(dev, "combos", []):
        c += ["/* combos: { buttons (one bit each), { action, number } } */",
              "const gm_btn_combo_t gm_btn_combos[GM_N_COMBOS] = {",
              *[f"    {{ 0x{x.mask:X}ull, {{ {ACTION_IDS[x.action_name]}, {x.number} }} }}, "
                f"/* {c_comment(gesture_names(dev, x.mask).replace(', ', ' + '))}: {x.action_name} */"
                for x in dev.combos],
              "};", ""]
    if gestures_used(dev):
        c += ["/* button gestures: { hold, double_tap }, each { action, number }, GM_BTN_NO_ACT = none */",
              "const gm_btn_gesture_t gm_btn_gestures[GM_N_BUTTONS] = {",
              *[f"    {{ {', '.join(c_gesture_act(b, g) for g in GESTURES)} }}, /* {c_comment(b.id)} */"
                for b in dev.buttons],
              "};", ""]
    return c


def c_gesture_act(b, gesture):
    """A gm_btn_act_t initializer: the button's gesture's { action, number }, or none."""
    a = getattr(b, "gestures", {}).get(gesture)
    return f"{{ {ACTION_IDS[a[0]]}, {a[1]} }}" if a else "{ GM_BTN_NO_ACT, 0 }"
