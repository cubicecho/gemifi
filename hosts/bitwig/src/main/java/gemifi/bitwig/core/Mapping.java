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
 * {@link Notifier}. State is compared, not pushed: {@link #refresh()} (called from the
 * extension's {@code flush()} and its tick) reads every bound state, compares it with what was last
 * sent and sends only the differences. A lost notification is fixed by the next change (§4.6.2
 * says feedback is idempotent).
 */
public final class Mapping {
    public static final int STRIPS = 8;
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

        ContBinding(Component c, Supplier<Param> t) {
            component = c;
            target = t;
        }
    }

    /** What an encoder's ring shows. */
    private static final class Ring {
        final int mode;
        final int value;
        final boolean centre;
        final boolean off;

        Ring(int mode, int value, boolean centre, boolean off) {
            this.mode = mode;
            this.value = value;
            this.centre = centre;
            this.off = off;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Ring)) return false;
            Ring r = (Ring) o;
            return mode == r.mode && value == r.value && centre == r.centre && off == r.off;
        }

        @Override
        public int hashCode() {
            return Objects.hash(mode, value, centre, off);
        }
    }

    /** (target key, 14-bit value) last sent as FB_VALUE. */
    private static final class Sent {
        final Object key;
        final int value;

        Sent(Object key, int value) {
            this.key = key;
            this.value = value;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Sent)) return false;
            Sent s = (Sent) o;
            return value == s.value && Objects.equals(key, s.key);
        }

        @Override
        public int hashCode() {
            return Objects.hash(key, value);
        }
    }

    private final Model model;
    private final Notifier notifier;
    private final Consumer<String> log;

    private final Map<Integer, ButtonBinding> buttons = new HashMap<>();
    private final Map<Integer, IntSupplier> leds = new TreeMap<>();
    private final Map<Integer, ContBinding> analogs = new HashMap<>();
    private final Map<Integer, ContBinding> encoders = new HashMap<>();
    private final Map<Integer, Integer> ledCache = new HashMap<>();
    private final Map<Integer, Sent> analogCache = new HashMap<>();
    private final Map<Integer, Sent> encoderCache = new HashMap<>();
    private final Map<Integer, Ring> ringCache = new HashMap<>();
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
        for (String s : unknownRoles) log.accept("unknown role, ignored: " + s);
        for (String s : unsupportedRoles) log.accept("role not supported in this host, ignored: " + s);
    }

    private static String describe(Component c) {
        return Protocol.kindName(c.kind) + " " + c.index + " " + c.role;
    }

    private boolean usable(Component c) {
        if (c.role.startsWith("x_")) return false;
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
        ledCache.clear();
        analogCache.clear();
        encoderCache.clear();
        ringCache.clear();
        for (ContBinding b : analogs.values()) {
            b.picked = false;
            b.lastRaw = null;
            b.holdTicks = 0;
        }
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
            default:
                break;                  // keys (type 2) and reserved types: not mapped
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
        analogCache.put(b.component.index, new Sent(p.key(), raw));
        b.holdTicks = SETTLE_TICKS;
    }

    // ---- clock ----

    /** Call about every 100 ms: held-button repeats, then refresh(). */
    public void tick() {
        for (ContBinding b : analogs.values()) {
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

    public void refresh() {
        refreshLeds();
        for (ContBinding b : analogs.values()) refreshAnalog(b);
        for (ContBinding b : encoders.values()) refreshEncoder(b);
    }

    private void refreshLeds() {
        TreeMap<Integer, Integer> changed = new TreeMap<>();
        for (Map.Entry<Integer, IntSupplier> e : leds.entrySet()) {
            int level = e.getValue().getAsInt();
            Integer old = ledCache.get(e.getKey());
            if (old == null || old != level) {
                changed.put(e.getKey(), level);
                ledCache.put(e.getKey(), level);
            }
        }
        // Runs of neighbouring LEDs go out as one FB_LEDS (up to 64), single ones as FB_LED.
        List<Integer> run = new ArrayList<>();
        for (int led : changed.keySet()) {
            if (!run.isEmpty() && (led != run.get(run.size() - 1) + 1 || run.size() == 64)) {
                sendRun(run, changed);
                run.clear();
            }
            run.add(led);
        }
        if (!run.isEmpty()) sendRun(run, changed);
    }

    private void sendRun(List<Integer> run, Map<Integer, Integer> levels) {
        if (run.size() == 1) {
            notifier.notify(Protocol.FB_LED, Protocol.fbLed(run.get(0), levels.get(run.get(0))));
            return;
        }
        int[] lv = new int[run.size()];
        for (int i = 0; i < lv.length; i++) lv[i] = levels.get(run.get(i));
        notifier.notify(Protocol.FB_LEDS, Protocol.fbLeds(run.get(0), lv));
    }

    private void refreshAnalog(ContBinding b) {
        Param p = b.target.get();
        int idx = b.component.index;
        if (p == null) {
            analogCache.put(idx, null);
            return;
        }
        Sent now = new Sent(p.key(), to14(p.get()));
        Sent old = analogCache.get(idx);
        boolean sameTarget = old != null && Objects.equals(old.key, now.key);
        if (sameTarget && b.holdTicks > 0) return;            // the host hasn't caught up with the knob yet
        if (!sameTarget || Math.abs(old.value - now.value) > SAME_TOLERANCE) {
            analogCache.put(idx, now);
            b.picked = false;                 // the target moved away from the knob
            b.lastRaw = null;
            if (b.component.takesValue()) {         // takeover: none ignores FB_VALUE (sysex.md §4.6.2)
                notifier.notify(Protocol.FB_VALUE, Protocol.fbValue(Protocol.KIND_ANALOG, idx, now.value));
            }
        }
    }

    private void refreshEncoder(ContBinding b) {
        Component c = b.component;
        Param p = b.target.get();
        Ring ring;
        if (p == null) {
            ring = new Ring(Protocol.RING_WRAP, 0, false, true);     // nothing mapped: ring off (flags bit 1)
            encoderCache.put(c.index, null);
        } else {
            int v = to14(p.get());
            Sent now = new Sent(p.key(), v);
            if (!now.equals(encoderCache.get(c.index))) {
                encoderCache.put(c.index, now);
                if (c.takesValue()) {                // a relative encoder ignores FB_VALUE (sysex.md §4.6.2)
                    notifier.notify(Protocol.FB_VALUE, Protocol.fbValue(Protocol.KIND_ENCODER, c.index, v));
                }
            }
            ring = p.bipolar() ? new Ring(Protocol.RING_BOOST_CUT, v, true, false)
                    : new Ring(Protocol.RING_WRAP, v, false, false);
        }
        if (c.ring > 0 && !ring.equals(ringCache.get(c.index))) {
            ringCache.put(c.index, ring);
            notifier.notify(Protocol.FB_RING, Protocol.fbRing(c.index, ring.mode, ring.value, ring.centre, ring.off));
        }
    }
}
