// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Frame;
import gemifi.bitwig.core.Protocol.Hello;
import gemifi.bitwig.core.Protocol.ProtocolException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;

/** A gemifi device that parses what the extension sends and answers like firmware with caps bit 6. */
final class FakeDevice {
    static Component c(int kind, String role) {
        Component c = new Component(kind, 0, 0, Protocol.kindName(kind), "", role);
        return c;
    }

    static Component button(String role, int led) {
        Component c = c(Protocol.KIND_BUTTON, role);
        c.led = led;
        return c;
    }

    static Component encoder(String role, int ring) {
        Component c = c(Protocol.KIND_ENCODER, role);
        c.ring = ring;
        return c;
    }

    static Component led(String role) {
        return c(Protocol.KIND_LED, role);
    }

    /** A small device using several roles (plus mcu: names and an x_ role). */
    static List<Component> defaultComponents() {
        List<Component> l = new ArrayList<>();
        l.add(button("play", 0));
        l.add(button("stop", 1));
        l.add(button("record", 2));
        l.add(button("bank_right", 3));         // mcu: name -> bank_next
        l.add(button("arm_1", 4));
        l.add(button("rec_2", 5));              // mcu: name -> arm_2
        l.add(button("select_1", 6));
        l.add(button("rewind", Protocol.NONE_INDEX));
        l.add(button("x_custom", Protocol.NONE_INDEX));
        l.add(c(Protocol.KIND_ANALOG, "volume_1"));
        l.add(c(Protocol.KIND_ANALOG, "fader_2"));   // mcu: name -> volume_2
        l.add(c(Protocol.KIND_ANALOG, "track_volume"));
        l.add(encoder("device_param_1", 11));
        l.add(encoder("vpot_1", 11));          // mcu: name -> pan_1
        l.add(encoder("tempo", 0));
        for (int i = 0; i < 7; i++) l.add(led(""));  // LEDs 0..6 belong to the buttons above
        l.add(led("metronome"));                // LED 7 stands alone
        l.add(led(""));
        return l;
    }

    final Map<Integer, List<Component>> byKind = new HashMap<>();
    Hello hello;
    final Map<Integer, Integer> params = new HashMap<>();
    final List<Frame> requests = new ArrayList<>();
    final List<Frame> notifications = new ArrayList<>();
    /** "cmd:n" = the reply to the n-th request with that cmd is lost. */
    final Set<String> drop = new HashSet<>();
    /** "cmd:n" = the n-th request with that cmd gets BUSY. */
    final Set<String> busy = new HashSet<>();
    boolean silent;
    private final Map<Integer, Integer> count = new HashMap<>();
    Consumer<byte[]> out;

    FakeDevice() {
        this(defaultComponents(), Protocol.CAP_CONFIG | Protocol.CAP_EVENT | Protocol.CAP_FEEDBACK);
    }

    FakeDevice(List<Component> comps, int caps) {
        for (Component c : comps) byKind.computeIfAbsent(c.kind, k -> new ArrayList<>()).add(c);
        for (List<Component> l : byKind.values()) {
            for (int i = 0; i < l.size(); i++) {      // the device numbers them 0..count-1 per kind
                l.get(i).index = i;
                l.get(i).count = l.size();
            }
        }
        hello = new Hello(0, 0x3F00, 2 << 16, 1, 0, caps, new byte[12], "test-pad");
        params.put(Protocol.PARAM_DEBUG_EVENTS, 0);
    }

    /** The extension sent {@code data}. */
    void wire(byte[] data) {
        Frame f;
        try {
            f = Protocol.parseFrame(data);
        } catch (ProtocolException e) {
            throw new AssertionError("extension sent a bad frame: " + e.getMessage());
        }
        if (f.isNotification()) {
            notifications.add(f);
            return;
        }
        requests.add(f);
        int n = count.merge(f.cmd, 1, Integer::sum);
        if (silent || drop.contains(f.cmd + ":" + n)) return;
        byte[] reply = busy.contains(f.cmd + ":" + n) ? Protocol.bytes(Protocol.BUSY) : answer(f);
        if (reply != null && out != null) out.accept(Protocol.buildFrame(f.cmd | Protocol.REPLY, f.seq, reply));
    }

    private byte[] answer(Frame f) {
        try {
            switch (f.cmd) {
                case Protocol.HELLO:
                    return Protocol.buildHelloReply(hello);
                case Protocol.GET_COMPONENT: {
                    if ((hello.caps & Protocol.CAP_FEEDBACK) == 0) return Protocol.bytes(Protocol.BAD_CMD);
                    int kind = f.payload[0];
                    int index = Protocol.decU14(f.payload, 1);
                    List<Component> l = byKind.getOrDefault(kind, new ArrayList<>());
                    if (index >= l.size()) return Protocol.bytes(Protocol.BAD_PARAM);
                    return Protocol.buildComponentReply(l.get(index));
                }
                case Protocol.GET_PARAM:
                case Protocol.SET_PARAM: {
                    int id = Protocol.decU14(f.payload, 0);
                    int index = Protocol.decU14(f.payload, 2);
                    if (!params.containsKey(id)) return Protocol.bytes(Protocol.BAD_PARAM);
                    if (f.cmd == Protocol.SET_PARAM) params.put(id, Protocol.decS32(f.payload, 4));
                    return Protocol.buildParamReply(id, index, params.get(id));
                }
                default:
                    return Protocol.bytes(Protocol.BAD_CMD);
            }
        } catch (ProtocolException e) {
            return Protocol.bytes(Protocol.BAD_LEN);
        }
    }

    void event(int type, int index, int value) {
        out.accept(Protocol.buildFrame(Protocol.EVENT, 0, Protocol.buildEvent(type, index, value, 1234)));
    }

    /** Pops the notifications so far with command {@code cmd} (-1 = all). */
    List<Frame> take(int cmd) {
        List<Frame> got = new ArrayList<>();
        for (Frame f : notifications) {
            if (cmd < 0 || f.cmd == cmd) got.add(f);
        }
        notifications.clear();
        return got;
    }

    int count(int cmd) {
        int n = 0;
        for (Frame f : requests) {
            if (f.cmd == cmd) n++;
        }
        return n;
    }

    /** Simulates a power cycle: volatile debug.events goes back to 0. */
    void reboot() {
        params.put(Protocol.PARAM_DEBUG_EVENTS, 0);
    }
}
