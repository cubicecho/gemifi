// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;

/** A small DAW for the mapping tests: tracks, a bank of 8, a device with remote pages, scenes. */
final class FakeModel implements Model {
    static final class FParam implements Model.Param {
        double value;
        final boolean bipolar;
        final String name;
        /** When true, set() is only applied by apply() (like Bitwig's asynchronous value updates). */
        boolean deferred;
        Double pending;

        FParam(double value, boolean bipolar, String name) {
            this.value = value;
            this.bipolar = bipolar;
            this.name = name;
        }

        @Override
        public Object key() {
            return name;
        }

        @Override
        public double get() {
            return value;
        }

        @Override
        public void set(double norm) {
            double v = Math.max(0.0, Math.min(1.0, norm));
            if (deferred) pending = v;
            else value = v;
        }

        void apply() {
            if (pending != null) value = pending;
            pending = null;
        }

        @Override
        public boolean bipolar() {
            return bipolar;
        }
    }

    static final class FTrack implements Model.Strip {
        final String name;
        final Map<TrackFlag, Boolean> flags = new EnumMap<>(TrackFlag.class);
        final Map<TrackParam, FParam> params = new EnumMap<>(TrackParam.class);
        boolean canArm = true;

        FTrack(String name) {
            this.name = name;
            for (TrackFlag f : TrackFlag.values()) flags.put(f, false);
            params.put(TrackParam.VOLUME, new FParam(0.85, false, name + ".vol"));
            params.put(TrackParam.PAN, new FParam(0.5, true, name + ".pan"));
            params.put(TrackParam.SEND_A, new FParam(0.0, false, name + ".sendA"));
        }

        @Override
        public boolean flag(TrackFlag f) {
            return flags.get(f);
        }

        @Override
        public void setFlag(TrackFlag f, boolean on) {
            flags.put(f, on);
        }

        @Override
        public boolean canArm() {
            return canArm;
        }

        @Override
        public Param param(TrackParam p) {
            return params.get(p);
        }
    }

    final Map<TransportFlag, Boolean> flags = new EnumMap<>(TransportFlag.class);
    final List<FTrack> tracks = new ArrayList<>();
    int selected;
    int bankOffset;
    final List<String> calls = new ArrayList<>();
    double position;
    final FParam tempo = new FParam(0.2, false, "tempo");
    final FParam masterVolume = new FParam(0.7, false, "master.vol");
    final FParam crossfader = new FParam(0.5, true, "xfade");

    // device: pages of params; null devices list = no device
    final List<List<FParam>> devices = new ArrayList<>();
    final List<Boolean> deviceOn = new ArrayList<>();
    int device;
    int page;
    int scene;
    int sceneCount = 4;

    FakeModel(int nTracks) {
        for (TransportFlag f : TransportFlag.values()) flags.put(f, false);
        for (int i = 0; i < nTracks; i++) tracks.add(new FTrack("t" + (i + 1)));
        List<FParam> dev = new ArrayList<>();
        for (int i = 0; i < 12; i++) dev.add(new FParam(0.1 * (i % 10), false, "d0p" + i));
        devices.add(dev);
        deviceOn.add(true);
        List<FParam> dev2 = new ArrayList<>();
        for (int i = 0; i < 3; i++) dev2.add(new FParam(0.3, false, "d1p" + i));
        devices.add(dev2);
        deviceOn.add(false);
    }

    @Override
    public boolean supports(String role) {
        return !"cue_volume".equals(role);
    }

    @Override
    public boolean flag(TransportFlag f) {
        return flags.get(f);
    }

    @Override
    public void setFlag(TransportFlag f, boolean on) {
        flags.put(f, on);
    }

    @Override
    public void stop() {
        flags.put(TransportFlag.PLAYING, false);
        calls.add("stop");
    }

    @Override
    public void tapTempo() {
        calls.add("tap");
    }

    @Override
    public void undo() {
        calls.add("undo");
    }

    @Override
    public void redo() {
        calls.add("redo");
    }

    @Override
    public void jump(double beats) {
        position += beats;
    }

    @Override
    public void stopAllClips() {
        calls.add("stop_all");
    }

    @Override
    public Param tempo() {
        return tempo;
    }

    @Override
    public Strip selectedTrack() {
        return tracks.isEmpty() ? null : tracks.get(selected);
    }

    @Override
    public boolean canStepTrack(int d) {
        return selected + d >= 0 && selected + d < tracks.size();
    }

    @Override
    public void stepTrack(int d) {
        if (canStepTrack(d)) selected += d;
    }

    @Override
    public Strip strip(int i) {
        int k = bankOffset + i;
        return k < tracks.size() ? tracks.get(k) : null;
    }

    @Override
    public boolean isStripSelected(int i) {
        return bankOffset + i == selected;
    }

    @Override
    public void selectStrip(int i) {
        selected = bankOffset + i;
    }

    @Override
    public boolean canStepBank(int d) {
        return d < 0 ? bankOffset > 0 : bankOffset + Mapping.STRIPS < tracks.size();
    }

    @Override
    public void stepBank(int d) {
        if (canStepBank(d)) bankOffset = Math.max(0, bankOffset + d * Mapping.STRIPS);
    }

    @Override
    public void stepStrip(int d) {
        if (canStepBank(d)) bankOffset = Math.max(0, bankOffset + d);
    }

    @Override
    public Param master(MasterParam p) {
        switch (p) {
            case VOLUME: return masterVolume;
            case CROSSFADER: return crossfader;
            default: return null;
        }
    }

    @Override
    public boolean hasDevice() {
        return device < devices.size();
    }

    @Override
    public boolean deviceEnabled() {
        return hasDevice() && deviceOn.get(device);
    }

    @Override
    public void setDeviceEnabled(boolean on) {
        deviceOn.set(device, on);
    }

    @Override
    public boolean canStepDevice(int d) {
        return device + d >= 0 && device + d < devices.size();
    }

    @Override
    public void stepDevice(int d) {
        if (canStepDevice(d)) {
            device += d;
            page = 0;
        }
    }

    @Override
    public Param deviceParam(int i) {
        if (!hasDevice()) return null;
        int k = page * Mapping.STRIPS + i;
        List<FParam> ps = devices.get(device);
        return k < ps.size() ? ps.get(k) : null;
    }

    private int pages() {
        return (devices.get(device).size() + Mapping.STRIPS - 1) / Mapping.STRIPS;
    }

    @Override
    public boolean canStepDevicePage(int d) {
        return hasDevice() && page + d >= 0 && page + d < pages();
    }

    @Override
    public void stepDevicePage(int d) {
        if (canStepDevicePage(d)) page += d;
    }

    @Override
    public boolean canStepScene(int d) {
        return scene + d >= 0 && scene + d < sceneCount;
    }

    @Override
    public void stepScene(int d) {
        if (canStepScene(d)) scene += d;
    }

    @Override
    public void launchScene() {
        calls.add("launch " + scene);
    }

    // ---- clip grid: slots by absolute track and scene, a window at (gridTrack, gridScene) ----

    static final class FSlot {
        int state;                  // Model.SLOT_* bits
        int colour = -1;
    }

    final Map<String, FSlot> slots = new java.util.HashMap<>();
    int gridTrack;
    int gridScene;
    int gridWidth;
    int gridHeight;

    /** The slot at absolute track t, scene s (created empty). */
    FSlot slot(int t, int s) {
        return slots.computeIfAbsent(t + "/" + s, k -> new FSlot());
    }

    @Override
    public int gridTrackCount() {
        return tracks.size();
    }

    @Override
    public int gridSceneCount() {
        return sceneCount;
    }

    @Override
    public void setGridSize(int tracks, int scenes) {
        gridWidth = tracks;
        gridHeight = scenes;
    }

    @Override
    public void setGridOffset(int track, int scene) {
        gridTrack = track;
        gridScene = scene;
    }

    @Override
    public int slotState(int t, int s) {
        int at = gridTrack + t;
        int as = gridScene + s;
        if (at >= tracks.size() || as >= sceneCount) return 0;
        FSlot f = slots.get(at + "/" + as);
        return f == null ? 0 : f.state;
    }

    @Override
    public int slotColour(int t, int s) {
        FSlot f = slots.get((gridTrack + t) + "/" + (gridScene + s));
        return f == null ? -1 : f.colour;
    }

    @Override
    public void launchSlot(int t, int s) {
        calls.add("launch clip " + (gridTrack + t) + " " + (gridScene + s));
    }

    @Override
    public void stopSlot(int t, int s) {
        calls.add("stop track " + (gridTrack + t));
    }

    @Override
    public void launchGridScene(int s) {
        calls.add("launch grid scene " + (gridScene + s));
    }
}
