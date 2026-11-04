// SPDX-License-Identifier: MIT
package gemifi.bitwig;

import com.bitwig.extension.controller.api.Application;
import com.bitwig.extension.controller.api.BooleanValue;
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

    BitwigModel(ControllerHost host) {
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
            deviceParams[i] = new BwParam(rc, () -> deviceKey() + "/" + index, null, false, true);
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
    }

    private String deviceKey() {
        return "dev@" + selected.position() + "/" + cursorDevice.position().get() + "/"
                + cursorDevice.name().get() + "/" + remotes.selectedPageIndex().get();
    }

    // ---- wrappers ----

    private interface KeyFn {
        String key();
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
            volume = new BwParam(t.volume(), () -> "vol@" + position(), t.exists(), false, false);
            pan = new BwParam(t.pan(), () -> "pan@" + position(), t.exists(), true, false);
            Send a = t.sendBank().getItemAt(0);
            Send b = t.sendBank().getItemAt(1);
            sendA = new BwParam(a, () -> "sendA@" + position(), t.exists(), false, false);
            sendB = new BwParam(b, () -> "sendB@" + position(), t.exists(), false, false);
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
}
