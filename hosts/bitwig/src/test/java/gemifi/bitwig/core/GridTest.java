// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import gemifi.bitwig.core.Protocol.Component;
import gemifi.bitwig.core.Protocol.Frame;
import gemifi.bitwig.core.Protocol.ProtocolException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/** Pads and the clip grid (daw-scripts.md §5.9), through a real Device talking to a FakeDevice. */
class GridTest {
    FakeDevice fake;
    Device device;
    FakeModel model;
    Mapping mapping;
    final List<String> logs = new ArrayList<>();

    // pads 0..3: base pads (play notes, no role); 4..7: their twins in mode 0, a 2 x 2 grid
    static final int CLIP_1_1 = 4, CLIP_2_1 = 5, CLIP_1_2 = 6, CLIP_2_2 = 7;
    // buttons
    static final int LEFT = 0, RIGHT = 1, UP = 2, DOWN = 3, SCENE_1 = 4, SCENE_2 = 5;

    /** A Code 61 sized down: 4 pads with twins, grid and scene buttons, LEDs 0..11. */
    static List<Component> gridComponents() {
        List<Component> l = new ArrayList<>();
        for (int i = 0; i < 4; i++) {
            Component base = FakeDevice.pad("", i);
            base.layer = 0;
            l.add(base);
        }
        String[] roles = {"clip_1_1", "clip_2_1", "clip_1_2", "clip_2_2"};
        for (int i = 0; i < 4; i++) l.add(FakeDevice.twin(FakeDevice.pad(roles[i], 4 + i), 1));
        l.add(FakeDevice.button("grid_left", 8));
        l.add(FakeDevice.button("grid_right", 9));
        l.add(FakeDevice.button("grid_up", 10));
        l.add(FakeDevice.button("grid_down", 11));
        l.add(FakeDevice.button("scene_1", Protocol.NONE_INDEX));
        l.add(FakeDevice.button("scene_2", Protocol.NONE_INDEX));
        for (int i = 0; i < 12; i++) l.add(FakeDevice.led(""));
        return l;
    }

    void setUp(List<Component> comps) {
        fake = new FakeDevice(comps, Protocol.CAP_CONFIG | Protocol.CAP_EVENT | Protocol.CAP_FEEDBACK);
        device = new Device(fake::wire);
        fake.out = device::receive;
        model = new FakeModel(10);                  // 10 tracks, 4 scenes
        mapping = new Mapping(model, device::notify, logs::add);
        device.setListener(new Device.Listener() {
            @Override
            public void onReady(Device d) {
                mapping.bind(d.allComponents());
                mapping.refresh();
            }

            @Override
            public void onLost(Device d) {
                mapping.release();
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
        setUp(gridComponents());
    }

    /** LED → {r, g, b, flags} from FB_LED_RGB since the last take. */
    Map<Integer, int[]> rgb() throws ProtocolException {
        Map<Integer, int[]> out = new HashMap<>();
        for (Frame f : fake.take(Protocol.FB_LED_RGB)) {
            out.put(Protocol.decU14(f.payload, 0), new int[] {f.payload[2], f.payload[3], f.payload[4], f.payload[5]});
        }
        return out;
    }

    /** LED index → level from FB_LED / FB_LEDS since the last take. */
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

    void pad(int index) {
        fake.event(Protocol.KIND_PAD, index, 9000);
        fake.event(Protocol.KIND_PAD, index, 0);
    }

    void press(int button) {
        fake.event(Protocol.KIND_BUTTON, button, 1);
        fake.event(Protocol.KIND_BUTTON, button, 0);
    }

    @Test
    void padsAreEnumeratedWithTheirLayer() {
        List<Component> pads = device.components(Protocol.KIND_PAD);
        assertEquals(8, pads.size());
        assertEquals(8, pads.get(0).count);
        assertEquals(0, (int) pads.get(0).layer);
        assertEquals(1, (int) pads.get(CLIP_1_1).layer);
        assertEquals("clip_1_1", pads.get(CLIP_1_1).role);
        assertEquals(4, pads.get(CLIP_1_1).led);
        assertTrue(mapping.unknownRoles().isEmpty());
        assertEquals(2, model.gridWidth, "the grid is sized from the roles");
        assertEquals(2, model.gridHeight);
    }

    @Test
    void twinPressLaunchesAndStopsAClip() {
        model.slot(1, 0).state = Model.SLOT_CONTENT;
        pad(CLIP_2_1);
        assertEquals(List.of("launch clip 1 0"), model.calls, "the release does nothing");
        model.calls.clear();
        model.slot(1, 0).state = Model.SLOT_CONTENT | Model.SLOT_PLAYING;
        pad(CLIP_2_1);
        assertEquals(List.of("stop track 1"), model.calls);
        model.calls.clear();
        model.slot(1, 0).state = Model.SLOT_CONTENT | Model.SLOT_RECORDING;
        pad(CLIP_2_1);
        assertEquals(List.of("stop track 1"), model.calls, "a recording clip stops too");
    }

    @Test
    void emptySlotPressDoesNothing() {
        pad(CLIP_1_2);
        fake.event(Protocol.KIND_PAD, 0, 9000);             // a base pad never sends one; unbound anyway
        fake.event(Protocol.KIND_PAD, 99, 9000);
        assertTrue(model.calls.isEmpty());
    }

    @Test
    void sceneNLaunchesTheRowAtTheGridPosition() {
        press(SCENE_2);
        assertEquals(List.of("launch grid scene 1"), model.calls);
        model.calls.clear();
        press(DOWN);
        press(SCENE_1);
        press(SCENE_2);
        assertEquals(List.of("launch grid scene 1", "launch grid scene 2"), model.calls);
    }

    @Test
    void gridScrollsByOneAndClamps() throws ProtocolException {
        Map<Integer, Integer> l = leds();                         // from the first refresh
        assertEquals(Mapping.OFF, (int) l.get(8));                // grid_left: at 0
        assertEquals(Mapping.ON, (int) l.get(9));                 // grid_right: 10 tracks > 2
        assertEquals(Mapping.OFF, (int) l.get(10));
        assertEquals(Mapping.ON, (int) l.get(11));                // grid_down: 4 scenes > 2
        press(LEFT);
        press(UP);
        assertArrayEquals(new int[] {0, 0}, mapping.gridOffset());
        press(RIGHT);
        assertArrayEquals(new int[] {1, 0}, mapping.gridOffset());
        assertEquals(1, model.gridTrack);
        for (int i = 0; i < 20; i++) press(RIGHT);
        assertArrayEquals(new int[] {8, 0}, mapping.gridOffset(), "10 tracks, 2 columns: stops at 8");
        for (int i = 0; i < 5; i++) press(DOWN);
        assertArrayEquals(new int[] {8, 2}, mapping.gridOffset(), "4 scenes, 2 rows: stops at 2");
        assertEquals(8, model.gridTrack);
        assertEquals(2, model.gridScene);
        l = leds();
        assertEquals(Mapping.ON, (int) l.get(8));
        assertEquals(Mapping.OFF, (int) l.get(9));
        assertEquals(Mapping.ON, (int) l.get(10));
        assertEquals(Mapping.OFF, (int) l.get(11));
        press(LEFT);
        assertArrayEquals(new int[] {7, 2}, mapping.gridOffset());
        model.tracks.subList(4, 10).clear();                      // tracks deleted: 4 left
        press(LEFT);
        assertArrayEquals(new int[] {2, 2}, mapping.gridOffset(), "clamped to the tracks there are");
    }

    @Test
    void clipLedsFollowTheGridScroll() throws ProtocolException {
        model.slot(1, 0).state = Model.SLOT_CONTENT;
        model.slot(1, 0).colour = 0xFF0000;
        mapping.refresh();
        Map<Integer, int[]> c = rgb();
        assertArrayEquals(new int[] {127, 0, 0, 0}, c.get(4 + 1));    // clip_2_1 (LED 5) is track 1
        press(RIGHT);
        c = rgb();
        assertArrayEquals(new int[] {127, 0, 0, 0}, c.get(4), "clip_1_1 now shows track 1");
        assertArrayEquals(new int[] {0, 0, 0, 0}, c.get(5));
        assertEquals(2, c.size(), "the other two stay empty: nothing sent");
    }

    @Test
    void clipColoursAndBlink() throws ProtocolException {
        Map<Integer, int[]> c = rgb();
        assertEquals(4, c.size(), "every clip LED at connect");
        assertArrayEquals(new int[] {0, 0, 0, 0}, c.get(4), "empty: off");
        FakeModel.FSlot s = model.slot(0, 0);
        s.state = Model.SLOT_CONTENT;
        s.colour = 0xFF8000;
        mapping.refresh();
        assertArrayEquals(new int[] {127, 64, 0, 0}, rgb().get(4), "stopped: the clip's colour");
        s.state |= Model.SLOT_PLAY_QUEUED;
        mapping.refresh();
        assertArrayEquals(new int[] {127, 64, 0, 1}, rgb().get(4), "launch queued: blinking");
        s.state = Model.SLOT_CONTENT | Model.SLOT_PLAYING;
        mapping.refresh();
        assertArrayEquals(new int[] {0, 127, 0, 0}, rgb().get(4), "playing: green");
        s.state |= Model.SLOT_STOP_QUEUED;
        mapping.refresh();
        assertArrayEquals(new int[] {0, 127, 0, 1}, rgb().get(4), "stop queued: green, blinking");
        s.state = Model.SLOT_CONTENT | Model.SLOT_RECORDING;
        mapping.refresh();
        assertArrayEquals(new int[] {127, 0, 0, 0}, rgb().get(4), "recording: red");
        s.state = Model.SLOT_CONTENT;
        s.colour = -1;
        mapping.refresh();
        assertArrayEquals(new int[] {0, 127, 127, 0}, rgb().get(4), "no colour: teal");
        s.colour = 0;
        mapping.refresh();
        assertTrue(rgb().isEmpty(), "black is shown as teal too: no change");
    }

    @Test
    void gridLedsAreChangeOnly() throws ProtocolException {
        rgb();
        mapping.refresh();
        mapping.flush();
        mapping.tick();
        assertTrue(fake.take(-1).isEmpty(), "nothing changed: nothing sent");
        model.slot(1, 1).state = Model.SLOT_CONTENT;
        mapping.flush();
        mapping.flush();
        Map<Integer, int[]> c = rgb();
        assertEquals(1, c.size());
        assertTrue(c.containsKey(4 + CLIP_2_2 - 4));
        press(DOWN);                                  // clip_2_1 now shows that slot, clip_2_2 an empty one
        assertEquals(2, rgb().size());
    }

    @Test
    void bootedResendsTheGridAndKeepsItsPosition() throws ProtocolException {
        press(RIGHT);
        fake.take(-1);
        fake.boot();
        assertTrue(device.isReady());
        assertEquals(4, rgb().size(), "every clip LED again");
        assertArrayEquals(new int[] {1, 0}, mapping.gridOffset());
        assertEquals(1, model.gridTrack);
    }

    @Test
    void clipRolesOnButtonsAndLeds() throws ProtocolException {
        List<Component> comps = new ArrayList<>();
        comps.add(FakeDevice.button("clip_3_1", 0));
        comps.add(FakeDevice.led(""));
        comps.add(FakeDevice.led("clip_1_1"));                    // an LED of its own: shows the colour
        comps.add(FakeDevice.button("clip_9_1", Protocol.NONE_INDEX));
        comps.add(FakeDevice.button("clip_1", Protocol.NONE_INDEX));
        setUp(comps);
        assertEquals(List.of("button 1 clip_9_1", "button 2 clip_1"), mapping.unknownRoles());
        assertEquals(3, model.gridWidth);
        assertEquals(1, model.gridHeight);
        Map<Integer, int[]> c = rgb();
        assertEquals(2, c.size());
        model.slot(2, 0).state = Model.SLOT_CONTENT;
        model.slot(2, 0).colour = 0x0000FF;
        press(0);
        assertEquals(List.of("launch clip 2 0"), model.calls);
        assertArrayEquals(new int[] {0, 0, 127, 0}, rgb().get(0));
    }

    @Test
    void clipCells() {
        assertEquals(0, Mapping.clipCell("clip_1_1"));
        assertEquals(2 * Mapping.GRID + 1, Mapping.clipCell("clip_3_2"));
        assertEquals(Mapping.GRID * Mapping.GRID - 1, Mapping.clipCell("clip_8_8"));
        for (String bad : new String[] {"clip_9_1", "clip_0_1", "clip_1_9", "clip_1", "clip_a_1", "clip__1", "clip_1_1_1"}) {
            assertEquals(-1, Mapping.clipCell(bad), bad);
        }
    }
}
