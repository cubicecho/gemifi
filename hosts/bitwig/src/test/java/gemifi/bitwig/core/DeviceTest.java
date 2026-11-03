// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import gemifi.bitwig.core.Device.State;
import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Event;
import gemifi.bitwig.core.Protocol.Frame;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

class DeviceTest {
    final List<String> logs = new ArrayList<>();
    final List<Event> events = new ArrayList<>();
    int ready;
    int lost;

    Device connect(FakeDevice fake) {
        Device[] holder = new Device[1];
        Device d = new Device(fake::wire);
        holder[0] = d;
        fake.out = msg -> holder[0].receive(msg);
        d.setListener(new Device.Listener() {
            @Override
            public void onReady(Device device) {
                ready++;
            }

            @Override
            public void onLost(Device device) {
                lost++;
            }

            @Override
            public void onEvent(Event event) {
                events.add(event);
            }

            @Override
            public void onLog(String text) {
                logs.add(text);
            }
        });
        return d;
    }

    static void ticks(Device d, int n) {
        for (int i = 0; i < n; i++) d.tick();
    }

    @Test
    void enumeratesAndEnablesEvents() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        assertEquals(State.READY, d.state());
        assertEquals(1, ready);
        assertEquals("test-pad", d.hello().node);
        assertEquals(9, d.components(Protocol.KIND_BUTTON).size());
        assertEquals(3, d.components(Protocol.KIND_ANALOG).size());
        assertEquals(3, d.components(Protocol.KIND_ENCODER).size());
        assertEquals(9, d.components(Protocol.KIND_LED).size());
        assertEquals(0, d.components(Protocol.KIND_KEYS).size());
        assertEquals(1, (int) fake.params.get(Protocol.PARAM_DEBUG_EVENTS));
        // mcu: names normalised
        assertEquals("bank_next", d.components(Protocol.KIND_BUTTON).get(3).role);
        assertEquals("arm_2", d.components(Protocol.KIND_BUTTON).get(5).role);
        assertEquals("volume_2", d.components(Protocol.KIND_ANALOG).get(1).role);
        assertEquals("pan_1", d.components(Protocol.KIND_ENCODER).get(1).role);
        // one HELLO, 9+3+3+9 components + BAD_PARAM at index 0 for keys and displays, GET + SET
        assertEquals(1, fake.count(Protocol.HELLO));
        assertEquals(24 + 2, fake.count(Protocol.GET_COMPONENT));
        assertEquals(1, fake.count(Protocol.GET_PARAM));
        assertEquals(1, fake.count(Protocol.SET_PARAM));
    }

    @Test
    void seqIncreasesAndWraps() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        for (int i = 1; i < fake.requests.size(); i++) {
            assertEquals((fake.requests.get(i - 1).seq + 1) & 0x7F, fake.requests.get(i).seq);
        }
    }

    @Test
    void eventsOnlyWhenReady() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        fake.event(Protocol.KIND_BUTTON, 0, 1);       // before start: ignored
        d.start();
        fake.event(Protocol.KIND_BUTTON, 0, 1);
        fake.event(Protocol.KIND_ENCODER, 2, Protocol.intToS14(-3));
        assertEquals(2, events.size());
        assertEquals(-3, events.get(1).delta());
    }

    @Test
    void lostReplyIsResentWithSameSeq() {
        FakeDevice fake = new FakeDevice();
        fake.drop.add(Protocol.GET_COMPONENT + ":3");
        Device d = connect(fake);
        d.start();
        assertEquals(State.ENUMERATING, d.state());
        ticks(d, Device.TIMEOUT_TICKS);
        assertEquals(State.READY, d.state());
        List<Frame> comps = new ArrayList<>();
        for (Frame f : fake.requests) {
            if (f.cmd == Protocol.GET_COMPONENT) comps.add(f);
        }
        assertEquals(comps.get(2).seq, comps.get(3).seq);
        assertEquals(java.util.Arrays.toString(comps.get(2).payload), java.util.Arrays.toString(comps.get(3).payload));
    }

    @Test
    void busyIsResent() {
        FakeDevice fake = new FakeDevice();
        fake.busy.add(Protocol.HELLO + ":1");
        Device d = connect(fake);
        d.start();
        assertEquals(State.READY, d.state());
        assertEquals(2, fake.count(Protocol.HELLO));
        assertEquals(fake.requests.get(0).seq, fake.requests.get(1).seq);
    }

    @Test
    void silentDeviceIsRescanned() {
        FakeDevice fake = new FakeDevice();
        fake.silent = true;
        Device d = connect(fake);
        d.start();
        ticks(d, Device.TIMEOUT_TICKS * Device.MAX_TRIES);
        assertEquals(Device.MAX_TRIES, fake.count(Protocol.HELLO));
        assertEquals(State.SEARCHING, d.state());
        ticks(d, Device.RESCAN_TICKS - 1);
        assertEquals(Device.MAX_TRIES, fake.count(Protocol.HELLO));
        fake.silent = false;
        d.tick();
        assertEquals(State.READY, d.state());
    }

    @Test
    void noFeedbackCapMeansNoNotifications() {
        FakeDevice fake = new FakeDevice(FakeDevice.defaultComponents(), Protocol.CAP_CONFIG);
        Device d = connect(fake);
        d.start();
        assertEquals(State.UNSUPPORTED, d.state());
        assertEquals(0, fake.count(Protocol.GET_COMPONENT));
        assertFalse(d.notify(Protocol.FB_LED, Protocol.fbLed(0, 127)));
        d.release();
        assertTrue(fake.notifications.isEmpty());
    }

    @Test
    void bootloaderIsWaitedOut() {
        FakeDevice fake = new FakeDevice();
        fake.hello = new Protocol.Hello(1, 1, 0, 1, 256, Protocol.CAP_FW, new byte[12], null);
        Device d = connect(fake);
        d.start();
        assertEquals(State.SEARCHING, d.state());
        fake.hello = new Protocol.Hello(0, 1, 0, 1, 0, Protocol.CAP_FEEDBACK, new byte[12], null);
        ticks(d, Device.RESCAN_TICKS);
        assertEquals(State.READY, d.state());
    }

    @Test
    void keepaliveNoticesRestart() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        ticks(d, Device.KEEPALIVE_TICKS);
        assertEquals(State.READY, d.state());
        assertEquals(0, lost);
        fake.reboot();
        ticks(d, Device.KEEPALIVE_TICKS);
        assertEquals(1, lost);
        assertEquals(State.READY, d.state());               // found again right away
        assertEquals(2, ready);
        assertEquals(2, fake.count(Protocol.HELLO));
    }

    @Test
    void keepaliveTimeoutRestartsSearch() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        fake.silent = true;
        ticks(d, Device.KEEPALIVE_TICKS + Device.TIMEOUT_TICKS * Device.MAX_TRIES);
        assertEquals(1, lost);
        assertEquals(State.SEARCHING, d.state());
    }

    @Test
    void paramChangedOffTurnsEventsBackOn() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        fake.params.put(Protocol.PARAM_DEBUG_EVENTS, 0);
        fake.out.accept(Protocol.buildFrame(Protocol.PARAM_CHANGED, 3,
                Protocol.concat(Protocol.u14(Protocol.PARAM_DEBUG_EVENTS), Protocol.u14(0), Protocol.s32(0))));
        assertEquals(1, (int) fake.params.get(Protocol.PARAM_DEBUG_EVENTS));
        assertEquals(1, ready);
    }

    @Test
    void releaseSendsFbReleaseAndRestoresEvents() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        d.release();
        List<Frame> rel = fake.take(Protocol.FB_RELEASE);
        assertEquals(1, rel.size());
        assertEquals(Protocol.RELEASE_ALL, rel.get(0).payload[0]);
        assertEquals(0, rel.get(0).seq);
        assertEquals(0, (int) fake.params.get(Protocol.PARAM_DEBUG_EVENTS));
        assertEquals(State.IDLE, d.state());
    }

    @Test
    void releaseKeepsEventsOnIfTheyWereOn() {
        FakeDevice fake = new FakeDevice();
        fake.params.put(Protocol.PARAM_DEBUG_EVENTS, 1);
        Device d = connect(fake);
        d.start();
        int sets = fake.count(Protocol.SET_PARAM);
        d.release();
        assertEquals(sets, fake.count(Protocol.SET_PARAM));
        assertEquals(1, (int) fake.params.get(Protocol.PARAM_DEBUG_EVENTS));
    }

    @Test
    void notificationsDoNotWaitForReplies() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        fake.silent = true;
        ticks(d, Device.KEEPALIVE_TICKS);                   // a keep-alive is now outstanding
        assertTrue(d.notify(Protocol.FB_LED, Protocol.fbLed(3, 127)));
        assertEquals(1, fake.take(Protocol.FB_LED).size());
    }

    @Test
    void strayAndBadFramesAreIgnored() {
        FakeDevice fake = new FakeDevice();
        Device d = connect(fake);
        d.start();
        assertFalse(d.receive(Protocol.bytes(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7)));
        byte[] bad = Protocol.buildFrame(Protocol.HELLO | Protocol.REPLY, 99, Protocol.bytes(0));
        bad[bad.length - 2] ^= 1;
        assertTrue(d.receive(bad));
        assertTrue(d.receive(Protocol.buildFrame(Protocol.HELLO | Protocol.REPLY, 99, Protocol.bytes(0))));
        assertEquals(State.READY, d.state());
    }

    @Test
    void badParamOnFirstIndexMeansNone() {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.c(Protocol.KIND_ANALOG, "volume_1"));
        FakeDevice fake = new FakeDevice(comps, Protocol.CAP_FEEDBACK);
        Device d = connect(fake);
        d.start();
        assertEquals(1, d.allComponents().size());
        assertEquals(State.READY, d.state());
    }
}
