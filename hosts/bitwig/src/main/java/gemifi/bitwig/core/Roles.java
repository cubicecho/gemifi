// SPDX-License-Identifier: MIT
package gemifi.bitwig.core;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** The {@code mcu:} name → role mapping from {@code hosts/README.md} ("Roles"). */
public final class Roles {
    private Roles() {}

    private static final Map<String, String> MCU_ROLE_MAP = new HashMap<>();
    private static final String[][] MCU_PREFIX_MAP = {{"fader_", "volume_"}, {"vpot_", "pan_"}, {"rec_", "arm_"}};

    static {
        MCU_ROLE_MAP.put("master", "master_volume");
        MCU_ROLE_MAP.put("bank_left", "bank_prev");
        MCU_ROLE_MAP.put("bank_right", "bank_next");
        MCU_ROLE_MAP.put("channel_left", "strip_prev");
        MCU_ROLE_MAP.put("channel_right", "strip_next");
        MCU_ROLE_MAP.put("cycle", "loop");
        MCU_ROLE_MAP.put("click", "metronome");
    }

    /** Applies the mapping. Unknown names pass through (the mapping layer ignores roles it doesn't know). */
    public static String normalise(String role) {
        String r = role == null ? "" : role.trim().toLowerCase(Locale.ROOT);
        String mapped = MCU_ROLE_MAP.get(r);
        if (mapped != null) return mapped;
        for (String[] pm : MCU_PREFIX_MAP) {
            if (r.startsWith(pm[0]) && isDigits(r.substring(pm[0].length()))) {
                return pm[1] + r.substring(pm[0].length());
            }
        }
        return r;
    }

    /**
     * True for another host's command ({@code <host>.<command>}, daw-scripts.md §6), which a script
     * ignores without logging; {@code ownHost} is this script's namespace, e.g. {@code "bitwig"}.
     */
    public static boolean isOtherHostCommand(String role, String ownHost) {
        int dot = role.indexOf('.');
        return dot > 0 && !role.substring(0, dot).equals(ownHost);
    }

    static boolean isDigits(String s) {
        if (s.isEmpty()) return false;
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
        }
        return true;
    }
}
