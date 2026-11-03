# SPDX-License-Identifier: MIT
"""The Remote Script object end to end: fake c_instance <-> fake device, fake song."""
import ast
import os

import gemifi
from gemifi import protocol as P
from gemifi import device as D
from gemifi.surface import GemifiSurface

from fakes import FakeCInstance, FakeDevice, FakeSong

PKG = os.path.dirname(gemifi.__file__)


def make():
    fake = FakeDevice()
    c = FakeCInstance(fake)
    song = FakeSong()
    s = GemifiSurface(c, song=song)
    fake.out = s.receive_midi
    return s, fake, c, song


def test_hello_waits_for_first_update_display():
    s, fake, c, song = make()
    assert c.sent == []
    s.build_midi_map(object())
    s.update_display()
    assert s.device.state == D.READY
    assert any("connected to test-pad" in m for m in c.messages)
    assert fake.params[P.PARAM_DEBUG_EVENTS] == 1
    assert fake.take(P.FB_LEDS)              # initial feedback went out


def test_end_to_end_play_and_disconnect():
    s, fake, c, song = make()
    s.update_display()
    fake.take()
    fake.event(P.KIND_BUTTON, 0, 1)          # play down
    fake.event(P.KIND_BUTTON, 0, 0)
    assert song.flags["playing"]
    assert fake.take() == [(P.FB_LED, P.fb_led(0, 127))]
    s.update_display()
    assert fake.take() == []
    s.disconnect()
    assert fake.take() == [(P.FB_RELEASE, bytes([0]))]
    assert fake.params[P.PARAM_DEBUG_EVENTS] == 0     # handed back as it was


def test_non_sysex_is_ignored():
    s, fake, c, song = make()
    s.update_display()
    fake.take()
    s.receive_midi((0x90, 60, 100))
    s.receive_midi((0xB0, 7, 100))
    assert fake.take() == [] and not song.flags["playing"]


def test_refresh_state_resends_everything():
    s, fake, c, song = make()
    s.update_display()
    fake.take()
    s.refresh_state()
    assert fake.take()                          # everything again
    s.update_display()
    assert fake.take() == []                    # then only changes go out


def test_callback_errors_are_logged_not_raised():
    s, fake, c, song = make()
    s.update_display()

    def boom(ev):
        raise RuntimeError("boom")
    s.device.on_event = boom
    fake.event(P.KIND_BUTTON, 0, 1)            # must not raise into Live
    assert any("boom" in line for line in c.logs)


def test_optional_callbacks_harmless():
    s, fake, c, song = make()
    s.connect_script_instances([])
    assert s.can_lock_to_devices() is False
    assert s.suggest_input_port() == "" and s.suggest_output_port() == ""
    s.port_settings_changed()
    assert s.device.state == D.READY


def test_device_without_feedback_gets_no_notifications():
    fake = FakeDevice(caps=P.CAP_CONFIG)
    c = FakeCInstance(fake)
    s = GemifiSurface(c, song=FakeSong())
    fake.out = s.receive_midi
    s.update_display()
    for _ in range(10):
        s.update_display()
    s.disconnect()
    assert fake.notifications == []
    assert [f.cmd for f in fake.requests] == [P.HELLO]


def test_create_instance_entry_point():
    assert callable(gemifi.create_instance)


def test_sources_parse_as_python_37():
    for name in sorted(os.listdir(PKG)):
        if name.endswith(".py"):
            with open(os.path.join(PKG, name)) as f:
                src = f.read()
            ast.parse(src, filename=name, feature_version=(3, 7))
            assert "SPDX-License-Identifier: MIT" in src.splitlines()[0], name


def test_only_adapter_files_mention_live():
    for name in ("protocol.py", "device.py", "mapping.py"):
        with open(os.path.join(PKG, name)) as f:
            src = f.read()
        assert "import Live" not in src and "_Framework" not in src, name
