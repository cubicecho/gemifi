// SPDX-License-Identifier: MIT
package gemifi.bitwig;

import com.bitwig.extension.controller.api.Application;
import com.bitwig.extension.controller.api.BooleanValue;
import com.bitwig.extension.controller.api.ClipLauncherSlot;
import com.bitwig.extension.controller.api.ClipLauncherSlotBank;
import com.bitwig.extension.controller.api.ColorValue;
import com.bitwig.extension.controller.api.ControllerHost;
import com.bitwig.extension.controller.api.CursorRemoteControlsPage;
import com.bitwig.extension.controller.api.CursorTrack;
import com.bitwig.extension.controller.api.MasterTrack;
import com.bitwig.extension.controller.api.Parameter;
import com.bitwig.extension.controller.api.PinnableCursorDevice;
import com.bitwig.extension.controller.api.RemoteControl;
import com.bitwig.extension.controller.api.SceneBank;
import com.bitwig.extension.controller.api.Send;
import com.bitwig.extension.controller.api.SettableBooleanValue;
import com.bitwig.extension.controller.api.Track;
import com.bitwig.extension.controller.api.TrackBank;
import com.bitwig.extension.controller.api.Transport;
import gemifi.bitwig.core.Mapping;
import gemifi.bitwig.core.Model;
import java.util.Arrays;
import java.util.Objects;

/**
 * {@link Model} on top of the Bitwig controller API. Everything the mapping reads is created in the
 * constructor (Bitwig only allows that during {@code init()}) and marked interested, so plain
 * {@code get()} calls return current values whenever the mapping polls.
 */
final class BitwigModel implements Model {
    /** Bitwig 5.2+ (API 20) has RangedValue.getOrigin() and discreteValueCount(). */
    private static final int API_ORIGIN = 20;

    private final Transport transport;
    private final Application application;
    private final CursorTrack cursorTrack;
    private final PinnableCursorDevice cursorDevice;
    private final CursorRemoteControlsPage remotes;
    private final TrackBank trackBank;
    private final SceneBank sceneBank;
    private final MasterTrack master;
    private final boolean hasOrigin;

    private final BwStrip selected;
    private final BwStrip[] strips = new BwStrip[Mapping.STRIPS];
    private final BooleanValue[] stripIsSelected = new BooleanValue[Mapping.STRIPS];
    private final BwParam[] deviceParams = new BwParam[Mapping.STRIPS];
    private final BwParam tempo;
    private final BwParam masterVolume;
    private final BwParam crossfader;

    private final ControllerHost host;
    private final TrackBank gridBank;
    private final SceneBank gridScenes;
    private final Track[] gridTracks = new Track[Mapping.GRID];
    private final ClipLauncherSlotBank[] gridSlotBanks = new ClipLauncherSlotBank[Mapping.GRID];
    private final ClipLauncherSlot[][] slots = new ClipLauncherSlot[Mapping.GRID][Mapping.GRID];
    private int gridWidth = -1;
    private int gridHeight = -1;

    BitwigModel(ControllerHost host) {
        this.host = host;
        hasOrigin = host.getHostApiVersion() >= API_ORIGIN;
        transport = host.createTransport();
        application = host.createApplication();
        cursorTrack = host.createCursorTrack("gemifi-cursor-track", "gemifi", 2, 0, true);
        cursorDevice = cursorTrack.createCursorDevice();
        remotes = cursorDevice.createCursorRemoteControlsPage(Mapping.STRIPS);
        trackBank = host.createMainTrackBank(Mapping.STRIPS, 0, 0);
        sceneBank = host.createSceneBank(1);
        master = host.createMasterTrack(0);

        for (SettableBooleanValue v : new SettableBooleanValue[] {transport.isPlaying(),
            transport.isArrangerRecordEnabled(), transport.isArrangerLoopEnabled(),
            transport.isMetronomeEnabled(), transport.isArrangerOverdubEnabled(),
            transport.isClipLauncherOverdubEnabled(), transport.isArrangerAutomationWriteEnabled()}) {
            v.markInterested();
        }

        cursorTrack.hasPrevious().markInterested();
        cursorTrack.hasNext().markInterested();
        selected = new BwStrip(cursorTrack, "sel");

        trackBank.canScrollBackwards().markInterested();
        trackBank.canScrollForwards().markInterested();
        for (int i = 0; i < Mapping.STRIPS; i++) {
            Track t = trackBank.getItemAt(i);
            strips[i] = new BwStrip(t, "strip");
            stripIsSelected[i] = t.createEqualsValue(cursorTrack);
            stripIsSelected[i].markInterested();
        }

        cursorDevice.exists().markInterested();
        cursorDevice.isEnabled().markInterested();
        cursorDevice.hasPrevious().markInterested();
        cursorDevice.hasNext().markInterested();
        cursorDevice.position().markInterested();
        cursorDevice.name().markInterested();
        remotes.hasPrevious().markInterested();
        remotes.hasNext().markInterested();
        remotes.selectedPageIndex().markInterested();
        for (int i = 0; i < Mapping.STRIPS; i++) {
            RemoteControl rc = remotes.getParameter(i);
            int index = i;
            deviceParams[i] = new BwParam(rc, () -> deviceParamKey(index), null, false, true);
        }

        Parameter tempoParam = transport.tempo();
        tempo = new BwParam(tempoParam, () -> "tempo", null, false, false) {
            @Override
            public void nudge(int delta) {
                tempoParam.value().incRaw(delta);            // 1 BPM per detent
            }
        };
        tempoParam.value().markInterested();
        crossfader = new BwParam(transport.crossfade(), () -> "crossfader", null, true, false);
        master.exists().markInterested();
        masterVolume = new BwParam(master.volume(), () -> "master/volume", master.exists(), false, false);

        sceneBank.canScrollBackwards().markInterested();
        sceneBank.canScrollForwards().markInterested();
        sceneBank.getScene(0).exists().markInterested();
        sceneBank.setIndication(true);

        // The clip grid: its own 8 x 8 track bank (the API is fixed after init(), so the maximum),
        // scrolled apart from the mixer bank. setGridSize() narrows it to what the roles use.
        gridBank = host.createMainTrackBank(Mapping.GRID, 0, Mapping.GRID);
        gridBank.itemCount().markInterested();
        gridScenes = gridBank.sceneBank();
        gridScenes.itemCount().markInterested();
        for (int t = 0; t < Mapping.GRID; t++) {
            gridTracks[t] = gridBank.getItemAt(t);
            gridSlotBanks[t] = gridTracks[t].clipLauncherSlotBank();
            for (int s = 0; s < Mapping.GRID; s++) {
                ClipLauncherSlot slot = gridSlotBanks[t].getItemAt(s);
                slot.exists().markInterested();
                slot.hasContent().markInterested();
                slot.isPlaying().markInterested();
                slot.isRecording().markInterested();
                slot.isPlaybackQueued().markInterested();
                slot.isStopQueued().markInterested();
                slot.isRecordingQueued().markInterested();
                slot.color().markInterested();
                slots[t][s] = slot;
            }
        }
        for (int s = 0; s < Mapping.GRID; s++) gridScenes.getScene(s).exists().markInterested();
    }

    // The keys are read on every refresh, for every control: build a string only when its parts change.
    private int keyTrack;
    private int keyDevice;
    private int keyPage;
    private String keyName;
    private String deviceKey;
    private final String[] deviceParamKeys = new String[Mapping.STRIPS];

    private String deviceKey() {
        int track = selected.position();
        int dev = cursorDevice.position().get();
        int page = remotes.selectedPageIndex().get();
        String name = cursorDevice.name().get();
        if (deviceKey == null || track != keyTrack || dev != keyDevice || page != keyPage
                || !Objects.equals(name, keyName)) {
            keyTrack = track;
            keyDevice = dev;
            keyPage = page;
            keyName = name;
            deviceKey = "dev@" + track + "/" + dev + "/" + name + "/" + page;
            Arrays.fill(deviceParamKeys, null);
        }
        return deviceKey;
    }

    private String deviceParamKey(int i) {
        String base = deviceKey();
        String k = deviceParamKeys[i];
        if (k == null) k = deviceParamKeys[i] = base + "/" + i;
        return k;
    }

    // ---- wrappers ----

    private interface KeyFn {
        String key();
    }

    /** prefix + the track's position, rebuilt only when the position changes. */
    private static KeyFn positionKey(String prefix, Track t) {
        return new KeyFn() {
            private int pos;
            private String key;

            @Override
            public String key() {
                int p = t.position().get();
                if (key == null || p != pos) {
                    pos = p;
                    key = prefix + p;
                }
                return key;
            }
        };
    }

    /** A Bitwig Parameter as a Model.Param. */
    private class BwParam implements Model.Param {
        final Parameter p;
        final KeyFn key;
        final BooleanValue owner;      // the param counts as missing while this is false (null = always there)
        final boolean bipolar;
        final boolean useOrigin;

        BwParam(Parameter p, KeyFn key, BooleanValue owner, boolean bipolar, boolean useOrigin) {
            this.p = p;
            this.key = key;
            this.owner = owner;
            this.bipolar = bipolar;
            this.useOrigin = useOrigin && hasOrigin;
            p.exists().markInterested();
            p.value().markInterested();
            if (this.useOrigin) {
                p.value().getOrigin().markInterested();
                p.value().discreteValueCount().markInterested();
            }
        }

        boolean present() {
            return p.exists().get() && (owner == null || owner.get());
        }

        @Override
        public Object key() {
            return key.key();
        }

        @Override
        public double get() {
            return p.value().get();
        }

        @Override
        public void set(double norm) {
            p.value().set(norm);
        }

        @Override
        public void nudge(int delta) {
            int steps = Mapping.ENCODER_STEPS;
            if (useOrigin) {
                int n = p.value().discreteValueCount().get();
                if (n > 1) steps = n - 1;                  // a quantized parameter: one value per detent
            }
            p.value().inc(delta, steps);
        }

        @Override
        public boolean bipolar() {
            if (useOrigin) {
                double origin = p.value().getOrigin().get();
                return origin > 0.25 && origin < 0.75;
            }
            return bipolar;
        }
    }

    /** A Bitwig Track (the cursor track or a bank slot) as a Model.Strip. */
    private final class BwStrip implements Model.Strip {
        final Track t;
        final String prefix;
        final BwParam volume;
        final BwParam pan;
        final BwParam sendA;
        final BwParam sendB;

        BwStrip(Track t, String prefix) {
            this.t = t;
            this.prefix = prefix;
            t.exists().markInterested();
            t.position().markInterested();
            t.mute().markInterested();
            t.solo().markInterested();
            t.arm().markInterested();
            volume = new BwParam(t.volume(), positionKey("vol@", t), t.exists(), false, false);
            pan = new BwParam(t.pan(), positionKey("pan@", t), t.exists(), true, false);
            Send a = t.sendBank().getItemAt(0);
            Send b = t.sendBank().getItemAt(1);
            sendA = new BwParam(a, positionKey("sendA@", t), t.exists(), false, false);
            sendB = new BwParam(b, positionKey("sendB@", t), t.exists(), false, false);
        }

        int position() {
            return t.position().get();
        }

        boolean exists() {
            return t.exists().get();
        }

        private SettableBooleanValue value(TrackFlag f) {
            switch (f) {
                case MUTE: return t.mute();
                case SOLO: return t.solo();
                default: return t.arm();
            }
        }

        @Override
        public boolean flag(TrackFlag f) {
            return value(f).get();
        }

        @Override
        public void setFlag(TrackFlag f, boolean on) {
            value(f).set(on);
        }

        @Override
        public Param param(TrackParam p) {
            BwParam bp;
            switch (p) {
                case VOLUME: bp = volume; break;
                case PAN: bp = pan; break;
                case SEND_A: bp = sendA; break;
                default: bp = sendB; break;
            }
            return bp.present() ? bp : null;
        }
    }

    // ---- Model ----

    @Override
    public boolean supports(String role) {
        // Bitwig's API has no cue / pre-listen volume.
        return !"cue_volume".equals(role);
    }

    private SettableBooleanValue transportValue(TransportFlag f) {
        switch (f) {
            case PLAYING: return transport.isPlaying();
            case RECORD: return transport.isArrangerRecordEnabled();
            case LOOP: return transport.isArrangerLoopEnabled();
            case METRONOME: return transport.isMetronomeEnabled();
            case OVERDUB: return transport.isArrangerOverdubEnabled();
            case LAUNCHER_OVERDUB: return transport.isClipLauncherOverdubEnabled();
            default: return transport.isArrangerAutomationWriteEnabled();
        }
    }

    @Override
    public boolean flag(TransportFlag f) {
        return transportValue(f).get();
    }

    @Override
    public void setFlag(TransportFlag f, boolean on) {
        if (f == TransportFlag.PLAYING) {
            if (on) transport.play();
            else transport.stop();
            return;
        }
        transportValue(f).set(on);
    }

    @Override
    public void stop() {
        transport.stop();
    }

    @Override
    public void tapTempo() {
        transport.tapTempo();
    }

    @Override
    public void undo() {
        application.undo();
    }

    @Override
    public void redo() {
        application.redo();
    }

    @Override
    public void jump(double beats) {
        transport.incPosition(beats, false);
    }

    @Override
    public void stopAllClips() {
        trackBank.sceneBank().stop();
    }

    @Override
    public Param tempo() {
        return tempo;
    }

    @Override
    public Strip selectedTrack() {
        return selected.exists() ? selected : null;
    }

    @Override
    public boolean canStepTrack(int d) {
        return d < 0 ? cursorTrack.hasPrevious().get() : cursorTrack.hasNext().get();
    }

    @Override
    public void stepTrack(int d) {
        if (d < 0) cursorTrack.selectPrevious();
        else cursorTrack.selectNext();
    }

    @Override
    public Strip strip(int i) {
        return strips[i].exists() ? strips[i] : null;
    }

    @Override
    public boolean isStripSelected(int i) {
        return stripIsSelected[i].get();
    }

    @Override
    public void selectStrip(int i) {
        cursorTrack.selectChannel(trackBank.getItemAt(i));
    }

    @Override
    public boolean canStepBank(int d) {
        return d < 0 ? trackBank.canScrollBackwards().get() : trackBank.canScrollForwards().get();
    }

    @Override
    public void stepBank(int d) {
        trackBank.scrollBy(d * Mapping.STRIPS);
    }

    @Override
    public void stepStrip(int d) {
        trackBank.scrollBy(d);
    }

    @Override
    public Param master(MasterParam p) {
        switch (p) {
            case VOLUME: return masterVolume.present() ? masterVolume : null;
            case CROSSFADER: return crossfader;
            default: return null;
        }
    }

    @Override
    public boolean hasDevice() {
        return cursorDevice.exists().get();
    }

    @Override
    public boolean deviceEnabled() {
        return cursorDevice.isEnabled().get();
    }

    @Override
    public void setDeviceEnabled(boolean on) {
        cursorDevice.isEnabled().set(on);
    }

    @Override
    public boolean canStepDevice(int d) {
        return d < 0 ? cursorDevice.hasPrevious().get() : cursorDevice.hasNext().get();
    }

    @Override
    public void stepDevice(int d) {
        if (d < 0) cursorDevice.selectPrevious();
        else cursorDevice.selectNext();
    }

    @Override
    public Param deviceParam(int i) {
        if (!hasDevice()) return null;
        BwParam p = deviceParams[i];
        return p.present() ? p : null;
    }

    @Override
    public boolean canStepDevicePage(int d) {
        return hasDevice() && (d < 0 ? remotes.hasPrevious().get() : remotes.hasNext().get());
    }

    @Override
    public void stepDevicePage(int d) {
        if (d < 0) remotes.selectPreviousPage(false);
        else remotes.selectNextPage(false);
    }

    @Override
    public boolean canStepScene(int d) {
        return d < 0 ? sceneBank.canScrollBackwards().get() : sceneBank.canScrollForwards().get();
    }

    @Override
    public void stepScene(int d) {
        sceneBank.scrollBy(d);
    }

    @Override
    public void launchScene() {
        if (sceneBank.getScene(0).exists().get()) sceneBank.launchScene(0);
    }

    // ---- clip grid ----

    @Override
    public int gridTrackCount() {
        return gridBank.itemCount().get();
    }

    @Override
    public int gridSceneCount() {
        return gridScenes.itemCount().get();
    }

    @Override
    public void setGridSize(int tracks, int scenes) {
        if (tracks == gridWidth && scenes == gridHeight) return;
        gridWidth = tracks;
        gridHeight = scenes;
        try {
            // So Bitwig scrolls (and indicates) the window the device shows, not the whole 8 x 8.
            gridBank.setSizeOfBank(tracks);
            gridScenes.setSizeOfBank(scenes);
        } catch (RuntimeException e) {
            host.errorln("gemifi: grid size " + tracks + "x" + scenes + ": " + e);
        }
        for (int t = 0; t < Mapping.GRID; t++) gridSlotBanks[t].setIndication(t < tracks);
    }

    @Override
    public void setGridOffset(int track, int scene) {
        gridBank.scrollPosition().set(track);
        gridScenes.scrollPosition().set(scene);
    }

    @Override
    public int slotState(int t, int s) {
        ClipLauncherSlot slot = slots[t][s];
        if (!slot.exists().get()) return 0;
        int st = 0;
        if (slot.hasContent().get()) st |= SLOT_CONTENT;
        if (slot.isPlaying().get()) st |= SLOT_PLAYING;
        if (slot.isRecording().get()) st |= SLOT_RECORDING;
        if (slot.isPlaybackQueued().get()) st |= SLOT_PLAY_QUEUED;
        if (slot.isStopQueued().get()) st |= SLOT_STOP_QUEUED;
        if (slot.isRecordingQueued().get()) st |= SLOT_RECORD_QUEUED;
        return st;
    }

    private static int byte8(float v) {
        return Math.max(0, Math.min(255, Math.round(v * 255f)));
    }

    @Override
    public int slotColour(int t, int s) {
        ColorValue c = slots[t][s].color();
        return byte8(c.red()) << 16 | byte8(c.green()) << 8 | byte8(c.blue());
    }

    @Override
    public void launchSlot(int t, int s) {
        slots[t][s].launch();
    }

    @Override
    public void stopSlot(int t, int s) {
        gridTracks[t].stop();
    }

    @Override
    public void launchGridScene(int s) {
        if (gridScenes.getScene(s).exists().get()) gridScenes.getScene(s).launch();
    }
}
