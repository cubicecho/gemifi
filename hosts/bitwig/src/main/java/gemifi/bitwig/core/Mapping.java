// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import gemifi.bitwig.core.Model.MasterParam;
import gemifi.bitwig.core.Model.Param;
import gemifi.bitwig.core.Model.Strip;
import gemifi.bitwig.core.Model.TrackFlag;
import gemifi.bitwig.core.Model.TrackParam;
import gemifi.bitwig.core.Model.TransportFlag;
import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Event;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeMap;
import java.util.function.Consumer;
import java.util.function.IntSupplier;
import java.util.function.Supplier;

/**
 * Roles → behaviour ({@code hosts/README.md}, "Roles"). Pure Java, no Bitwig imports; the design
 * follows {@code hosts/ableton-live/gemifi/mapping.py}.
 *
 * <p>Inputs: EVENTs from the device, by component index. Outputs: FB_* notifications through
 * {@link Notifier}. State is compared, not pushed: {@link #refresh()} (called from the extension's
 * tick) and {@link #flush()} (from Bitwig's {@code flush()}) read every bound state, compare it with
 * what was last sent and send only the differences. A lost notification is fixed by the next change
 * (§4.6.2 says feedback is idempotent).
 */
public final class Mapping {
    public static final int STRIPS = 8;
    /** This extension's {@code <host>.<command>} namespace (no commands yet); other hosts' are ignored. */
    public static final String HOST_NAMESPACE = "bitwig";
    public static final int V14 = Protocol.U14_MAX;
    /** FB_LED levels (§4.6.2: 0 off, 1 blink, 2..127 on). */
    public static final int ON = 127, OFF = 0, BLINK = 1;
    /** Encoder detents for a parameter's full range. */
    public static final int ENCODER_STEPS = 128;
    /** An analog input within 3 % of the target picks it up. */
    public static final int PICKUP_WINDOW = V14 * 3 / 100;
    /** rewind / fast_forward step. */
    public static final double JUMP_BEATS = 4.0;
    /** Held rewind / fast_forward repeats after ~300 ms, then every tick. */
    public static final int REPEAT_DELAY_TICKS = 3;
    /**
     * After an analog input writes a value, ignore the host's (possibly not yet updated) value for
     * ~300 ms. Bitwig applies set() asynchronously, so get() can still return the old value.
     */
    public static final int SETTLE_TICKS = 3;
    /** Host value vs what the knob set: closer than this is rounding, not a change from elsewhere. */
    public static final int SAME_TOLERANCE = 8;

    /** Puts one FB_* notification on the wire. */
    public interface Notifier {
        void notify(int cmd, byte[] payload);
    }

    public static int to14(double norm) {
        if (!(norm > 0.0)) return 0;
        if (norm >= 1.0) return V14;
        return (int) Math.round(norm * V14);
    }

    // ---- bindings ----

    private static final class ButtonRole {
        Runnable press;
        Runnable repeat;
        IntSupplier state;          // LED level; null with momentary
        boolean momentary;          // LED lit while held

        static ButtonRole of(Runnable press, IntSupplier state) {
            ButtonRole r = new ButtonRole();
            r.press = press;
            r.state = state;
            return r;
        }

        static ButtonRole momentary(Runnable press) {
            ButtonRole r = new ButtonRole();
            r.press = press;
            r.momentary = true;
            return r;
        }
    }

    private static final class ButtonBinding {
        final Component component;
        final ButtonRole role;
        boolean held;
        int heldTicks;

        ButtonBinding(Component c, ButtonRole r) {
            component = c;
            role = r;
        }

        int level() {
            if (role.momentary) return held ? ON : OFF;
            return role.state != null ? role.state.getAsInt() : OFF;
        }
    }

    private static final class ContBinding {
        final Component component;
        final Supplier<Param> target;
        boolean picked;
        Integer lastRaw;
        int holdTicks;              // > 0: the knob just wrote; the host's value may still be the old one
        // The value last sent (for an analog input: or what the knob set). hasValue false: nothing
        // yet, or no target.
        boolean hasValue;
        Object valueKey;
        int value;
        int valueTick = -1;         // the tick of the last FB_VALUE
        // The ring last sent (ringMode -1: nothing yet).
        int ringMode = -1;
        int ringValue;
        boolean ringCentre;
        boolean ringOff;

        ContBinding(Component c, Supplier<Param> t) {
            component = c;
            target = t;
        }

        void sent(Object key, int v) {
            hasValue = true;
            valueKey = key;
            value = v;
        }

        void noTarget() {
            hasValue = false;
            valueKey = null;
        }

        void forget() {
            picked = false;
            lastRaw = null;
            holdTicks = 0;
            noTarget();
            valueTick = -1;
            ringMode = -1;
        }

        /** Whether the ring differs from the last one sent; if so it becomes the last one sent. */
        boolean ringChanged(int mode, int v, boolean centre, boolean off) {
            if (ringMode == mode && ringValue == v && ringCentre == centre && ringOff == off) return false;
            ringMode = mode;
            ringValue = v;
            ringCentre = centre;
            ringOff = off;
            return true;
        }
    }

    private final Model model;
    private final Notifier notifier;
    private final Consumer<String> log;

    private final Map<Integer, ButtonBinding> buttons = new HashMap<>();
    private final Map<Integer, IntSupplier> leds = new TreeMap<>();
    private final Map<Integer, ContBinding> analogs = new HashMap<>();
    private final Map<Integer, ContBinding> encoders = new HashMap<>();
    // The same bindings as arrays (built by bind()), so a refresh allocates nothing. LEDs are in
    // ascending index order; ledSent is the level last sent (-1: none yet).
    private int[] ledIndex = new int[0];
    private IntSupplier[] ledLevel = new IntSupplier[0];
    private int[] ledSent = new int[0];
    private int[] ledChanged = new int[0];        // scratch for refreshLeds(): the new level, or -1
    private ContBinding[] analogList = new ContBinding[0];
    private ContBinding[] encoderList = new ContBinding[0];
    private int tickCount;
    private boolean limitValues;                  // inside flush(): one FB_VALUE per control per tick
    private final List<String> unknownRoles = new ArrayList<>();
    private final List<String> unsupportedRoles = new ArrayList<>();

    public Mapping(Model model, Notifier notifier, Consumer<String> log) {
        this.model = model;
        this.notifier = notifier;
        this.log = log != null ? log : s -> {};
    }

    /** "kind index role" for each role this mapping doesn't know (x_ roles excluded). */
    public List<String> unknownRoles() {
        return unknownRoles;
    }

    /** "kind index role" for each known role the host can't do. */
    public List<String> unsupportedRoles() {
        return unsupportedRoles;
    }

    // ---- setup ----

    /**
     * Builds the bindings from the device's components (roles already normalised). LEDs with
     * owner: local ignore FB_LED, so they get no binding (sysex.md §4.6.1 {@code owner}).
     */
    public void bind(List<Component> components) {
        buttons.clear();
        leds.clear();
        analogs.clear();
        encoders.clear();
        unknownRoles.clear();
        unsupportedRoles.clear();
        forget();
        Set<Integer> local = new HashSet<>();
        for (Component c : components) {
            if (c.kind == Protocol.KIND_LED && c.localOnly()) local.add(c.index);
        }
        // Buttons first, so a button's own LED binding (which knows "held") wins over a kind-5 LED role.
        for (Component c : components) {
            if (c.kind != Protocol.KIND_BUTTON || c.role.isEmpty() || !usable(c)) continue;
            ButtonRole br = buttonRole(c.role);
            if (br == null) {
                unknown(c);
                continue;
            }
            ButtonBinding b = new ButtonBinding(c, br);
            buttons.put(c.index, b);
            if (c.hasLed() && !local.contains(c.led)) leds.put(c.led, b::level);
        }
        for (Component c : components) {
            if (c.role.isEmpty()) continue;
            if (c.kind == Protocol.KIND_LED) {
                if (c.localOnly() || !usable(c)) continue;
                ButtonRole br = buttonRole(c.role);
                if (br == null) {
                    unknown(c);
                    continue;
                }
                if (!leds.containsKey(c.index)) {
                    leds.put(c.index, () -> br.momentary || br.state == null ? OFF : br.state.getAsInt());
                }
            } else if (c.kind == Protocol.KIND_ANALOG || c.kind == Protocol.KIND_ENCODER) {
                if (!usable(c)) continue;
                Supplier<Param> target = contRole(c.role);
                if (target == null) {
                    unknown(c);
                    continue;
                }
                (c.kind == Protocol.KIND_ANALOG ? analogs : encoders).put(c.index, new ContBinding(c, target));
            }
            // keys (2) and displays (6): no roles in hosts/README.md yet
        }
        int n = leds.size();
        ledIndex = new int[n];
        ledLevel = new IntSupplier[n];
        ledSent = new int[n];
        ledChanged = new int[n];
        int k = 0;
        for (Map.Entry<Integer, IntSupplier> e : leds.entrySet()) {
            ledIndex[k] = e.getKey();
            ledLevel[k] = e.getValue();
            k++;
        }
        Arrays.fill(ledSent, -1);
        analogList = analogs.values().toArray(new ContBinding[0]);
        encoderList = encoders.values().toArray(new ContBinding[0]);
        for (String s : unknownRoles) log.accept("unknown role, ignored: " + s);
        for (String s : unsupportedRoles) log.accept("role not supported in this host, ignored: " + s);
    }

    private static String describe(Component c) {
        return Protocol.kindName(c.kind) + " " + c.index + " " + c.role;
    }

    private boolean usable(Component c) {
        if (c.role.startsWith("x_") || Roles.isOtherHostCommand(c.role, HOST_NAMESPACE)) return false;
        if (!model.supports(c.role)) {
            unsupportedRoles.add(describe(c));
            return false;
        }
        return true;
    }

    private void unknown(Component c) {
        unknownRoles.add(describe(c));
    }

    /** Drops what was sent: the next refresh() sends everything (after ready / a release). */
    public void forget() {
        Arrays.fill(ledSent, -1);
        for (ContBinding b : analogList) b.forget();
        for (ContBinding b : encoderList) b.forget();
    }

    /** The device is being handed back (FB_RELEASE goes out from Device.release()). */
    public void release() {
        forget();
        for (ButtonBinding b : buttons.values()) b.held = false;
    }

    // ---- role tables ----

    private static int lit(boolean on) {
        return on ? ON : OFF;
    }

    private ButtonRole toggle(TransportFlag f) {
        return ButtonRole.of(() -> model.setFlag(f, !model.flag(f)), () -> lit(model.flag(f)));
    }

    private ButtonRole buttonRole(String role) {
        Model m = model;
        switch (role) {
            case "play": return toggle(TransportFlag.PLAYING);
            case "stop": return ButtonRole.momentary(m::stop);
            case "record":
                return ButtonRole.of(() -> m.setFlag(TransportFlag.RECORD, !m.flag(TransportFlag.RECORD)), () -> {
                    if (!m.flag(TransportFlag.RECORD)) return OFF;
                    return m.flag(TransportFlag.PLAYING) ? ON : BLINK;     // armed, waiting for play
                });
            case "loop": return toggle(TransportFlag.LOOP);
            case "metronome": return toggle(TransportFlag.METRONOME);
            case "overdub": return toggle(TransportFlag.OVERDUB);
            case "session_record": return toggle(TransportFlag.LAUNCHER_OVERDUB);
            case "automation_arm": return toggle(TransportFlag.AUTOMATION_WRITE);
            case "tap_tempo": return ButtonRole.momentary(m::tapTempo);
            case "undo": return ButtonRole.momentary(m::undo);
            case "redo": return ButtonRole.momentary(m::redo);
            case "rewind":
            case "fast_forward": {
                double beats = role.equals("rewind") ? -JUMP_BEATS : JUMP_BEATS;
                ButtonRole r = ButtonRole.momentary(() -> m.jump(beats));
                r.repeat = r.press;
                return r;
            }
            case "track_prev":
            case "track_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepTrack(d), () -> lit(m.canStepTrack(d)));
            }
            case "track_mute": return trackFlagRole(TrackFlag.MUTE, m::selectedTrack);
            case "track_solo": return trackFlagRole(TrackFlag.SOLO, m::selectedTrack);
            case "track_arm": return trackFlagRole(TrackFlag.ARM, m::selectedTrack);
            case "bank_prev":
            case "bank_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepBank(d), () -> lit(m.canStepBank(d)));
            }
            case "strip_prev":
            case "strip_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepStrip(d), () -> lit(m.canStepBank(d)));
            }
            case "device_prev":
            case "device_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepDevice(d), () -> lit(m.canStepDevice(d)));
            }
            case "device_bank_prev":
            case "device_bank_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepDevicePage(d), () -> lit(m.canStepDevicePage(d)));
            }
            case "device_on":
                return ButtonRole.of(() -> {
                    if (m.hasDevice()) m.setDeviceEnabled(!m.deviceEnabled());
                }, () -> lit(m.hasDevice() && m.deviceEnabled()));
            case "scene_launch": return ButtonRole.momentary(m::launchScene);
            case "scene_prev":
            case "scene_next": {
                int d = role.endsWith("prev") ? -1 : 1;
                return ButtonRole.of(() -> m.stepScene(d), () -> lit(m.canStepScene(d)));
            }
            case "stop_all_clips": return ButtonRole.momentary(m::stopAllClips);
            default:
                break;
        }
        int n = stripNumber(role, "mute_");
        if (n > 0) return trackFlagRole(TrackFlag.MUTE, stripFn(n - 1));
        n = stripNumber(role, "solo_");
        if (n > 0) return trackFlagRole(TrackFlag.SOLO, stripFn(n - 1));
        n = stripNumber(role, "arm_");
        if (n > 0) return trackFlagRole(TrackFlag.ARM, stripFn(n - 1));
        n = stripNumber(role, "select_");
        if (n > 0) {
            int i = n - 1;
            return ButtonRole.of(() -> {
                if (m.strip(i) != null) m.selectStrip(i);
            }, () -> lit(m.strip(i) != null && m.isStripSelected(i)));
        }
        return null;
    }

    private Supplier<Strip> stripFn(int i) {
        return () -> model.strip(i);
    }

    private static ButtonRole trackFlagRole(TrackFlag f, Supplier<Strip> track) {
        return ButtonRole.of(() -> {
            Strip t = track.get();
            if (t == null || (f == TrackFlag.ARM && !t.canArm())) return;
            t.setFlag(f, !t.flag(f));
        }, () -> {
            Strip t = track.get();
            return lit(t != null && t.flag(f));
        });
    }

    /** "volume_3" with prefix "volume_" → 3 (1..8), else 0. */
    static int stripNumber(String role, String prefix) {
        if (!role.startsWith(prefix)) return 0;
        String num = role.substring(prefix.length());
        if (!Roles.isDigits(num) || num.length() > 2) return 0;
        int n = Integer.parseInt(num);
        return n >= 1 && n <= STRIPS ? n : 0;
    }

    private Supplier<Param> selectedParam(TrackParam p) {
        return () -> {
            Strip t = model.selectedTrack();
            return t != null ? t.param(p) : null;
        };
    }

    private Supplier<Param> contRole(String role) {
        Model m = model;
        switch (role) {
            case "track_volume": return selectedParam(TrackParam.VOLUME);
            case "track_pan": return selectedParam(TrackParam.PAN);
            case "send_a": return selectedParam(TrackParam.SEND_A);
            case "send_b": return selectedParam(TrackParam.SEND_B);
            case "master_volume": return () -> m.master(MasterParam.VOLUME);
            case "cue_volume": return () -> m.master(MasterParam.CUE);
            case "crossfader": return () -> m.master(MasterParam.CROSSFADER);
            case "tempo": return m::tempo;
            default:
                break;
        }
        int n = stripNumber(role, "volume_");
        if (n > 0) {
            int i = n - 1;
            return () -> {
                Strip t = m.strip(i);
                return t != null ? t.param(TrackParam.VOLUME) : null;
            };
        }
        n = stripNumber(role, "pan_");
        if (n > 0) {
            int i = n - 1;
            return () -> {
                Strip t = m.strip(i);
                return t != null ? t.param(TrackParam.PAN) : null;
            };
        }
        n = stripNumber(role, "device_param_");
        if (n > 0) {
            int i = n - 1;
            return () -> m.deviceParam(i);
        }
        return null;
    }

    // ---- events ----

    public void handleEvent(Event ev) {
        switch (ev.type) {
            case Protocol.KIND_BUTTON: {
                ButtonBinding b = buttons.get(ev.index);
                if (b == null) return;
                if (ev.value != 0) {
                    b.held = true;
                    b.heldTicks = 0;
                    if (b.role.press != null) b.role.press.run();
                } else {
                    b.held = false;
                }
                refresh();
                break;
            }
            case Protocol.KIND_ANALOG: {
                ContBinding b = analogs.get(ev.index);
                if (b != null) analog(b, Math.min(V14, ev.value));
                break;
            }
            case Protocol.KIND_ENCODER: {
                ContBinding b = encoders.get(ev.index);
                if (b == null) return;
                Param p = b.target.get();
                int delta = ev.delta();
                if (p != null && delta != 0) {
                    p.nudge(delta);
                    refreshEncoder(b);
                }
                break;
            }
            case Protocol.EVENT_LAYER:
                // A layer on or off. Each twin has its own binding, so twins with one role in two
                // modes both drive it and both get its feedback; the device only sends EVENTs from
                // live ones. Device pages stay Bitwig's remote-control pages of 8.
                break;
            default:
                break;                  // keys (type 2), pads, touch and reserved types: not mapped
        }
    }

    private void analog(ContBinding b, int raw) {
        Param p = b.target.get();
        if (p == null) return;
        int cur = to14(p.get());
        if (!b.picked) {
            boolean crossed = b.lastRaw != null && (long) (b.lastRaw - cur) * (raw - cur) <= 0;
            if (crossed || Math.abs(raw - cur) <= PICKUP_WINDOW) b.picked = true;
        }
        b.lastRaw = raw;
        if (!b.picked) return;
        p.set(raw / (double) V14);
        // What the knob set is not news to the knob: no FB_VALUE echo. The host may apply the value
        // later (Bitwig does), so remember what the knob asked for, not what get() says right now.
        b.sent(p.key(), raw);
        b.holdTicks = SETTLE_TICKS;
    }

    // ---- clock ----

    /** Call about every 100 ms: held-button repeats, then refresh(). */
    public void tick() {
        tickCount++;
        for (ContBinding b : analogList) {
            if (b.holdTicks > 0) b.holdTicks--;
        }
        for (ButtonBinding b : buttons.values()) {
            if (b.held && b.role.repeat != null) {
                b.heldTicks++;
                if (b.heldTicks >= REPEAT_DELAY_TICKS) b.role.repeat.run();
            }
        }
        refresh();
    }

    // ---- feedback ----

    /** Sends every difference now. */
    public void refresh() {
        refreshLeds();
        for (ContBinding b : analogList) refreshAnalog(b);
        for (ContBinding b : encoderList) refreshEncoder(b);
    }

    /**
     * For Bitwig's {@code flush()}, which can come many times a tick while values move (automation,
     * a mouse drag): LEDs and rings now, but at most one FB_VALUE per control per tick. A value held
     * back goes out on the next tick, so the device always ends on the latest one.
     */
    public void flush() {
        limitValues = true;
        try {
            refresh();
        } finally {
            limitValues = false;
        }
    }

    /** Whether an FB_VALUE for {@code b} has to wait for the next tick. */
    private boolean holdValue(ContBinding b) {
        return limitValues && b.valueTick == tickCount;
    }

    private void sendValue(ContBinding b, int kind, int v) {
        b.valueTick = tickCount;
        notifier.notify(Protocol.FB_VALUE, Protocol.fbValue(kind, b.component.index, v));
    }

    private void refreshLeds() {
        int changed = 0;
        for (int i = 0; i < ledIndex.length; i++) {
            int level = ledLevel[i].getAsInt();
            if (ledSent[i] != level) {
                ledSent[i] = level;
                ledChanged[i] = level;
                changed++;
            } else {
                ledChanged[i] = -1;
            }
        }
        if (changed == 0) return;
        // Runs of neighbouring LEDs go out as one FB_LEDS (up to 64), single ones as FB_LED.
        int i = 0;
        while (i < ledIndex.length) {
            if (ledChanged[i] < 0) {
                i++;
                continue;
            }
            int j = i + 1;
            while (j < ledIndex.length && j - i < 64 && ledChanged[j] >= 0 && ledIndex[j] == ledIndex[j - 1] + 1) j++;
            if (j - i == 1) {
                notifier.notify(Protocol.FB_LED, Protocol.fbLed(ledIndex[i], ledChanged[i]));
            } else {
                notifier.notify(Protocol.FB_LEDS, Protocol.fbLeds(ledIndex[i], Arrays.copyOfRange(ledChanged, i, j)));
            }
            i = j;
        }
    }

    private void refreshAnalog(ContBinding b) {
        Param p = b.target.get();
        if (p == null) {
            b.noTarget();
            return;
        }
        Object key = p.key();
        int v = to14(p.get());
        boolean sameTarget = b.hasValue && Objects.equals(b.valueKey, key);
        if (sameTarget && b.holdTicks > 0) return;            // the host hasn't caught up with the knob yet
        if (!sameTarget || Math.abs(b.value - v) > SAME_TOLERANCE) {
            b.picked = false;                 // the target moved away from the knob
            b.lastRaw = null;
            boolean takes = b.component.takesValue();       // takeover: none ignores FB_VALUE (sysex.md §4.6.2)
            if (takes && holdValue(b)) return;              // one went out this tick: the tick sends this one
            b.sent(key, v);
            if (takes) sendValue(b, Protocol.KIND_ANALOG, v);
        }
    }

    private void refreshEncoder(ContBinding b) {
        Component c = b.component;
        Param p = b.target.get();
        int mode = Protocol.RING_WRAP;
        int v = 0;
        boolean centre = false;
        boolean off = false;
        if (p == null) {
            off = true;                                   // nothing mapped: ring off (flags bit 1)
            b.noTarget();
        } else {
            v = to14(p.get());
            Object key = p.key();
            if (!b.hasValue || b.value != v || !Objects.equals(b.valueKey, key)) {
                boolean takes = c.takesValue();           // a relative encoder ignores FB_VALUE (sysex.md §4.6.2)
                if (!takes || !holdValue(b)) {            // else one went out this tick: the tick sends this one
                    b.sent(key, v);
                    if (takes) sendValue(b, Protocol.KIND_ENCODER, v);
                }
            }
            if (p.bipolar()) {
                mode = Protocol.RING_BOOST_CUT;
                centre = true;
            }
        }
        if (c.ring > 0 && b.ringChanged(mode, v, centre, off)) {
            notifier.notify(Protocol.FB_RING, Protocol.fbRing(c.index, mode, v, centre, off));
        }
    }
}
