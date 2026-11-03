// SPDX-License-Identifier: MIT
package gemifi.bitwig;

import com.bitwig.extension.api.PlatformType;
import com.bitwig.extension.controller.AutoDetectionMidiPortNamesList;
import com.bitwig.extension.controller.ControllerExtension;
import com.bitwig.extension.controller.ControllerExtensionDefinition;
import com.bitwig.extension.controller.api.ControllerHost;
import java.util.UUID;

/** Settings → Controllers: vendor "gemifi", model "gemifi", one MIDI in / out pair (the device's main port). */
public final class GemifiExtensionDefinition extends ControllerExtensionDefinition {
    private static final UUID ID = UUID.fromString("861dcdfa-5463-43cc-8cd0-14b006e70512");

    /** Oldest controller API this needs (Bitwig Studio 5.0). Newer-API features are used only when present. */
    static final int REQUIRED_API = 18;

    @Override
    public String getName() {
        return "gemifi";
    }

    @Override
    public String getAuthor() {
        return "gemifi";
    }

    @Override
    public String getVersion() {
        return "0.1.0";
    }

    @Override
    public UUID getId() {
        return ID;
    }

    @Override
    public String getHardwareVendor() {
        return "gemifi";
    }

    @Override
    public String getHardwareModel() {
        return "gemifi";
    }

    @Override
    public int getRequiredAPIVersion() {
        return REQUIRED_API;
    }

    @Override
    public int getNumMidiInPorts() {
        return 1;
    }

    @Override
    public int getNumMidiOutPorts() {
        return 1;
    }

    @Override
    public void listAutoDetectionMidiPortNames(AutoDetectionMidiPortNamesList list, PlatformType platformType) {
        // No auto-detection: port names differ per device ("gemifi (<board>)") and per OS, and a device
        // with a DAW port has a second "… DAW" port that must not be picked. The user adds it by hand.
    }

    @Override
    public ControllerExtension createInstance(ControllerHost host) {
        return new GemifiExtension(this, host);
    }
}
