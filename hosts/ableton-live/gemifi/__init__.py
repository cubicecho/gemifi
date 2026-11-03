# SPDX-License-Identifier: MIT
"""gemifi: an Ableton Live Remote Script for gemifi devices (host feedback, protocol/sysex.md 4.6).

Written from scratch against the public Live Object Model (as documented for Max for Live)
and the long-standing c_instance interface. It contains no code from, and is not derived
from, Ableton's own MIDI Remote Scripts. MIT licensed (see the repository's LICENSE).

Live loads this package from "User Library/Remote Scripts/gemifi" and calls create_instance().
"""

from .surface import GemifiSurface


def create_instance(c_instance):
    return GemifiSurface(c_instance)
