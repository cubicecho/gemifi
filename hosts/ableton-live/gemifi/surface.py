# SPDX-License-Identifier: MIT
"""The Remote Script object Live talks to.

A plain class (no Ableton framework base class). It uses only the long-standing `c_instance`
calls: song(), send_midi(), show_message(), log_message(), request_rebuild_midi_map().
Callbacks Live makes on the script are the methods below; ones this script has no use for are
harmless no-ops.

It owns one Device (device.py) on the ports assigned to the "gemifi" control surface and one
Mapping (mapping.py) over the song. Only SysEx is handled here; the device's normal notes and
CCs are not mapped, so Live passes them on to tracks / MIDI mapping as usual.
"""

import traceback

from .device import Device
from .mapping import Mapping

SCRIPT_NAME = "gemifi"


def _guarded(fn):
    """Exceptions inside Live callbacks go to Log.txt instead of disabling the script."""
    def wrapper(self, *args, **kwargs):
        try:
            return fn(self, *args, **kwargs)
        except Exception:
            self._log("error in %s:\n%s" % (fn.__name__, traceback.format_exc()))
            return None
    wrapper.__name__ = fn.__name__
    wrapper.__doc__ = fn.__doc__
    return wrapper


class GemifiSurface(object):
    def __init__(self, c_instance, song=None):
        self._c = c_instance
        if song is None:
            from .live_song import LiveSong
            song = LiveSong(c_instance.song())
        self.song = song
        self.device = Device(self._send)
        self.mapping = Mapping(song, self.device.notify, log=self._log)
        self.device.on_ready = self._on_ready
        self.device.on_lost = self._on_lost
        self.device.on_event = self._on_event
        self.device.on_log = self._log
        self._started = False       # HELLO goes out on the first update_display, not in __init__
        self._in_sysex = False

    # ---- output ----

    def _send(self, data):
        self._c.send_midi(tuple(data))

    def _log(self, text):
        try:
            self._c.log_message("%s: %s" % (SCRIPT_NAME, text))
        except Exception:
            pass

    def _message(self, text):
        try:
            self._c.show_message("%s: %s" % (SCRIPT_NAME, text))
        except Exception:
            pass

    # ---- device callbacks ----

    def _on_ready(self, device):
        self.mapping.bind(device.all_components(), device.layers_on)
        for kind, index, role in self.mapping.unknown_roles:
            self._log("ignoring unknown role %r (kind %d, index %d)" % (role, kind, index))
        self.mapping.refresh()
        h = device.hello
        self._message("connected to %s (fw %s)" % (h.node or "device", h.version_text))

    def _on_lost(self, device):
        self.mapping.release()
        self._message("device lost, looking for it again")

    def _on_event(self, event):
        self.mapping.handle_event(event)

    # ---- Live -> script ----

    @_guarded
    def receive_midi(self, midi_bytes):
        """Live calls this with the MIDI the script asked for (build_midi_map) and SysEx."""
        if not midi_bytes:
            return
        if midi_bytes[0] == 0xF0 or self._in_sysex:
            self._in_sysex = midi_bytes[-1] != 0xF7
            self.device.receive(midi_bytes)
        # Anything else is ignored: notes and CCs are not mapped by this script.

    @_guarded
    def build_midi_map(self, midi_map_handle):
        _route_sysex_to_script(self._c, midi_map_handle)

    @_guarded
    def update_display(self):
        """Live calls this about every 100 ms: the clock for retries and feedback."""
        if not self._started:
            self._started = True
            self.device.start()
            return
        self.device.tick()
        if self.device.ready:
            self.mapping.tick()

    @_guarded
    def refresh_state(self):
        """Live asks the script to resend its state (e.g. after the ports changed)."""
        if self.device.ready:
            self.mapping.forget()
            self.mapping.refresh()
        elif self._started:
            self.device.start()

    @_guarded
    def port_settings_changed(self):
        # A different port may be behind the script now: start over.
        self.mapping.release()
        self._started = True
        self.device.start()

    @_guarded
    def disconnect(self):
        """Live is closing, the set is being replaced, or the surface was unselected."""
        self.device.release()           # FB_RELEASE 0, debug.events back to what it was
        self.mapping.release()

    # Optional callbacks: harmless answers.

    def connect_script_instances(self, instances):
        pass

    def can_lock_to_devices(self):
        return False

    def suggest_input_port(self):
        return ""

    def suggest_output_port(self):
        return ""


def _route_sysex_to_script(c_instance, midi_map_handle):
    """Make sure SysEx on the script's input port reaches receive_midi.

    UNCERTAIN (needs real Live): to the author's knowledge Live hands every SysEx message that
    arrives on a control surface's input port to that script's receive_midi() without a MIDI-map
    entry; notes and CCs, by contrast, need Live.MidiMap.forward_midi_note / forward_midi_cc.
    So this deliberately maps nothing (which also leaves the device's notes and CCs alone for
    tracks). If testing in Live shows SysEx doesn't arrive, this is the one place to add the
    forwarding call.
    """
    return None
