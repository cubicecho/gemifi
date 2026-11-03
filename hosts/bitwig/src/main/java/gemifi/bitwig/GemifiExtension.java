// SPDX-License-Identifier: MIT
package gemifi.bitwig;

import com.bitwig.extension.controller.ControllerExtension;
import com.bitwig.extension.controller.api.ControllerHost;
import com.bitwig.extension.controller.api.MidiIn;
import com.bitwig.extension.controller.api.MidiOut;
import gemifi.bitwig.core.Device;
import gemifi.bitwig.core.Mapping;
import gemifi.bitwig.core.Protocol;
import gemifi.bitwig.core.Protocol.Event;

/**
 * The thin Bitwig side: ports, SysEx in/out, the 100 ms tick and {@code flush()}. Everything with
 * protocol or role logic is in {@code gemifi.bitwig.core} (no Bitwig imports, unit-tested).
 */
public final class GemifiExtension extends ControllerExtension {
    /** Device and mapping clock. */
    static final long TICK_MS = 100;

    private ControllerHost host;
    private MidiOut midiOut;
    private Device device;
    private Mapping mapping;
    private boolean running;

    GemifiExtension(GemifiExtensionDefinition definition, ControllerHost host) {
        super(definition, host);
    }

    @Override
    public void init() {
        host = getHost();
        MidiIn midiIn = host.getMidiInPort(0);
        midiOut = host.getMidiOutPort(0);

        // The device's keys, pads and CCs keep playing instruments: pass every channel message on to
        // the tracks. The extension maps none of them; it only reads SysEx.
        midiIn.createNoteInput("gemifi", "80????", "90????", "A0????", "B0????", "C0????", "D0????", "E0????");
        midiIn.setSysexCallback(this::onSysex);

        BitwigModel model = new BitwigModel(host);
        device = new Device(this::sendSysex);
        mapping = new Mapping(model, device::notify, this::log);
        device.setListener(new Device.Listener() {
            @Override
            public void onReady(Device d) {
                mapping.bind(d.allComponents());
                mapping.refresh();
                Protocol.Hello h = d.hello();
                String what = h.node != null ? h.node : "gemifi device";
                host.showPopupNotification("gemifi: connected to " + what + " (fw " + h.versionText() + ")");
            }

            @Override
            public void onLost(Device d) {
                mapping.release();
            }

            @Override
            public void onEvent(Event ev) {
                mapping.handleEvent(ev);
            }

            @Override
            public void onLog(String text) {
                log(text);
            }
        });

        running = true;
        device.start();
        host.scheduleTask(this::tick, TICK_MS);
        log("started");
    }

    private void tick() {
        if (!running) return;
        try {
            device.tick();
            if (device.isReady()) mapping.tick();
        } catch (RuntimeException e) {
            host.errorln("gemifi: " + e);
        }
        host.scheduleTask(this::tick, TICK_MS);
    }

    private void onSysex(String hex) {
        if (!running) return;
        try {
            device.receive(Protocol.fromHex(hex));
        } catch (RuntimeException e) {
            host.errorln("gemifi: " + e);
        }
    }

    private void sendSysex(byte[] frame) {
        midiOut.sendSysex(Protocol.toHex(frame));
    }

    private void log(String text) {
        host.println("gemifi: " + text);
    }

    @Override
    public void flush() {
        // Bitwig calls this after observed values changed: send the differences as FB_*.
        if (running && device.isReady()) mapping.refresh();
    }

    @Override
    public void exit() {
        running = false;
        if (mapping != null) mapping.release();
        if (device != null) device.release();     // FB_RELEASE 0, debug.events back to what it was
        log("stopped");
    }
}
