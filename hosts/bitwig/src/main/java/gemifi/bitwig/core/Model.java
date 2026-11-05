// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

/**
 * The DAW as the mapping sees it. No Bitwig imports: {@code gemifi.bitwig.BitwigModel} implements
 * it on top of the Bitwig controller API and the tests use a fake one. The defaults make a missing
 * feature harmless.
 *
 * <p>Unlike the Live script, banking (8 strips, 8 remote controls per page, the scene window) is
 * done by the host's own banks and cursors, so the model exposes "step" and "can step" for them
 * instead of whole lists.
 */
public interface Model {
    enum TransportFlag { PLAYING, RECORD, LOOP, METRONOME, OVERDUB, LAUNCHER_OVERDUB, AUTOMATION_WRITE }

    enum TrackFlag { MUTE, SOLO, ARM }

    enum TrackParam { VOLUME, PAN, SEND_A, SEND_B }

    enum MasterParam { VOLUME, CUE, CROSSFADER }

    /** A continuous target. */
    interface Param {
        /**
         * Identifies the underlying parameter, so the mapping notices when a control's target changes
         * (a bank switch, another device) even if the value happens to be the same. Compared with equals().
         */
        Object key();

        /** Normalised 0..1. */
        double get();

        /** Normalised 0..1. */
        void set(double norm);

        /** An encoder turn by {@code delta} detents (signed). */
        default void nudge(int delta) {
            set(Math.max(0.0, Math.min(1.0, get() + delta / (double) Mapping.ENCODER_STEPS)));
        }

        /** Centre = neutral (pan, crossfader): the ring shows boost/cut with the centre LED. */
        default boolean bipolar() {
            return false;
        }
    }

    /** A track: the selected one or a mixer strip. */
    interface Strip {
        boolean flag(TrackFlag f);

        void setFlag(TrackFlag f, boolean on);

        default boolean canArm() {
            return true;
        }

        /** Null if the track has no such parameter (e.g. no second send). */
        Param param(TrackParam p);
    }

    /** False for a role this host can't do at all; the mapping logs it and leaves the control alone. */
    default boolean supports(String role) {
        return true;
    }

    // ---- transport ----

    default boolean flag(TransportFlag f) {
        return false;
    }

    default void setFlag(TransportFlag f, boolean on) {}

    default void stop() {}

    default void tapTempo() {}

    default void undo() {}

    default void redo() {}

    /** Move the play position by {@code beats} (negative = back). */
    default void jump(double beats) {}

    default void stopAllClips() {}

    default Param tempo() {
        return null;
    }

    // ---- selected track ----

    default Strip selectedTrack() {
        return null;
    }

    default boolean canStepTrack(int d) {
        return false;
    }

    default void stepTrack(int d) {}

    // ---- mixer strips (bank of 8) ----

    /** Strip {@code i} (0..7) of the current bank, or null if there is no track there. */
    default Strip strip(int i) {
        return null;
    }

    default boolean isStripSelected(int i) {
        return false;
    }

    default void selectStrip(int i) {}

    default boolean canStepBank(int d) {
        return false;
    }

    /** Moves the 8 strips by 8 tracks. */
    default void stepBank(int d) {}

    /** Moves the 8 strips by 1 track ({@code strip_prev} / {@code strip_next}). */
    default void stepStrip(int d) {}

    // ---- master ----

    default Param master(MasterParam p) {
        return null;
    }

    // ---- selected device ----

    default boolean hasDevice() {
        return false;
    }

    default boolean deviceEnabled() {
        return false;
    }

    default void setDeviceEnabled(boolean on) {}

    default boolean canStepDevice(int d) {
        return false;
    }

    default void stepDevice(int d) {}

    /** Parameter {@code i} (0..7) of the current remote-controls page, or null. */
    default Param deviceParam(int i) {
        return null;
    }

    default boolean canStepDevicePage(int d) {
        return false;
    }

    default void stepDevicePage(int d) {}

    // ---- session ----

    default boolean canStepScene(int d) {
        return false;
    }

    default void stepScene(int d) {}

    /** Launches the current (selected) scene. */
    default void launchScene() {}

    // ---- clip grid (daw-scripts.md §5.9) ----
    // A window of up to 8 tracks x 8 scenes with its own scroll position, apart from the mixer
    // bank. t and s count from the window's first track and scene (0..7).

    /** slotState() bits. */
    int SLOT_CONTENT = 1, SLOT_PLAYING = 2, SLOT_RECORDING = 4, SLOT_PLAY_QUEUED = 8,
            SLOT_STOP_QUEUED = 16, SLOT_RECORD_QUEUED = 32;

    /** Tracks and scenes in the project the grid can show, for clamping the scroll. */
    default int gridTrackCount() {
        return 0;
    }

    default int gridSceneCount() {
        return 0;
    }

    /** The window's size, from the roles the device has (1..8 each). Called on bind. */
    default void setGridSize(int tracks, int scenes) {}

    /** Moves the window so its first track and scene are these (already clamped). */
    default void setGridOffset(int track, int scene) {}

    /** SLOT_* bits for slot (t, s) of the window; 0 = no slot there or empty. */
    default int slotState(int t, int s) {
        return 0;
    }

    /** The clip's colour as 0xRRGGBB (8 bits each), or -1 if it has none. */
    default int slotColour(int t, int s) {
        return -1;
    }

    default void launchSlot(int t, int s) {}

    /** Stops what plays in the window's track t (only one clip per track plays). */
    default void stopSlot(int t, int s) {}

    /** Launches the scene in the window's row s, if there is one. */
    default void launchGridScene(int s) {}
}
