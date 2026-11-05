// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import gemifi.bitwig.core.Model.TrackFlag;
import gemifi.bitwig.core.Model.TrackParam;
import gemifi.bitwig.core.Model.TransportFlag;
import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Frame;
import gemifi.bitwig.core.Protocol.ProtocolException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/** Roles → behaviour and change-only feedback, through a real Device talking to a FakeDevice. */
class MappingTest {
    FakeDevice fake;
    Device device;
    FakeModel model;
    Mapping mapping;
    final List<String> logs = new ArrayList<>();

    void setUp(List<Component> comps) {
        fake = new FakeDevice(comps, Protocol.CAP_CONFIG | Protocol.CAP_EVENT | Protocol.CAP_FEEDBACK);
        device = new Device(fake::wire);
        fake.out = device::receive;
        model = new FakeModel(10);
        mapping = new Mapping(model, device::notify, logs::add);
        device.setListener(new Device.Listener() {
            @Override
            public void onReady(Device d) {
                mapping.bind(d.allComponents());
                mapping.refresh();
            }

            @Override
            public void onLost(Device d) {
                mapping.release();                                  // as GemifiExtension does
            }

            @Override
            public void onEvent(Protocol.Event ev) {
                mapping.handleEvent(ev);
            }
        });
        device.start();
        assertTrue(device.isReady());
    }

    @BeforeEach
    void defaults() {
        setUp(FakeDevice.defaultComponents());
    }

    /** LED index → level, from FB_LED and FB_LEDS since the last take. */
    Map<Integer, Integer> leds() throws ProtocolException {
        Map<Integer, Integer> out = new HashMap<>();
        for (Frame f : fake.take(-1)) {
            if (f.cmd == Protocol.FB_LED) {
                out.put(Protocol.decU14(f.payload, 0), (int) f.payload[2]);
            } else if (f.cmd == Protocol.FB_LEDS) {
                int first = Protocol.decU14(f.payload, 0);
                for (int i = 0; i < f.payload[2]; i++) out.put(first + i, (int) f.payload[3 + i]);
            }
        }
        return out;
    }

    /** (kind, index) → value of FB_VALUEs, and "ring index" → {mode, value, flags}, since the last take. */
    static final class Values {
        final Map<String, Integer> values = new HashMap<>();
        final Map<Integer, int[]> rings = new HashMap<>();
    }

    Values values() throws ProtocolException {
        Values v = new Values();
        for (Frame f : fake.take(-1)) {
            if (f.cmd == Protocol.FB_VALUE) {
                v.values.put(f.payload[0] + ":" + Protocol.decU14(f.payload, 1), Protocol.decU14(f.payload, 3));
            } else if (f.cmd == Protocol.FB_RING) {
                v.rings.put(Protocol.decU14(f.payload, 0),
                        new int[] {f.payload[2], Protocol.decU14(f.payload, 3), f.payload[5]});
            }
        }
        return v;
    }

    void press(int button) {
        fake.event(Protocol.KIND_BUTTON, button, 1);
        fake.event(Protocol.KIND_BUTTON, button, 0);
    }

    @Test
    void initialFeedbackGoesOutOnce() throws ProtocolException {
        mapping.refresh();
        // LEDs 0..7 changed from "unknown": sent as one FB_LEDS run (LED 7 is the metronome LED).
        List<Frame> all = new ArrayList<>(fake.notifications);
        Map<Integer, Integer> l = leds();
        assertEquals(8, l.size());
        assertEquals(Mapping.OFF, (int) l.get(0));                  // not playing
        assertEquals(Mapping.ON, (int) l.get(3));                   // bank_next: 10 tracks > 8
        assertEquals(Mapping.ON, (int) l.get(6));                   // select_1: track 1 is selected
        long runs = all.stream().filter(f -> f.cmd == Protocol.FB_LEDS).count();
        assertEquals(1, runs);
        mapping.refresh();
        assertTrue(fake.take(-1).isEmpty(), "nothing changed: nothing sent");
    }

    @Test
    void unknownAndUnsupportedRolesAreLoggedAndXRolesIgnored() {
        List<Component> comps = FakeDevice.defaultComponents();
        comps.add(FakeDevice.button("frobnicate", Protocol.NONE_INDEX));
        comps.add(FakeDevice.c(Protocol.KIND_ANALOG, "cue_volume"));
        comps.add(FakeDevice.button("zynthian.toggle_audio_play", Protocol.NONE_INDEX));  // another host's
        comps.add(FakeDevice.button("bitwig.not_yet", Protocol.NONE_INDEX));
        setUp(comps);
        assertEquals(2, mapping.unknownRoles().size());
        assertTrue(mapping.unknownRoles().get(0).endsWith("frobnicate"));
        assertTrue(mapping.unknownRoles().get(1).endsWith("bitwig.not_yet"));
        assertTrue(logs.stream().noneMatch(s -> s.contains("zynthian.")));
        assertEquals(1, mapping.unsupportedRoles().size());
        assertTrue(logs.stream().anyMatch(s -> s.contains("cue_volume")));
        assertTrue(logs.stream().noneMatch(s -> s.contains("x_custom")));
    }

    @Test
    void playTogglesAndLights() throws ProtocolException {
        mapping.refresh();
        fake.take(-1);
        press(0);
        assertTrue(model.flag(TransportFlag.PLAYING));
        assertEquals(Mapping.ON, (int) leds().get(0));
        press(0);
        assertFalse(model.flag(TransportFlag.PLAYING));
        assertEquals(Mapping.OFF, (int) leds().get(0));
    }

    @Test
    void stopIsMomentary() throws ProtocolException {
        fake.take(-1);
        fake.event(Protocol.KIND_BUTTON, 1, 1);
        assertEquals(Mapping.ON, (int) leds().get(1));
        fake.event(Protocol.KIND_BUTTON, 1, 0);
        assertEquals(Mapping.OFF, (int) leds().get(1));
        assertEquals(List.of("stop"), model.calls);
    }

    @Test
    void recordBlinksWhileArmedAndStopped() throws ProtocolException {
        fake.take(-1);
        press(2);
        assertEquals(Mapping.BLINK, (int) leds().get(2));
        press(0);                                                   // play
        assertEquals(Mapping.ON, (int) leds().get(2));
    }

    @Test
    void standaloneLedFollowsItsRole() throws ProtocolException {
        fake.take(-1);
        model.setFlag(TransportFlag.METRONOME, true);
        mapping.refresh();
        Map<Integer, Integer> l = leds();
        assertEquals(1, l.size());
        assertEquals(Mapping.ON, (int) l.get(7));
    }

    @Test
    void bankStepAndStripRoles() throws ProtocolException {
        fake.take(-1);
        press(4);                                                   // arm_1
        assertTrue(model.tracks.get(0).flag(TrackFlag.ARM));
        press(5);                                                   // rec_2 -> arm_2
        assertTrue(model.tracks.get(1).flag(TrackFlag.ARM));
        leds();
        press(3);                                                   // bank_next
        assertEquals(8, model.bankOffset);
        Map<Integer, Integer> l = leds();
        assertEquals(Mapping.OFF, (int) l.get(3));                  // no further bank
        assertEquals(Mapping.OFF, (int) l.get(4));                  // track 9 not armed
        assertEquals(Mapping.OFF, (int) l.get(6));                  // track 9 not selected
        press(6);                                                   // select_1 -> track 9
        assertEquals(8, model.selected);
        assertEquals(Mapping.ON, (int) leds().get(6));
    }

    @Test
    void armIsSkippedWhenTrackCantArm() {
        model.tracks.get(0).canArm = false;
        press(4);
        assertFalse(model.tracks.get(0).flag(TrackFlag.ARM));
    }

    @Test
    void rewindRepeatsWhileHeld() {
        fake.event(Protocol.KIND_BUTTON, 7, 1);
        assertEquals(-4.0, model.position);
        for (int i = 0; i < Mapping.REPEAT_DELAY_TICKS - 1; i++) mapping.tick();
        assertEquals(-4.0, model.position);
        mapping.tick();
        assertEquals(-8.0, model.position);
        mapping.tick();
        assertEquals(-12.0, model.position);
        fake.event(Protocol.KIND_BUTTON, 7, 0);
        mapping.tick();
        assertEquals(-12.0, model.position);
    }

    @Test
    void analogPickup() throws ProtocolException {
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(0).param(TrackParam.VOLUME);   // 0.85
        Values v0 = values();
        assertEquals(Mapping.to14(0.85), (int) v0.values.get("3:0"));        // pick-up target sent
        fake.event(Protocol.KIND_ANALOG, 0, 2000);                           // far away: nothing
        assertEquals(0.85, vol.value, 1e-9);
        fake.event(Protocol.KIND_ANALOG, 0, Mapping.to14(0.85) - 100);       // within 3 %: picked up
        assertEquals((Mapping.to14(0.85) - 100) / 16383.0, vol.value, 1e-9);
        fake.event(Protocol.KIND_ANALOG, 0, 0);
        assertEquals(0.0, vol.value, 1e-9);
        mapping.refresh();
        // (analog 2, track_volume, has the same target and does get it)
        assertFalse(values().values.containsKey("3:0"), "what the knob set is not echoed");
        // the value changes elsewhere: new target, pick-up again
        vol.value = 0.5;
        for (int i = 0; i < Mapping.SETTLE_TICKS; i++) mapping.tick();      // after the knob's settle time
        assertEquals(Mapping.to14(0.5), (int) values().values.get("3:0"));
        fake.event(Protocol.KIND_ANALOG, 0, 100);
        assertEquals(0.5, vol.value, 1e-9);
    }

    @Test
    void analogPickupByCrossing() {
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(1).param(TrackParam.VOLUME);   // fader_2
        fake.event(Protocol.KIND_ANALOG, 1, 10000);
        fake.event(Protocol.KIND_ANALOG, 1, 16000);                          // crossed 0.85*16383
        assertEquals(16000 / 16383.0, vol.value, 1e-9);
    }

    @Test
    void analogWithDeferredHostValue() throws ProtocolException {
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(0).param(TrackParam.VOLUME);
        vol.deferred = true;
        values();
        int start = Mapping.to14(0.85);
        fake.event(Protocol.KIND_ANALOG, 0, start);
        fake.event(Protocol.KIND_ANALOG, 0, start - 300);
        mapping.refresh();                                                   // host still reports the old value
        assertFalse(values().values.containsKey("3:0"));
        vol.apply();
        for (int i = 0; i < Mapping.SETTLE_TICKS + 1; i++) mapping.tick();
        assertFalse(values().values.containsKey("3:0"), "the host caught up: no FB_VALUE, still picked up");
        fake.event(Protocol.KIND_ANALOG, 0, start - 600);
        vol.apply();
        assertEquals((start - 600) / 16383.0, vol.value, 1e-9);
    }

    @Test
    void selectedTrackFollowsSelection() throws ProtocolException {
        values();
        model.tracks.get(3).params.get(TrackParam.VOLUME).value = 0.25;
        model.selected = 3;
        mapping.refresh();
        assertEquals(Mapping.to14(0.25), (int) values().values.get("3:2"));
    }

    @Test
    void encoderNudgesAndDrivesTheRing() throws ProtocolException {
        Values v0 = values();
        int[] ring0 = v0.rings.get(0);                                       // device_param_1 = 0.0, wrap
        assertEquals(Protocol.RING_WRAP, ring0[0]);
        assertEquals(0, ring0[2]);
        int[] pan = v0.rings.get(1);                                         // pan_1: bipolar
        assertEquals(Protocol.RING_BOOST_CUT, pan[0]);
        assertEquals(Mapping.to14(0.5), pan[1]);
        assertEquals(1, pan[2]);                                             // centre LED
        assertFalse(v0.rings.containsKey(2), "tempo encoder has no ring");

        fake.event(Protocol.KIND_ENCODER, 0, 3);
        FakeModel.FParam p = (FakeModel.FParam) model.deviceParam(0);
        assertEquals(3.0 / Mapping.ENCODER_STEPS, p.value, 1e-9);
        Values v1 = values();
        assertEquals(Mapping.to14(3.0 / Mapping.ENCODER_STEPS), (int) v1.values.get("4:0"));
        assertEquals(Mapping.to14(3.0 / Mapping.ENCODER_STEPS), v1.rings.get(0)[1]);

        fake.event(Protocol.KIND_ENCODER, 1, Protocol.intToS14(-64));       // pan all the way left
        assertEquals(0.0, model.tracks.get(0).params.get(TrackParam.PAN).value, 1e-9);
    }

    @Test
    void ringOffWhenNothingMapped() throws ProtocolException {
        values();
        model.device = 5;                                                    // no device
        mapping.refresh();
        int[] r = values().rings.get(0);
        assertEquals(Protocol.RING_WRAP, r[0]);
        assertEquals(0, r[1]);
        assertEquals(2, r[2]);                                               // flags bit 1: ring off
        mapping.refresh();
        assertTrue(values().rings.isEmpty());
    }

    @Test
    void devicePagingAndDeviceRoles() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.button("device_bank_next", 0));
        comps.add(FakeDevice.button("device_bank_prev", 1));
        comps.add(FakeDevice.button("device_next", 2));
        comps.add(FakeDevice.button("device_on", 3));
        comps.add(FakeDevice.encoder("device_param_1", 11));
        comps.add(FakeDevice.encoder("device_param_8", 11));
        for (int i = 0; i < 4; i++) comps.add(FakeDevice.led(""));
        setUp(comps);
        Map<Integer, Integer> l = leds();
        assertEquals(Mapping.ON, (int) l.get(0));
        assertEquals(Mapping.OFF, (int) l.get(1));
        assertEquals(Mapping.ON, (int) l.get(3));                            // device on
        press(0);                                                            // page 2: params 8..11
        assertEquals(1, model.page);
        assertEquals(2, (int) values().rings.get(1)[2]);                     // param 16 doesn't exist: ring off
        fake.event(Protocol.KIND_ENCODER, 0, 1);
        assertEquals(0.8 + 1.0 / Mapping.ENCODER_STEPS, model.devices.get(0).get(8).value, 1e-9);
        press(3);
        assertFalse(model.deviceOn.get(0));
        press(2);                                                            // next device: page resets
        assertEquals(1, model.device);
        assertEquals(0, model.page);
    }

    @Test
    void transportExtras() {
        List<Component> comps = new ArrayList<>();
        for (String r : new String[] {"loop", "metronome", "overdub", "session_record", "automation_arm",
            "tap_tempo", "undo", "redo", "scene_next", "scene_launch", "stop_all_clips", "track_next",
            "track_mute", "track_solo", "track_arm", "cycle", "click", "channel_right", "bank_left"}) {
            comps.add(FakeDevice.button(r, Protocol.NONE_INDEX));
        }
        setUp(comps);
        assertTrue(mapping.unknownRoles().isEmpty(), mapping.unknownRoles().toString());
        for (int i = 0; i < 8; i++) press(i);
        assertTrue(model.flag(TransportFlag.LOOP));
        assertTrue(model.flag(TransportFlag.METRONOME));
        assertTrue(model.flag(TransportFlag.OVERDUB));
        assertTrue(model.flag(TransportFlag.LAUNCHER_OVERDUB));
        assertTrue(model.flag(TransportFlag.AUTOMATION_WRITE));
        press(8);
        press(9);
        press(10);
        assertEquals(List.of("tap", "undo", "redo", "launch 1", "stop_all"), model.calls);
        press(11);
        assertEquals(1, model.selected);
        press(12);
        press(13);
        press(14);
        assertTrue(model.tracks.get(1).flag(TrackFlag.MUTE));
        assertTrue(model.tracks.get(1).flag(TrackFlag.SOLO));
        assertTrue(model.tracks.get(1).flag(TrackFlag.ARM));
        press(15);                                                           // cycle -> loop: off again
        assertFalse(model.flag(TransportFlag.LOOP));
    }

    @Test
    void masterTempoAndCrossfader() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.c(Protocol.KIND_ANALOG, "master"));            // mcu: -> master_volume
        comps.add(FakeDevice.encoder("crossfader", 15));
        comps.add(FakeDevice.encoder("tempo", 0));
        setUp(comps);
        Values v = values();
        assertEquals(Mapping.to14(0.7), (int) v.values.get("3:0"));
        assertEquals(Protocol.RING_BOOST_CUT, v.rings.get(0)[0]);
        fake.event(Protocol.KIND_ENCODER, 1, 2);
        assertEquals(0.2 + 2.0 / Mapping.ENCODER_STEPS, model.tempo.value, 1e-9);
    }

    @Test
    void releaseForgetsSoNextReadySendsEverything() throws ProtocolException {
        mapping.refresh();
        fake.take(-1);
        mapping.release();
        mapping.refresh();
        assertEquals(8, leds().size());
    }

    @Test
    void ledRunsSplitOnGaps() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.button("loop", 0));
        comps.add(FakeDevice.button("metronome", 1));
        comps.add(FakeDevice.button("overdub", 5));
        for (int i = 0; i < 6; i++) comps.add(FakeDevice.led(""));
        setUp(comps);
        List<Frame> f = fake.take(-1);
        assertEquals(2, f.size());
        assertEquals(Protocol.FB_LEDS, f.get(0).cmd);
        assertEquals(Protocol.FB_LED, f.get(1).cmd);
    }

    @Test
    void stripNumbers() {
        assertEquals(3, Mapping.stripNumber("volume_3", "volume_"));
        assertEquals(0, Mapping.stripNumber("volume_9", "volume_"));
        assertEquals(0, Mapping.stripNumber("volume_0", "volume_"));
        assertEquals(0, Mapping.stripNumber("volume_x", "volume_"));
        assertEquals(0, Mapping.stripNumber("volume_", "volume_"));
    }

    @Test
    void addedFieldsLimitWhatIsSent() throws ProtocolException {
        // sysex.md §4.6.1: no FB_VALUE to an analog input without pick-up or a relative encoder, and
        // nothing to an owner: local LED (its button still works).
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.button("play", 0));
        comps.add(FakeDevice.button("stop", 1));
        comps.add(FakeDevice.analog("volume_1", Protocol.TAKEOVER_NONE));
        comps.add(FakeDevice.analog("volume_2", Protocol.TAKEOVER_PICKUP));
        comps.add(FakeDevice.encoder("device_param_1", 5, Protocol.ENC_REL_TWOS));
        comps.add(FakeDevice.encoder("pan_1", 0, Protocol.ENC_ABSOLUTE));
        comps.add(FakeDevice.encoder("device_param_2", 5, Protocol.ENC_MCU));
        comps.add(FakeDevice.led("", Protocol.OWNER_LOCAL));
        comps.add(FakeDevice.led("", Protocol.OWNER_HOST_THEN_LOCAL));
        comps.add(FakeDevice.led("metronome", Protocol.OWNER_LOCAL));
        setUp(comps);
        List<Frame> first = new ArrayList<>(fake.notifications);
        Values v = values();
        assertFalse(v.values.containsKey("3:0"), "takeover none: no FB_VALUE");
        assertTrue(v.values.containsKey("3:1"));
        assertFalse(v.values.containsKey("4:0"), "relative encoder: no FB_VALUE");
        assertTrue(v.rings.containsKey(0), "its ring still follows");
        assertTrue(v.values.containsKey("4:1"));
        assertFalse(v.values.containsKey("4:2"), "Mackie V-Pot counts as relative");
        assertTrue(v.rings.containsKey(2));
        fake.notifications.addAll(first);
        assertEquals(Set.of(1), leds().keySet());                            // LEDs 0 and 2 are local
        press(0);                                                            // play still works
        assertTrue(model.flag(TransportFlag.PLAYING));
        assertTrue(leds().isEmpty());
        // host-side pick-up still applies to the analog input without device pick-up
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(0).param(TrackParam.VOLUME);   // 0.85
        fake.event(Protocol.KIND_ANALOG, 0, 1000);
        assertEquals(0.85, vol.value, 1e-9);
        // the relative encoder still nudges, and its ring moves
        fake.event(Protocol.KIND_ENCODER, 0, 3);
        Values v1 = values();
        assertFalse(v1.values.containsKey("4:0"));
        assertEquals(Mapping.to14(3.0 / Mapping.ENCODER_STEPS), v1.rings.get(0)[1]);
    }

    @Test
    void olderFirmwareGetsEverything() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.analog("volume_1", null));
        comps.add(FakeDevice.encoder("device_param_1", 5, null));
        comps.add(FakeDevice.led("metronome", null));
        setUp(comps);
        List<Frame> first = new ArrayList<>(fake.notifications);
        Values v = values();
        assertTrue(v.values.containsKey("3:0"));
        assertTrue(v.values.containsKey("4:0"));
        fake.notifications.addAll(first);
        assertEquals(Set.of(0), leds().keySet());
    }

    @Test
    void bootedResendsEverything() throws ProtocolException {
        mapping.refresh();
        fake.take(-1);
        fake.boot();                                                         // re-enumerates: onLost + onReady
        assertTrue(device.isReady());
        assertEquals(8, leds().size());
    }

    @Test
    void stripPrevNextMoveTheBankByOne() throws ProtocolException {
        List<Component> comps = FakeDevice.defaultComponents();
        comps.add(FakeDevice.button("channel_left", Protocol.NONE_INDEX));    // mcu: name -> strip_prev (9)
        comps.add(FakeDevice.button("strip_next", Protocol.NONE_INDEX));      // 10
        setUp(comps);
        press(10);
        assertEquals(1, model.bankOffset);
        press(10);
        press(10);
        press(10);                                                  // 10 tracks: stops at 2
        assertEquals(2, model.bankOffset);
        press(9);
        assertEquals(1, model.bankOffset);
    }

    @Test
    void touchAndPadEventsAreIgnored() {
        fake.take(-1);
        fake.event(5, 0, 1);                                        // touch (daw-scripts.md §4.5)
        fake.event(7, 0, 9000);                                     // a pad twin's hit
        fake.event(7, 0, 0);
        assertTrue(fake.take(-1).isEmpty());
    }

    // ---- layers (daw-scripts.md §5.11) ----

    @Test
    void twinsWithOneRoleBothDriveItAndGetFeedback() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.encoder("", 0));                                        // base: plain CC, no role
        comps.add(FakeDevice.twin(FakeDevice.encoder("zynthian.zynpot_1", 0), 2));  // another host's: ignored
        comps.add(FakeDevice.twin(FakeDevice.encoder("device_param_1", 0), 2));     // mode 1
        comps.add(FakeDevice.twin(FakeDevice.encoder("device_param_1", 0), 3));     // mode 2
        comps.add(FakeDevice.twin(FakeDevice.button("device_bank_next", 0), 2));
        comps.add(FakeDevice.twin(FakeDevice.button("device_bank_next", 1), 3));
        for (int i = 0; i < 2; i++) comps.add(FakeDevice.led(""));
        setUp(comps);
        assertTrue(mapping.unknownRoles().isEmpty());
        Map<Integer, Integer> l = leds();
        assertEquals(Mapping.ON, (int) l.get(0));                                     // both twins' LEDs
        assertEquals(Mapping.ON, (int) l.get(1));
        fake.event(Protocol.EVENT_LAYER, 1, 1);                                       // known, ignored
        Model.Param p = model.deviceParam(0);
        double before = p.get();
        fake.event(Protocol.KIND_ENCODER, 2, 1);
        fake.event(Protocol.KIND_ENCODER, 3, 1);
        assertEquals(before + 2.0 / Mapping.ENCODER_STEPS, p.get(), 1e-9);
        fake.take(-1);
        ((FakeModel.FParam) p).value = 0.25;
        mapping.refresh();
        Values v = values();
        assertEquals(Mapping.to14(0.25), (int) v.values.get(Protocol.KIND_ENCODER + ":2"));
        assertEquals(Mapping.to14(0.25), (int) v.values.get(Protocol.KIND_ENCODER + ":3"));
        assertEquals(2, v.values.size());
    }

    // ---- rate ----

    @Test
    void flushSendsOneValuePerControlPerTick() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.analog("volume_1", Protocol.TAKEOVER_PICKUP));
        comps.add(FakeDevice.encoder("device_param_1", 11, Protocol.ENC_ABSOLUTE));
        comps.add(FakeDevice.button("play", 0));
        comps.add(FakeDevice.led(""));
        setUp(comps);
        mapping.tick();
        fake.take(-1);
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(0).param(TrackParam.VOLUME);
        FakeModel.FParam dp = (FakeModel.FParam) model.deviceParam(0);
        int values = 0;
        int rings = 0;
        int leds = 0;
        for (int i = 1; i <= 10; i++) {                  // automation: ten flushes within one tick
            vol.value = 0.05 * i;
            dp.value = 0.05 * i;
            model.setFlag(TransportFlag.PLAYING, i % 2 == 1);
            mapping.flush();
            for (Frame f : fake.take(-1)) {
                if (f.cmd == Protocol.FB_VALUE) values++;
                if (f.cmd == Protocol.FB_RING) rings++;
                if (f.cmd == Protocol.FB_LED) leds++;
            }
        }
        assertEquals(2, values, "the first change of each control goes out at once");
        assertEquals(10, rings, "rings follow every flush");
        assertEquals(10, leds, "LEDs follow every flush");
        mapping.tick();                                  // the latest values
        Values v = values();
        assertEquals(Mapping.to14(0.5), (int) v.values.get("3:0"));
        assertEquals(Mapping.to14(0.5), (int) v.values.get("4:0"));
        mapping.flush();
        mapping.tick();
        assertTrue(fake.take(-1).isEmpty(), "nothing changed: nothing sent");
    }

    @Test
    void heldBackValueStillResetsPickup() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.analog("volume_1", Protocol.TAKEOVER_PICKUP));
        setUp(comps);
        FakeModel.FParam vol = (FakeModel.FParam) model.tracks.get(0).param(TrackParam.VOLUME);   // 0.85
        fake.event(Protocol.KIND_ANALOG, 0, Mapping.to14(0.85));                 // picked up
        for (int i = 0; i < Mapping.SETTLE_TICKS; i++) mapping.tick();
        vol.value = 0.5;
        mapping.flush();                                                         // sent
        vol.value = 0.2;
        mapping.flush();                                                         // held back
        assertEquals(Mapping.to14(0.5), (int) values().values.get("3:0"));
        fake.event(Protocol.KIND_ANALOG, 0, Mapping.to14(0.5));                  // the knob at the old target
        assertEquals(0.2, vol.value, 1e-9, "not picked up: the target moved on");
        mapping.tick();
        assertEquals(Mapping.to14(0.2), (int) values().values.get("3:0"));
        fake.event(Protocol.KIND_ANALOG, 0, Mapping.to14(0.2));
        assertEquals(Mapping.to14(0.2) / 16383.0, vol.value, 1e-9);
    }
}
