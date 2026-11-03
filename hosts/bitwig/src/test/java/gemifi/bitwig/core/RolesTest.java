// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

/** The mcu: → role table from hosts/README.md. */
class RolesTest {
    @Test
    void mcuNames() {
        assertEquals("volume_3", Roles.normalise("fader_3"));
        assertEquals("master_volume", Roles.normalise("master"));
        assertEquals("pan_8", Roles.normalise("vpot_8"));
        assertEquals("arm_1", Roles.normalise("rec_1"));
        assertEquals("solo_2", Roles.normalise("solo_2"));
        assertEquals("mute_2", Roles.normalise("mute_2"));
        assertEquals("select_4", Roles.normalise("select_4"));
        assertEquals("bank_prev", Roles.normalise("bank_left"));
        assertEquals("bank_next", Roles.normalise("bank_right"));
        assertEquals("track_prev", Roles.normalise("channel_left"));
        assertEquals("track_next", Roles.normalise("channel_right"));
        assertEquals("loop", Roles.normalise("cycle"));
        assertEquals("metronome", Roles.normalise("click"));
        assertEquals("play", Roles.normalise("play"));
        assertEquals("fast_forward", Roles.normalise("fast_forward"));
    }

    @Test
    void passThrough() {
        assertEquals("record", Roles.normalise("record"));
        assertEquals("fader_", Roles.normalise("fader_"));
        assertEquals("rec_x", Roles.normalise("rec_x"));
        assertEquals("x_custom", Roles.normalise("x_custom"));
        assertEquals("", Roles.normalise(null));
        assertEquals("play", Roles.normalise("  PLAY "));
    }
}
