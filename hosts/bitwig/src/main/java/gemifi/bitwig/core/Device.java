// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Event;
import gemifi.bitwig.core.Protocol.Frame;
import gemifi.bitwig.core.Protocol.Hello;
import gemifi.bitwig.core.Protocol.ProtocolException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import java.util.function.IntConsumer;

/**
 * State machine for one gemifi device on the extension's port ({@code protocol/sysex.md} §4.6).
 *
 * <pre>
 * searching --HELLO ok, caps bit 6--&gt; enumerating --all kinds read--&gt; enabling --debug.events=1--&gt; ready
 *     |                                                                                       |
 *     +--HELLO ok, no caps bit 6--&gt; unsupported          keep-alive fails / events turned off +--&gt; searching
 * </pre>
 *
 * <p>Pure Java, no Bitwig imports; a port of {@code hosts/ableton-live/gemifi/device.py}. Time is a
 * tick counter: the extension calls {@link #tick()} about every 100 ms. Requests are stop-and-wait
 * (§1): one outstanding request, resent with the same seq on a timeout, BAD_CHECKSUM or BUSY, up to
 * {@link #MAX_TRIES} sends.
 */
public final class Device {
    public enum State { IDLE, SEARCHING, ENUMERATING, ENABLING, READY, UNSUPPORTED }

    /** §1: 200 ms reply timeout. A tick is ~100 ms but may come early, so wait 3 ticks. */
    public static final int TIMEOUT_TICKS = 3;
    /** 5 resends of the same seq. */
    public static final int MAX_TRIES = 6;
    /** After a failed search, try HELLO again ~3 s later. */
    public static final int RESCAN_TICKS = 30;
    /** In ready, read debug.events every ~5 s to notice a replug / reboot. */
    public static final int KEEPALIVE_TICKS = 50;

    /** Callbacks; any may be left null. */
    public interface Listener {
        /** Components are known and EVENTs are on: drive feedback now. */
        default void onReady(Device device) {}

        /** The device stopped answering (or rebooted): its feedback state is gone. */
        default void onLost(Device device) {}

        default void onEvent(Event event) {}

        default void onLog(String text) {}
    }

    private interface OnOk {
        void accept(Frame frame) throws ProtocolException;
    }

    private static final class Request {
        final int cmd;
        int seq;
        final byte[] payload;
        final OnOk onOk;
        final IntConsumer onError;     // status, or -1 for "gave up"
        int tries;
        int waited;

        Request(int cmd, byte[] payload, OnOk onOk, IntConsumer onError) {
            this.cmd = cmd;
            this.payload = payload;
            this.onOk = onOk;
            this.onError = onError;
        }
    }

    /** "No reply" passed to error handlers. */
    public static final int GAVE_UP = -1;

    private final Consumer<byte[]> send;
    private final int timeoutTicks;
    private final int maxTries;
    private final int rescanTicks;
    private final int keepaliveTicks;
    private Listener listener = new Listener() {};

    private State state = State.IDLE;
    private Hello hello;
    private final Map<Integer, List<Component>> components = new LinkedHashMap<>();
    private Integer eventsBefore;
    private int seq;
    private Request pending;
    private final Deque<Request> queue = new ArrayDeque<>();
    private int wait;
    private int enumKind;

    public Device(Consumer<byte[]> send) {
        this(send, TIMEOUT_TICKS, MAX_TRIES, RESCAN_TICKS, KEEPALIVE_TICKS);
    }

    public Device(Consumer<byte[]> send, int timeoutTicks, int maxTries, int rescanTicks, int keepaliveTicks) {
        this.send = send;
        this.timeoutTicks = timeoutTicks;
        this.maxTries = maxTries;
        this.rescanTicks = rescanTicks;
        this.keepaliveTicks = keepaliveTicks;
    }

    public void setListener(Listener l) {
        listener = l == null ? new Listener() {} : l;
    }

    // ---- public ----

    public State state() {
        return state;
    }

    public Hello hello() {
        return hello;
    }

    public boolean isReady() {
        return state == State.READY;
    }

    /** §4.6.2: notifications only go to a device that set caps bit 6. */
    public boolean feedbackOk() {
        return hello != null && hello.hasFeedback();
    }

    public List<Component> components(int kind) {
        List<Component> l = components.get(kind);
        return l == null ? new ArrayList<>() : l;
    }

    public List<Component> allComponents() {
        List<Component> out = new ArrayList<>();
        for (int kind : Protocol.ALL_KINDS) out.addAll(components(kind));
        return out;
    }

    /** (Re)starts from scratch: forget everything and send HELLO. */
    public void start() {
        pending = null;
        queue.clear();
        hello = null;
        components.clear();
        state = State.SEARCHING;
        request(Protocol.HELLO, new byte[0], this::onHello, s -> onSearchFailed());
    }

    public void stop() {
        pending = null;
        queue.clear();
        state = State.IDLE;
    }

    /** Sends an FB_* notification. Never answered, never retried, ignores stop-and-wait. */
    public boolean notify(int cmd, byte[] payload) {
        if (cmd < Protocol.NOTIFY_FIRST || cmd > Protocol.NOTIFY_LAST) {
            throw new IllegalArgumentException(String.format("not a notification command: 0x%02X", cmd));
        }
        if (!feedbackOk()) return false;
        send.accept(Protocol.buildFrame(cmd, 0, payload));
        return true;
    }

    /**
     * Hands everything back (§4.6 step 5): FB_RELEASE 0 and debug.events back to what it was.
     * Fire-and-forget: this runs while the extension is going away.
     */
    public void release() {
        if (!feedbackOk()) {
            stop();
            return;
        }
        notify(Protocol.FB_RELEASE, Protocol.fbRelease(Protocol.RELEASE_ALL));
        if (eventsBefore != null && eventsBefore != 1 && state == State.READY) {
            send.accept(Protocol.buildFrame(Protocol.SET_PARAM, nextSeq(),
                    Protocol.reqSetParam(Protocol.PARAM_DEBUG_EVENTS, 0, eventsBefore)));
        }
        stop();
    }

    /** Call about every 100 ms. */
    public void tick() {
        Request p = pending;
        if (p != null) {
            p.waited++;
            if (p.waited >= timeoutTicks) resendOrFail(p, "timeout");
            return;
        }
        if (wait > 0) {
            wait--;
            if (wait == 0) {
                if (state == State.SEARCHING) start();
                else if (state == State.READY) keepalive();
            }
        }
    }

    /** Feed every incoming SysEx message here. Returns true if it was a gemifi message. */
    public boolean receive(byte[] msg) {
        if (msg == null || !Protocol.isGemifi(msg)) return false;
        Frame frame;
        try {
            frame = Protocol.parseFrame(msg);
        } catch (ProtocolException e) {
            log("dropped bad frame: " + e.getMessage());
            return true;
        }
        if (frame.isUnsolicited()) onUnsolicited(frame);
        else if (frame.isReply()) onReply(frame);
        return true;
    }

    // ---- requests ----

    private int nextSeq() {
        seq = (seq + 1) & 0x7F;
        return seq;
    }

    private void request(int cmd, byte[] payload, OnOk onOk, IntConsumer onError) {
        Request r = new Request(cmd, payload, onOk, onError);
        if (pending == null) startRequest(r);
        else queue.add(r);
    }

    private void startRequest(Request r) {
        r.seq = nextSeq();
        pending = r;
        transmit(r);
    }

    private void transmit(Request r) {
        r.tries++;
        r.waited = 0;
        send.accept(Protocol.buildFrame(r.cmd, r.seq, r.payload));
    }

    private void resendOrFail(Request r, String why) {
        if (r.tries < maxTries) {
            transmit(r);
            return;
        }
        log(String.format("0x%02X failed after %d tries (%s)", r.cmd, r.tries, why));
        finish(r, null);
    }

    /** Completes r with frame (null = gave up), then starts the next queued request. */
    private void finish(Request r, Frame frame) {
        pending = null;
        if (frame != null && frame.status() == Protocol.OK) {
            try {
                r.onOk.accept(frame);
            } catch (ProtocolException e) {
                log(String.format("bad reply to 0x%02X: %s", r.cmd, e.getMessage()));
                if (r.onError != null) r.onError.accept(Protocol.BAD_LEN);
            }
        } else if (r.onError != null) {
            r.onError.accept(frame != null ? frame.status() : GAVE_UP);
        }
        if (pending == null && !queue.isEmpty()) startRequest(queue.poll());
    }

    private void onReply(Frame frame) {
        Request r = pending;
        if (r == null || frame.cmd != (r.cmd | Protocol.REPLY) || frame.seq != r.seq || frame.payload.length == 0) {
            return;                                            // stale or stray reply
        }
        int status = frame.status();
        if (status == Protocol.BAD_CHECKSUM || status == Protocol.BUSY) {
            resendOrFail(r, Protocol.statusName(status));
            return;
        }
        finish(r, frame);
    }

    // ---- unsolicited ----

    private void onUnsolicited(Frame frame) {
        try {
            if (frame.cmd == Protocol.EVENT) {
                Event ev = Protocol.parseEvent(frame.payload);
                if (state == State.READY) listener.onEvent(ev);
            } else if (frame.cmd == Protocol.LOG) {
                int n = Math.max(0, frame.payload.length - 5);
                StringBuilder sb = new StringBuilder(n);
                for (int i = 5; i < frame.payload.length; i++) sb.append((char) (frame.payload[i] & 0x7F));
                log("device: " + sb);
            } else if (frame.cmd == Protocol.PARAM_CHANGED) {
                int[] p = Protocol.parseParamReply(frame.payload);
                if (p[0] == Protocol.PARAM_DEBUG_EVENTS && p[2] == 0 && state == State.READY) {
                    log("debug.events turned off on the device; turning it back on");
                    enableEvents();
                }
            }
        } catch (ProtocolException e) {
            // malformed unsolicited message: drop it
        }
    }

    // ---- the steps ----

    private void onHello(Frame frame) {
        try {
            hello = Protocol.parseHello(frame.payload);
        } catch (ProtocolException e) {
            log("bad HELLO reply: " + e.getMessage());
            onSearchFailed();
            return;
        }
        Hello h = hello;
        if (h.role != 0) {
            // The bootloader (an update in progress): look again later, the app comes back.
            log("device is in its bootloader; waiting for the app");
            hello = null;
            onSearchFailed();
            return;
        }
        if (!h.hasFeedback()) {
            state = State.UNSUPPORTED;
            log(String.format("device %s (fw %s) has no host feedback (caps 0x%X)",
                    h.node != null ? h.node : "?", h.versionText(), h.caps));
            return;
        }
        log("found " + (h.node != null ? h.node : "gemifi device") + " fw " + h.versionText());
        state = State.ENUMERATING;
        components.clear();
        enumKind = 0;
        enumNextKind();
    }

    private void onSearchFailed() {
        state = State.SEARCHING;
        wait = rescanTicks;
    }

    private void enumNextKind() {
        if (enumKind < Protocol.ALL_KINDS.length) {
            int kind = Protocol.ALL_KINDS[enumKind++];
            components.put(kind, new ArrayList<>());
            getComponent(kind, 0);
            return;
        }
        enumDone();
    }

    private void getComponent(int kind, int index) {
        request(Protocol.GET_COMPONENT, Protocol.reqGetComponent(kind, index), frame -> {
            Component c;
            try {
                c = Protocol.parseComponentReply(frame.payload);
            } catch (ProtocolException e) {
                log("bad GET_COMPONENT reply (" + kind + ", " + index + "): " + e.getMessage());
                enumNextKind();
                return;
            }
            c.kind = kind;                       // trust our request over the echo
            c.index = index;
            c.role = Roles.normalise(c.role);
            components.get(kind).add(c);
            if (index + 1 < c.count && index + 1 < Protocol.NONE_INDEX) getComponent(kind, index + 1);
            else enumNextKind();
        }, status -> {
            if (status == Protocol.BAD_PARAM) {
                enumNextKind();                  // none of this kind (index 0) or past the end
            } else if (status == GAVE_UP) {
                lost("GET_COMPONENT timed out");
            } else {
                log("GET_COMPONENT (" + kind + ", " + index + ") -> " + Protocol.statusName(status) + "; skipping the kind");
                enumNextKind();
            }
        });
    }

    private void enumDone() {
        state = State.ENABLING;
        log(allComponents().size() + " components");
        request(Protocol.GET_PARAM, Protocol.reqGetParam(Protocol.PARAM_DEBUG_EVENTS, 0), frame -> {
            int[] p = Protocol.parseParamReply(frame.payload);
            if (eventsBefore == null) eventsBefore = p[2];
            enableEvents();
        }, status -> {
            if (status == GAVE_UP) lost("GET_PARAM debug.events timed out");
            else enableEvents();
        });
    }

    private void enableEvents() {
        request(Protocol.SET_PARAM, Protocol.reqSetParam(Protocol.PARAM_DEBUG_EVENTS, 0, 1), frame -> {
            int[] p = Protocol.parseParamReply(frame.payload);
            if (p[2] != 1) log("debug.events is " + p[2] + " after SET_PARAM 1");
            boolean wasReady = state == State.READY;
            state = State.READY;
            wait = keepaliveTicks;
            if (!wasReady) listener.onReady(this);
        }, status -> {
            if (status == GAVE_UP) {
                lost("SET_PARAM debug.events timed out");
            } else {
                log("SET_PARAM debug.events -> " + Protocol.statusName(status));
                state = State.UNSUPPORTED;
            }
        });
    }

    private void keepalive() {
        request(Protocol.GET_PARAM, Protocol.reqGetParam(Protocol.PARAM_DEBUG_EVENTS, 0), frame -> {
            int[] p = Protocol.parseParamReply(frame.payload);
            if (p[2] != 1) {
                // Rebooted or replugged (debug.events is volatile): its FB_* states are gone.
                lost("debug.events is off (device restarted?)");
                return;
            }
            wait = keepaliveTicks;
        }, status -> {
            if (status == GAVE_UP) lost("keep-alive timed out");
            else wait = keepaliveTicks;
        });
    }

    private void lost(String why) {
        log("lost device: " + why);
        boolean wasReady = state == State.READY;
        state = State.SEARCHING;
        pending = null;
        queue.clear();
        if (wasReady) listener.onLost(this);
        start();
    }

    private void log(String text) {
        listener.onLog(text);
    }
}
