"""modes: the modes (`modes:`), a control: key and button modes, hold modes, layers (a control's twins) and
cycles. modes.c runs them, this file validates their YAML and writes their C (README.md here). The builder
(gemifi_device.py) calls validate() after the controls and before the presets (presets.list), and
c_rgb_defines(), c_layer_defines(), c_defines() and c_tables() where gm_device_config.h / .c have them.
"""
import difflib
import re

from components.api import CURVES, PRESETS_OFF, WHITE, Hub, LDict, c_str, errors, with_article
from components.buttons import ACTION_IDS, button_action
from components.hub import c_comment
from components.leds import (LED_DEFAULT_LOOK, LED_LOOK_NAMES, LED_LOOKS, colour_exact, colour_look,
                             colour_look_needs, colour_mask, full_colour_hubs, led_full_colour, led_kind,
                             look_word, resolve_led_default)

# What a key can do in a mode: YAML action -> (gm_device.h GM_MK_* op, the key giving its number or None).
MODE_ACTIONS = {"preset_select": (1, "preset"), "preset_next": (2, None), "preset_prev": (3, None),
                "octave_down": (4, None), "octave_up": (5, None), "octave_reset": (6, None),
                "transpose_down": (7, None), "transpose_up": (8, None), "transpose_reset": (9, None),
                "channel": (10, "channel"), "velocity_curve": (11, "curve"),
                "channel_up": (13, None), "channel_down": (14, None)}
MK_MODE = 12                   # GM_MK_MODE: go to a sub-mode
MODE_KEYS = ["id", "toggle", "hold", "one_shot", "parent", "cycle", "led", "preset_blink", "keys", "buttons", "layer",
             "steps"]
# gm_mode_t.hold (gm_device.h): 1 a hold mode, GM_HOLD_ONE_SHOT a one-shot (components/modes/README.md "One-shot layers")
HOLD_ONE_SHOT = 2
STEP_KEYS = ["id", "name", "colour", "enabled", "layer"]     # cycle: steps (components/modes/README.md "Cycles")
MAX_STEPS = 8
MODE_NAME_LEN = 16             # a step's name: on the display when it is switched to
# modes[].layer.<button, pad or encoder id>: what its twin is (components/modes/README.md "Button layers")
LAYER_KEYS = ["name", "action", "cc", "note", "preset", "program", "mmc", "sysex", "daw", "idle", "active"]
LAYER_PAD_KEYS = ["name", "daw", "idle", "active"]
LAYER_ENCODER_KEYS = ["name", "cc", "daw"]
RANGE_KEYS = ("white_from", "from", "action", "count")      # the range form inside keys:
RANGE_ACTIONS = ("preset_select", "channel")
MAX_MODES = 64
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
NOTE_RE = re.compile(r"([A-Ga-g])([#b]?)(-?\d+)")

# gm_controls.h LV_STEP: a step's full colour goes in its LED's level as the mode index + 1, in 7 bits
MODE_RGB_MAX = 127


def in_device(dev):
    """Built when the device has modes (GM_N_MODES > 0)."""
    return bool(dev.modes)


def validate(v, dev, data):
    """modes: -> dev.modes, one Hub per mode: toggle (button index) or parent (mode index), root, led,
    blink, show (preset_blink LEDs) and keys = sorted [(key index, op, arg, comment)]."""
    if data.get("modes") is None:
        return
    lst = v.seq(data["modes"], "modes", data.line_of("modes"))
    if not lst:
        return
    if len(lst) > MAX_MODES:
        v.err("modes", data.line_of("modes"), f"{len(lst)} modes: at most {MAX_MODES}")
        return
    n_errors = len(errors(v.diags))
    # pass 1: ids, toggles and parents; a cycle: becomes its steps, one root mode each
    modes, by_id, cycles, seen = [], {}, [], {}

    def new_id(d, p, what):
        if "id" not in d:
            v.err(p, d.line, f"needs id: (e.g. `id: {what}`)")
            return None
        mid = d["id"]
        why = v.id_problem(mid, d.raw_of("id"))
        if why:
            v.err(f"{p}.id", d.line_of("id"), why)
            return None
        if mid in seen:
            v.err(f"{p}.id", d.line_of("id"), f"mode '{mid}' is already {seen[mid]}: mode ids (and cycle "
                                              "and step ids) are unique")
            return None
        seen[mid] = p
        return mid

    def new_mode(mid, p, d):
        m = Hub(id=mid, path=p, line=d.line, d=d, index=len(modes), toggle=None, hold=False, parent=None,
                root=None, led=None, blink=False, show=None, keys=[], buttons=[], with_=None, twins=0,
                cycle=0, colour=0, rgb=None, name=None, enabled=True, step=False, modifier=False,
                one_shot=False)
        modes.append(m)
        by_id[mid] = m
        return m

    for i, d in enumerate(lst):
        p = f"modes[{i}]"
        d = v.mapping(d, p, lst.line_of(i), MODE_KEYS)
        if d is None:
            continue
        mid = new_id(d, p, "edit; keys go to a sub-mode by its id")
        if mid is None:
            continue
        given = [k for k in ("toggle", "hold", "one_shot", "parent", "cycle") if d.get(k) is not None]
        if "cycle" in given or d.get("steps") is not None:
            mode_cycle(v, dev, d, p, mid, given, cycles, new_id, new_mode)
            continue
        m = new_mode(mid, p, d)
        if len(given) != 1:
            v.err(p, d.line, f"mode '{mid}' needs " + (
                f"one of toggle:, hold:, one_shot: or parent:, not {' and '.join(given)}" if given else
                "toggle: { button: <id> } (the button that enters and leaves it), hold: { button: <id> } "
                "(active while that button is held), one_shot: { button: <id> } (its layer for the next press), "
                "cycle: { button: <id> } with steps: (layers in turn) "
                "or parent: <mode id> (a sub-mode, entered from a key of another mode)"))
        elif given[0] != "parent":
            m.hold = given[0] in ("hold", "one_shot")
            m.one_shot = given[0] == "one_shot"
            mode_toggle(v, dev, m, d, p, given[0])
    dev.n_cycles = len(cycles)
    for m in modes:
        d = m.d
        if d.get("parent") is None or any(d.get(k) is not None for k in ("toggle", "hold", "one_shot")):
            continue
        pid, pl = d["parent"], d.line_of("parent")
        if pid not in by_id:
            close = difflib.get_close_matches(str(pid), list(by_id), 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            v.err(f"{m.path}.parent", pl, f"no mode with id '{d.raw_of('parent')}' (modes: "
                                          f"{', '.join(by_id)}{hint})")
        elif pid == m.id:
            v.err(f"{m.path}.parent", pl, "a mode can't be its own parent")
        else:
            m.parent = by_id[pid].index
    for m in modes:                  # the root: follow the parents (and catch loops)
        seen, x = [m.index], m
        while x.parent is not None and x.parent not in seen:
            x = modes[x.parent]
            seen.append(x.index)
        if x.parent is not None:
            v.err(f"{m.path}.parent", m.d.line_of("parent"), "the parents go round in a loop: " +
                  " -> ".join(modes[i].id for i in seen + [x.parent]))
        m.root = x.index
        if x.parent is None and x.hold and m is not x:
            what = "a one-shot mode (for one press)" if x.one_shot else "a hold mode (active while its button is held)"
            v.err(f"{m.path}.parent", m.d.line_of("parent"), f"mode '{x.id}' is {what}: it can't have sub-modes")
    # pass 2: LEDs, keys and buttons (a key may name a sub-mode further down the file)
    for m in modes:
        d, p = m.d, m.path
        if d.get("led") is not None:
            r = mode_led_ref(v, d, f"{p}.led")
            if r:
                m.led, m.blink = r
        if d.get("preset_blink") is not None:
            mode_show(v, dev, m, d, p)
        keys = d.get("keys")
        if d.get("buttons") is not None:
            mode_buttons(v, dev, m, d["buttons"], f"{p}.buttons", d.line_of("buttons"))
        if d.get("layer") is not None:
            mode_layer(v, dev, m, d["layer"], f"{p}.layer", d.line_of("layer"))
        if keys is None:
            if d.get("buttons") is None and d.get("layer") is None and not m.step:
                v.warn(p, d.line, f"mode '{m.id}' has no keys, buttons or layer: it does nothing")
        elif not isinstance(keys, dict):
            v.err(f"{p}.keys", d.line_of("keys"), "must be a mapping of keys to what they do, e.g. "
                                                   "`keys: { C3: octave_reset, E3: { mode: pick } }`")
        elif dev.matrix is None:
            v.err(f"{p}.keys", d.line_of("keys"), "keys: turns the keyboard's keys into function keys, and "
                                                   "this device has no key_matrix: bind buttons instead "
                                                   "(`buttons: { up: preset_next }`), or give the buttons a "
                                                   "layer:")
        else:
            mode_keys(v, dev, m, keys, f"{p}.keys", by_id)
    targets = {k[2] for m in modes for k in m.keys if k[1] == MK_MODE}
    for m in modes:
        if m.parent is not None and m.index not in targets:
            if dev.matrix is None:
                v.err(f"{m.path}.parent", m.d.line_of("parent"), f"mode '{m.id}' is a sub-mode, entered from a "
                      "key of its parent's keys:, and this device has no key_matrix: make it a root mode "
                      "(toggle:, hold: or cycle:)")
            else:
                v.err(m.path, m.line, f"mode '{m.id}' can't be reached: no key goes to it (add e.g. "
                                      f"`E3: {{ mode: {m.id} }}` to the keys of mode '{modes[m.parent].id}')")
    if len(errors(v.diags)) > n_errors:
        return
    # defaults: a root lights its toggle button's LED, a sub-mode blinks its parent's; preset_blink is
    # inherited from the parent
    for m in modes:
        x = m
        while m.parent is not None and x.parent is not None and (m.led is None or m.show is None):
            x = modes[x.parent]
            if m.led is None and x.led is not None:
                m.led, m.blink = x.led, True
            if m.show is None:
                m.show = x.show
        root = modes[m.root]
        if m.led is None and root.toggle is not None and dev.buttons[root.toggle].led >= 0:
            m.led, m.blink = dev.buttons[root.toggle].led, m is not root
        m.keys.sort()
        m.buttons.sort()
    dev.modes = modes


def note_text(n):
    """MIDI note -> name, C4 = 60 (scientific pitch)."""
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def parse_note(x):
    """`E3`, `F#3`, `Db4` (C4 = 60) or a MIDI note number -> note, or None."""
    if isinstance(x, int) and not isinstance(x, bool):
        return x if 0 <= x <= 127 else None
    m = NOTE_RE.fullmatch(x.strip()) if isinstance(x, str) else None
    if not m:
        return None
    n = (int(m.group(3)) + 1) * 12 + [0, 2, 4, 5, 7, 9, 11]["CDEFGAB".index(m.group(1).upper())] + \
        {"#": 1, "b": -1, "": 0}[m.group(2)]
    return n if 0 <= n <= 127 else None


def mode_key_index(v, dev, x, path, line):
    """A key named in a mode (its note at octave 0, no transpose: the physical key) -> its matrix index."""
    m = dev.matrix
    lo, hi = m.first_note, m.first_note + m.keys - 1
    n = parse_note(x)
    if n is None:
        v.err(path, line, f"'{x}' isn't a key: name it by its note at octave 0 (C4 = 60), e.g. E3, F#3, Db4, "
                          f"or give the MIDI note number (this keyboard: {note_text(lo)}..{note_text(hi)})")
        return None
    if not lo <= n <= hi:
        v.err(path, line, f"{note_text(n)} (note {n}) isn't on the keyboard: its keys are "
                          f"{note_text(lo)}..{note_text(hi)} (notes {lo}..{hi} at octave 0, from "
                          "key_matrix.first_note)")
        return None
    return n - lo


def mode_action(v, dev, name, d, path, line, arg=None):
    """An action name (+ its number from d, or arg for a key of a range) -> (op, arg, text), or None."""
    if name not in MODE_ACTIONS:
        close = difflib.get_close_matches(str(name), list(MODE_ACTIONS), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(path, line, f"unknown action '{name}' ({hint}valid in a mode: {', '.join(MODE_ACTIONS)}; or "
                          "`{ mode: <id> }` to go to a sub-mode)")
        return None
    op, argkey = MODE_ACTIONS[name]
    if name.startswith("preset") and not dev.has_presets:
        v.err(path, line, f"action {name} switches presets, but {PRESETS_OFF}")
        return None
    if name == "velocity_curve" and not (dev.matrix is not None and dev.matrix.kind == "velocity"):
        v.err(path, line, "velocity_curve needs a velocity key matrix (key_matrix: { type: velocity })")
        return None
    extra = [k for k in (d or {}) if k not in ("action", argkey)]
    if extra:
        v.err(f"{path}.{extra[0]}", d.line_of(extra[0]), f"action {name} takes " +
              (f"{argkey}:" if argkey else "nothing else") + f", not {extra[0]}:")
        return None
    if argkey is None:
        return op, 0, name
    if arg is not None:
        return op, arg, f"{name} {arg}"
    if d is None or argkey not in d:
        need = {"preset": f"preset: (the slot, 1..{dev.n_presets})", "channel": "channel: (1..16)",
                "curve": f"curve: ({' | '.join(CURVES)})"}[argkey]
        example = {"preset": 2, "channel": 10, "curve": "hard"}[argkey]
        v.err(path, line, f"action {name} needs {need}, e.g. `{{ action: {name}, {argkey}: {example} }}`")
        return None
    if argkey == "curve":
        x = v.enum_value(d, "curve", f"{path}.curve", CURVES)
        return None if x is None else (op, x, f"{name} {CURVES[x]}")
    x = v.integer(d, argkey, path, 1, dev.n_presets if argkey == "preset" else 16)
    return None if x is None else (op, x, f"{name} {x}")


def mode_led_ref(v, d, path):
    """A mode's led: an LED id (lit while the mode is active) or { <LED id>: blink } -> (index, blink)."""
    x, line = d["led"], d.line_of("led")
    if isinstance(x, dict):
        if len(x) != 1:
            v.err(path, line, "must be an LED id (lit while the mode is active) or `{ <LED id>: blink }`")
            return None
        lid, how = next(iter(x.items()))
        if how not in ("blink", True):
            v.err(f"{path}.{lid}", x.line_of(lid), f"must be blink or on (got {x.raw_of(lid)!r})")
            return None
        holder = LDict(led=lid)
        holder.line = line
        idx = v.ref(holder, "led", path, line, "LED", "LEDs")
        return None if idx is None else (idx, how == "blink")
    idx = v.ref(d, "led", path, line, "LED", "LEDs")
    return None if idx is None else (idx, False)


def mode_cycle(v, dev, d, p, cid, given, cycles, new_id, new_mode):
    """cycle: { button, with } + steps: (components/modes/README.md "Cycles"): each press moves to the next enabled
    step. Each step is a root mode of its own (its own index, layer and LED colour), all with the cycle's
    button as toggle / with and cycle = the group number + 1."""
    if given != ["cycle"] or d.get("steps") is None:
        others = [k for k in given if k != "cycle"]
        v.err(p, d.line, f"cycle '{cid}' needs " + (
            f"cycle: and steps: only, not {' and '.join(others)}:" if others else
            "cycle: { button: <id>, with: <id> } (the button that moves to the next step)" if "cycle" not in given
            else "steps: (2 to 8 of { id, name, colour, enabled, layer })"))
        return
    for k in ("hold", "parent", "keys", "buttons", "layer", "preset_blink"):
        if d.get(k) is not None:
            v.err(f"{p}.{k}", d.line_of(k), f"a cycle has no {k}: of its own: give each step its layer:")
            return
    cyc = Hub(id=cid, path=p, toggle=None, with_=None)
    mode_toggle(v, dev, cyc, d, p, "cycle")
    led = None
    if d.get("led") is not None:
        r = mode_led_ref(v, d, f"{p}.led")
        if r:
            led = r[0]
    elif cyc.toggle is not None and dev.buttons[cyc.toggle].led >= 0:
        led = dev.buttons[cyc.toggle].led
    steps, sl = d["steps"], d.line_of("steps")
    lst = v.seq(steps, f"{p}.steps", sl)
    if isinstance(steps, list) and not 2 <= len(lst) <= MAX_STEPS:
        v.err(f"{p}.steps", sl, f"{len(lst)} step{'s' if len(lst) != 1 else ''}: a cycle has 2 to {MAX_STEPS}")
        return
    group = len(cycles) + 1
    out = []
    for j, s in enumerate(lst):
        sp = f"{p}.steps[{j}]"
        s = v.mapping(s, sp, lst.line_of(j), STEP_KEYS)
        if s is None:
            continue
        sid = new_id(s, sp, "enc_midi")
        if sid is None:
            continue
        m = new_mode(sid, sp, s)
        m.step, m.cycle, m.root = True, group, m.index
        m.toggle, m.with_, m.led = cyc.toggle, cyc.with_, led
        m.enabled = v.boolean(s, "enabled", sp, default=True)
        name = v.string(s, "name", sp)
        if name is not None and (len(name) > MODE_NAME_LEN or any(not 32 <= ord(c) <= 126 for c in name)):
            v.err(f"{sp}.name", s.line_of("name"), f"'{name}': at most {MODE_NAME_LEN} ASCII characters (the display "
                                                   "shows as much as fits)")
        m.name = name
        if s.get("colour") is not None:
            m.colour, m.rgb = step_colour(v, dev, s, sp, led)
            if m.rgb is not None and m.index >= MODE_RGB_MAX:
                v.warn(f"{sp}.colour", s.line_of("colour"), f"mode {m.index}: only the first {MODE_RGB_MAX} modes "
                                                            "show a step's full colour; this one shows the nearest "
                                                            "on/off colour")
                m.rgb = None
        out.append(m)
    if out and not any(m.enabled for m in out):
        v.err(f"{p}.steps", sl, f"cycle '{cid}': every step has enabled: false; at least one must be enabled")
    cycles.append(cyc)


def step_colour(v, dev, s, sp, led):
    """A step's colour: an LED look for the cycle's LED, resolved for its kind like led_defaults ->
    (0x80 | pin mask (gm_mode_t.colour), 0 after an error; the full colour (gm_mode_rgb[]) or None). A full
    colour ("#rrggbb", "blue 20%") shows as it is on an LED that can show any colour, elsewhere as the
    nearest on/off colour."""
    cp, cl = f"{sp}.colour", s.line_of("colour")
    if led is None:
        v.err(cp, cl, "the cycle has no LED to show it: add led: <LED id> to the cycle (or give its button an LED)")
        return 0, None
    w = look_word(s, "colour")
    x = dev.leds[led]
    kind = led_kind(x.pin2, x.pin3)
    full = led_full_colour(dev, x.pin, x.pin2, x.pin3)
    col = colour_look(w) if w not in LED_LOOKS[kind] and w not in LED_LOOK_NAMES else None
    if w not in LED_LOOKS[kind] and w not in LED_LOOK_NAMES and col is None:
        close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
        hint = f"did you mean '{close[0]}'? " if close else ""
        v.err(cp, cl, f"unknown colour '{w}' for LED '{x.id}' ({hint}valid: {', '.join(LED_LOOKS[kind])}, "
                      "or a full colour: \"#rrggbb\", blue 20%)")
        return 0, None
    if col is not None and not (full and kind == "rgb") and not colour_exact(kind, col):
        v.warn(cp, cl, f"LED '{x.id}' ({kind}, on/off) can't show {w}: it shows the nearest it can (full colours "
                       f"need an LED that can show any colour: one LED of an LED strip, led_strip: {' / '.join(full_colour_hubs())})")
    elif col is None and w not in LED_LOOKS[kind]:
        v.warn(cp, cl, f"LED '{x.id}' ({kind}) can't show {w}: it shows the nearest it can (valid: "
                       f"{', '.join(LED_LOOKS[kind])})")
    mask, rgb = resolve_led_default(kind, w, full)
    return 0x80 | mask, rgb


def mode_toggle(v, dev, m, d, p, key="toggle"):
    """toggle: { button: <id> } (or toggle: <id>): the button that enters and leaves a root mode;
    hold: the same form, the button that keeps a hold mode active while it is held; cycle: the same as
    toggle:, the button that moves a cycle to its next step."""
    t, tl = d[key], d.line_of(key)
    if isinstance(t, dict):
        if v.mapping(t, f"{p}.{key}", tl, ["button", "with"] if key in ("toggle", "cycle") else ["button"]) is None:
            return
        if "button" not in t:
            v.err(f"{p}.{key}", tl, f"needs button: (e.g. `{key}: {{ button: edit }}`)")
            return
        holder = t
    else:
        holder = LDict(button=t)
        holder.line = tl
    idx = v.ref(holder, "button", f"{p}.{key}.button", holder.line_of("button"), "button", "buttons")
    if idx is None:
        return
    b = dev.buttons[idx]
    verb = {"toggle": "toggles", "hold": "holds", "one_shot": "arms", "cycle": "cycles"}[key]
    if holder.get("with") is not None:
        # toggle: { button, with }: the mode only with `with` held; a plain press keeps the button's action
        w = v.ref(holder, "with", f"{p}.{key}.with", holder.line_of("with"), "button", "buttons")
        if w is None:
            return
        if w == idx:
            v.err(f"{p}.{key}.with", holder.line_of("with"), "the button can't be held with itself")
        elif idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers:
            v.err(f"{p}.{key}", tl, f"button '{b.id}' already enters mode "
                                    f"'{v.mode_toggles.get(idx) or v.mode_with_toggles.get(idx) or v.mode_modifiers[idx]}'")
        elif w in v.mode_toggles:
            v.err(f"{p}.{key}.with", holder.line_of("with"), f"button '{dev.buttons[w].id}' enters mode "
                                                             f"'{v.mode_toggles[w]}' itself")
        else:
            v.mode_with_toggles[idx] = m.id
            v.mode_with_of[idx] = w
            v.mode_with_buttons.setdefault(w, m.id)
            m.toggle, m.with_ = idx, w
        return
    if idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers:
        v.err(f"{p}.{key}", tl, f"button '{b.id}' already enters mode "
                                f"'{v.mode_toggles.get(idx) or v.mode_with_toggles.get(idx) or v.mode_modifiers[idx]}'")
    elif key == "one_shot" and (d.get("layer") is None or d.get("keys") is not None or d.get("buttons") is not None):
        v.err(f"{p}.{key}", tl, "a one-shot mode has a layer: and nothing else (no keys: or buttons:): the layer's "
                                "twins take the next press (components/modes/README.md \"One-shot layers\")")
    elif key == "hold" and d.get("layer") is not None and d.get("keys") is None and d.get("buttons") is None:
        # a hold mode with only a layer: is a modifier (Shift): its button keeps its own action, and
        # while it is held the layer's twins take their buttons' presses (components/modes/README.md "Button layers")
        v.mode_modifiers[idx] = m.id
        m.toggle, m.modifier = idx, True
    elif b.action_name != "none":
        v.err(f"{p}.{key}", tl, f"button '{b.id}' {verb} this mode, so its own action would never run: remove "
                                f"`action: {b.action_name}` (and its cc:/note:) from the button")
    else:
        v.mode_toggles[idx] = m.id
        m.toggle = idx


def mode_buttons(v, dev, m, btns, bp, bl):
    """buttons: { <button id>: <action> }: what buttons do instead of their own action while the mode is
    active (the actions of keys:, but no sub-modes or ranges) -> m.buttons."""
    if not isinstance(btns, dict) or not btns:
        v.err(bp, bl, "must be a mapping of button ids to what they do in the mode, e.g. "
                      "`buttons: { up: channel_up, down: channel_down }`")
        return
    for bid in btns:
        path, line = f"{bp}.{bid}", btns.line_of(bid)
        holder = LDict(button=bid)
        holder.line = line
        idx = v.ref(holder, "button", path, line, "button", "buttons")
        if idx is None:
            continue
        if idx in v.mode_toggles or idx in v.mode_with_toggles or idx in v.mode_modifiers or idx == m.toggle:
            v.err(path, line, f"button '{bid}' enters a mode (toggle:/hold:): it can't also be bound in one")
            continue
        b = btns[bid]
        r = None
        if isinstance(b, str):
            r = mode_action(v, dev, b, None, path, line)
        elif isinstance(b, dict) and "action" in b:
            r = mode_action(v, dev, b["action"], b, path, line)
        else:
            v.err(path, line, "must be an action (`channel_up`, `preset_next`, `{ action: channel, channel: 10 }`, "
                              "...); buttons can't go to a sub-mode")
        if r:
            m.buttons.append((idx, r[0], r[1], f"{dev.buttons[idx].id}: {r[2]}"))


def mode_layer(v, dev, m, layer, lp, ll):
    """layer: { <button, pad or encoder id>: { ... } } (components/modes/README.md "Button layers"): while the mode is
    active those are their twins. A button twin (action/cc/note/preset, daw, name, idle, active) is appended
    to dev.buttons, a pad twin (daw, name, idle, active) to dev.pad_twins, an encoder twin (cc, daw, name) to
    dev.encoders; a button's or pad's LED gets a twin LED in dev.leds: its pins with the twin's looks.
    What a twin leaves out is its base's."""
    if m.parent is not None:
        v.err(lp, ll, "only a root mode (toggle: or hold:) has a layer: a sub-mode lasts one action")
        return
    if not isinstance(layer, dict) or not layer:
        v.err(lp, ll, "must be a mapping of button, pad or encoder ids to what they are in the layer, e.g. "
                      "`layer: { b2: { cc: 52, daw: arm_1, active: red } }`")
        return
    bound = {k[0] for k in m.buttons}
    for bid in layer:
        path, line = f"{lp}.{bid}", layer.line_of(bid)
        target = v.ids.get(bid) if isinstance(bid, str) else None
        kind = target[0] if target else None
        if kind in ("pad twin",) or (kind in ("button", "encoder") and
                                     getattr((dev.buttons if kind == "button" else dev.encoders)[target[3]],
                                             "layer", 0)):
            v.err(path, line, f"'{bid}' is a layer twin: give the layer the {kind.split()[0]} itself")
            continue
        if kind not in ("button", "pad", "encoder"):
            known = [i for i, t in v.ids.items() if t[0] in ("button", "pad", "encoder")]
            close = difflib.get_close_matches(str(bid), known, 1)
            hint = f"; did you mean '{close[0]}'?" if close else ""
            what = f"is the id of {target[1]} ({with_article(kind)})" if target else "matches nothing"
            v.err(path, line, f"no button, pad or encoder with id '{bid}': it {what}{hint}")
            continue
        idx = target[3]
        e = layer[bid]
        if e is None:
            e = LDict()
            e.line = line
        e = v.mapping(e, path, line, {"button": LAYER_KEYS, "pad": LAYER_PAD_KEYS,
                                      "encoder": LAYER_ENCODER_KEYS}[kind],
                      renamed={"acceleration": "acceleration: is the physical encoder's, and its twins share it: "
                                               "set it on the encoder"} if kind == "encoder" else None)
        if e is None:
            continue
        if kind == "pad":
            layer_pad(v, dev, m, idx, e, path, line)
        elif kind == "encoder":
            layer_encoder(v, dev, m, idx, e, path, line)
        else:
            b = dev.buttons[idx]
            if (idx == m.with_ or idx in v.mode_toggles or idx in v.mode_with_buttons or
                    idx in v.mode_modifiers or (idx == m.toggle and m.with_ is None)):
                v.err(path, line, f"button '{bid}' enters a mode (or is held for one): it can't have a twin")
                continue
            if m.modifier and v.mode_with_of.get(idx) == m.toggle:
                v.err(path, line, f"{dev.buttons[m.toggle].id} + {bid} already toggles mode "
                                  f"'{v.mode_with_toggles[idx]}', so this twin's action would never run")
                continue
            if idx in bound:
                v.err(path, line, f"button '{bid}' is also in this mode's buttons:; give it one of the two")
                continue
            act, num = button_action(v, dev, e, path, base=(b.action_name, b.number))
            tid = v.register(f"{b.id}_{m.id}", path, line, "button", len(dev.buttons))
            if tid is None:
                continue
            led = layer_led(v, dev, m, b.led, e, path, line, ("button", act), f"button '{bid}'")
            if led is None:
                continue
            name = v.string(e, "name", path)
            dev.buttons.append(Hub(id=tid, name=name if name is not None else f"{b.name} {m.id}", pin=b.pin,
                                   action=ACTION_IDS[act], action_name=act, number=num, led=led,
                                   daw=v.daw_role(dev, e, path), layer=m.index + 1, base=idx,
                                   ctl=b.ctl, bank=b.bank))
        m.twins += 1


def layer_led(v, dev, m, base_led, e, path, line, use, what):
    """The twin LED of LED base_led (-1: none) for a layer twin -> its index, -1 for none, None after an
    error."""
    if base_led < 0:
        if e.get("idle") is not None or e.get("active") is not None:
            v.warn(path, line, f"{what} has no LED (led:): idle: / active: do nothing")
        return -1
    bl = dev.leds[base_led]
    led = len(dev.leds)
    if led > 127:
        v.err(path, line, "LED index over 127 can't be referenced: too many LEDs for another twin")
        return None
    looks, rgb = layer_led_looks(v, e, path, led_kind(bl.pin2, bl.pin3), bl.looks, getattr(bl, "rgb", None),
                                 led_full_colour(dev, bl.pin, bl.pin2, bl.pin3))
    lid = v.register(f"{bl.id}_{m.id}", path, line, "LED", led) or f"{bl.id}_{m.id}"
    dev.leds.append(Hub(id=lid, name=f"{bl.name} {m.id}", pin=bl.pin, pin2=bl.pin2, pin3=bl.pin3,
                        looks=looks, rgb=rgb, source=None, owner=bl.owner, timeout=bl.timeout, path=path,
                        line=line, used_by=[use], daw=None, local_line=None,
                        layer=m.index + 1, base=base_led))
    return led


def layer_pad(v, dev, m, idx, e, path, line):
    """A pad's twin: hits go to it (an EVENT, no note) while the layer is active."""
    x = dev.pads[idx]
    tid = v.register(f"{x.id}_{m.id}", path, line, "pad twin", len(dev.pad_twins))
    if tid is None:
        return
    led = layer_led(v, dev, m, x.led, e, path, line, ("pad", None), f"pad '{x.id}'")
    if led is None:
        return
    name = v.string(e, "name", path)
    dev.pad_twins.append(Hub(id=tid, name=name if name is not None else f"{x.name} {m.id}", led=led,
                             daw=v.daw_role(dev, e, path), layer=m.index + 1, base=idx))


def layer_encoder(v, dev, m, idx, e, path, line):
    """An encoder's twin: turns go to it (its CC, its EVENT index) while the layer is active. Same pins, not
    scanned again; layer.encoder.mode starts as absolute. cc: none sends no CC, only EVENTs."""
    x = dev.encoders[idx]
    if x.ring or x.ring_center is not None:
        v.err(path, line, "a ring encoder can't have a layer twin yet")
        return
    if isinstance(e.get("cc"), str) and e["cc"].strip().lower() == "none":
        cc = -1                  # cc: none: only EVENTs (layer.encoder.cc -1)
    else:
        cc = v.integer(e, "cc", path, 0, 127, default=x.cc)
    tid = v.register(f"{x.id}_{m.id}", path, line, "encoder", len(dev.encoders))
    if tid is None:
        return
    name = v.string(e, "name", path)
    dev.encoders.append(Hub(id=tid, name=name if name is not None else f"{x.name} {m.id}", a=x.a, b=x.b,
                            cc=cc, steps=x.steps, mcu=None, ring=[], ring_center=None,
                            daw=v.daw_role(dev, e, path), layer=m.index + 1, base=idx, mode=0, ctl=x.ctl))


def layer_led_looks(v, e, p, kind, base_looks, base_rgb=None, full=False):
    """A twin LED's ((idle, active), full colours): the button LED's, with the twin's idle: / active: in
    their place."""
    looks = list(base_looks or LED_DEFAULT_LOOK[kind])
    rgb = list(base_rgb or (None, None))
    given = False
    for k, key in enumerate(("idle", "active")):
        if e.get(key) is None:
            continue
        w = look_word(e, key)
        col = colour_look(w) if kind == "rgb" and w not in LED_LOOKS[kind] else None
        if col is not None:
            if not full:
                v.err(f"{p}.{key}", e.line_of(key), f"'{w}' {colour_look_needs()}")
                continue
            looks[k], rgb[k] = colour_mask(col), col
            given = True
            continue
        if w not in LED_LOOKS[kind]:
            close = difflib.get_close_matches(w, list(LED_LOOKS[kind]), 1)
            hint = f"did you mean '{close[0]}'? " if close else ""
            v.err(f"{p}.{key}", e.line_of(key), f"unknown look '{w}' for this LED ({hint}valid: "
                                                f"{', '.join(LED_LOOKS[kind])})")
            continue
        looks[k], rgb[k] = LED_LOOKS[kind][w], None
        given = True
    if not given:
        return base_looks, base_rgb
    if looks[0] == looks[1] and rgb[0] == rgb[1]:
        v.warn(p, e.line, "idle and active look the same: the LED never shows its state")
    return tuple(looks), (tuple(rgb) if any(c is not None for c in rgb) else None)


def mode_show(v, dev, m, d, p):
    """preset_blink: [count LED, done LED]: after a preset switch from the mode, the first blinks the
    slot number, then both flash once."""
    pb, pbl = d["preset_blink"], d.line_of("preset_blink")
    items = v.seq(pb, f"{p}.preset_blink", pbl)
    if not 1 <= len(items) <= 2:
        if isinstance(pb, list):
            v.err(f"{p}.preset_blink", pbl, "needs one or two LED ids: the first blinks the preset number, then "
                                            "both flash once (e.g. [oct_down_led, oct_up_led])")
        return
    idx = [v.ref(items, k, f"{p}.preset_blink[{k}]", items.line_of(k), "LED", "LEDs") for k in range(len(items))]
    if None in idx:
        return
    m.show = (idx[0], idx[-1] if len(idx) > 1 else -1)
    if not dev.has_presets:
        v.warn(f"{p}.preset_blink", pbl, f"only shown after a preset switch, and {PRESETS_OFF}")


def mode_keys(v, dev, m, keys, kp, by_id):
    """The keys: mapping of one mode -> m.keys."""
    first, top = dev.matrix.first_note, dev.matrix.first_note + dev.matrix.keys - 1
    bound = {}

    def bind(k, op, arg, text, line):
        name = note_text(first + k)
        if k in bound:
            v.err(kp, line, f"{name} is bound twice in this mode ({bound[k]}, and {text})")
            return
        bound[k] = f"{text} at line {line}"
        m.keys.append((k, op, arg, f"{name}: {text}"))

    if any(k in keys for k in RANGE_KEYS):
        mode_range(v, dev, keys, kp, bind, first, top)
    for name in keys:
        if name in RANGE_KEYS:
            continue
        path, line = f"{kp}.{name}", keys.line_of(name)
        k = mode_key_index(v, dev, name, path, line)
        if k is None:
            continue
        b = keys[name]
        r = None
        if isinstance(b, str):
            r = mode_action(v, dev, b, None, path, line)
        elif isinstance(b, dict) and "mode" in b:
            target = by_id.get(b["mode"])
            extra = [x for x in b if x != "mode"]
            if extra:
                v.err(f"{path}.{extra[0]}", b.line_of(extra[0]), "a key that goes to a sub-mode does nothing "
                                                                 "else (mode: only)")
            elif target is None:
                close = difflib.get_close_matches(str(b["mode"]), list(by_id), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                v.err(f"{path}.mode", b.line_of("mode"), f"no mode with id '{b.raw_of('mode')}' (modes: "
                                                         f"{', '.join(by_id)}{hint})")
            elif target.parent is None:
                v.err(f"{path}.mode", b.line_of("mode"), f"mode '{target.id}' has no parent: a key can only go to "
                                                         "a sub-mode (one with parent:)")
            else:
                r = (MK_MODE, target.index, f"mode {target.id}")
        elif isinstance(b, dict) and "action" in b:
            r = mode_action(v, dev, b["action"], b, path, line)
        else:
            v.err(path, line, "must be an action (`octave_reset`, `{ action: preset_select, preset: 2 }`, "
                              "`{ action: channel, channel: 10 }`, ...) or `{ mode: <sub-mode id> }`")
        if r:
            bind(k, *r, line)


def mode_range(v, dev, keys, kp, bind, first, top):
    """keys: { white_from: D4 | from: C4, action: preset_select | channel, count: N }: consecutive (white)
    keys numbered 1..count (default: every slot / 16 channels)."""
    start = [k for k in ("white_from", "from") if k in keys]
    if len(start) != 1 or "action" not in keys:
        v.err(kp, keys.line, "a range of keys needs white_from: (white keys) or from: (every key), and "
                             "action: (preset_select | channel), e.g. `{ white_from: D4, action: preset_select }`")
        return
    act, sk = keys["action"], start[0]
    if act not in RANGE_ACTIONS:
        v.err(f"{kp}.action", keys.line_of("action"), f"a range numbers its keys 1, 2, ...: action is one of "
                                                      f"{', '.join(RANGE_ACTIONS)} (got {keys.raw_of('action')!r})")
        return
    if mode_action(v, dev, act, None, f"{kp}.action", keys.line_of("action"), arg=1) is None:
        return
    count = v.integer(keys, "count", kp, 1, dev.n_presets if act == "preset_select" else 16,
                      default=dev.n_presets if act == "preset_select" else 16)
    k0 = mode_key_index(v, dev, keys[sk], f"{kp}.{sk}", keys.line_of(sk))
    if count is None or k0 is None:
        return
    white = sk == "white_from"
    if white and (first + k0) % 12 not in WHITE:
        v.err(f"{kp}.{sk}", keys.line_of(sk), f"{note_text(first + k0)} is a black key")
        return
    ks = [n - first for n in range(first + k0, top + 1) if not white or n % 12 in WHITE][:count]
    if len(ks) < count:
        v.err(f"{kp}.{sk}", keys.line_of(sk), f"{count} {'white ' if white else ''}keys from "
                                              f"{note_text(first + k0)} go past the top key ({note_text(top)}): "
                                              f"{len(ks)} fit (set count: {len(ks)}, or start lower)")
        return
    op = MODE_ACTIONS[act][0]
    for j, k in enumerate(ks, 1):
        bind(k, op, j, f"{act} {j}", keys.line_of(sk))


def mode_rgb(dev):
    """gm_mode_rgb[] (GM_MODE_RGB): each mode's full step colour, None when no step has one."""
    rgb = [getattr(m, "rgb", None) for m in dev.modes]
    return rgb if any(c is not None for c in rgb) else None


def c_rgb_defines(dev):
    """GM_MODE_RGB 1 when a cycle step has a full colour (gm_mode_rgb[])."""
    return ["#define GM_MODE_RGB 1 /* a cycle step has a full colour: gm_mode_rgb[] */"] if mode_rgb(dev) else []


def c_layer_defines(dev):
    """GM_LAYERS: the layer twins of the buttons, pads and encoders together, when there are any."""
    twins = (sum(1 for b in dev.buttons if getattr(b, "layer", 0)) + len(dev.pad_twins) +
             sum(1 for e in dev.encoders if getattr(e, "layer", 0)))
    if not twins:
        return []
    return [f"#define GM_LAYERS {twins} /* layer twins (modes: layer:, components/modes/README.md \"Button layers\") */"]


def c_defines(dev):
    """GM_MODES_ENABLED (each mode's layer.enabled default), GM_N_CYCLES (the cycle: groups) and
    GM_MODE_ONE_SHOT (a one_shot: mode), when there are any."""
    h = []
    if dev.modes:
        h.append(f"#define GM_MODES_ENABLED {', '.join(str(int(m.enabled)) for m in dev.modes)} "
                 "/* layer.enabled defaults, per mode */")
    if getattr(dev, "n_cycles", 0):
        h.append(f"#define GM_N_CYCLES {dev.n_cycles} /* modes: cycle: groups (components/modes/README.md \"Cycles\") */")
    if any(getattr(m, "one_shot", False) for m in dev.modes):
        h.append("#define GM_MODE_ONE_SHOT 1 /* a one_shot: mode (components/modes/README.md \"One-shot layers\") */")
    return h


def c_tables(dev):
    """gm_modes[] (gm_device.h gm_mode_t), with each mode's key bindings sorted by key; gm_mode_rgb[] when a
    step has a full colour."""
    if not dev.modes:
        return []
    c = ["/* modes: key bindings { key index, op (gm_device.h GM_MK_*), arg } */"]
    rows = []
    for m in dev.modes:
        arr = "0"
        if m.keys:
            arr = f"mode_{m.index + 1}_keys"
            c.append(f"static const gm_mode_key_t {arr}[{len(m.keys)}] = {{ /* {c_comment(m.id)} */")
            c += [f"    {{ {k}, {op}, {arg} }}, /* {c_comment(text)} */" for k, op, arg, text in m.keys]
            c.append("};")
        barr = "0"
        if m.buttons:
            barr = f"mode_{m.index + 1}_buttons"
            c.append(f"static const gm_mode_key_t {barr}[{len(m.buttons)}] = {{ /* {c_comment(m.id)}: buttons */")
            c += [f"    {{ {k}, {op}, {arg} }}, /* {c_comment(text)} */" for k, op, arg, text in m.buttons]
            c.append("};")
        show = m.show or (-1, -1)
        toggle = "GM_MODE_NONE" if m.toggle is None else m.toggle
        parent = "GM_MODE_NONE" if m.parent is None else m.parent
        hold = HOLD_ONE_SHOT if m.one_shot else int(m.hold)
        rows.append(f"    {{ {toggle}, {parent}, {m.root}, {-1 if m.led is None else m.led}, {int(m.blink)}, "
                    f"{show[0]}, {show[1]}, {len(m.keys)}, {arr}, {hold}, {len(m.buttons)}, {barr}"
                    f", {0 if m.with_ is None else m.with_ + 1}, {m.cycle}, 0x{m.colour:02X}, "
                    f"{c_str(m.name if m.name is not None else m.id)}, {int(m.modifier)} }}, "
                    f"/* {c_comment(m.id)} */")
    c += ["/* { toggle/hold/cycle button, parent, root, led, led_blink, blink_count, blink_done, n_keys, keys, "
          "hold, n_buttons, buttons, with, cycle, colour, name, modifier } */",
          "const gm_mode_t gm_modes[GM_N_MODES] = {", *rows, "};", ""]
    rgb = mode_rgb(dev)
    if rgb:
        c += ["/* the steps' full colours (cycle step colour: \"#rrggbb\" on an LED that can show any colour), "
              "0 = gm_modes[].colour */",
              "const uint32_t gm_mode_rgb[GM_N_MODES] = {",
              *[f"    {'0' if x is None else f'GM_LED_RGB(0x{x:06X})'}, /* {c_comment(m.id)} */"
                for m, x in zip(dev.modes, rgb)], "};", ""]
    return c
