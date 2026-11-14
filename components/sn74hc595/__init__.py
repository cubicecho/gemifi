"""sn74hc595: a chain of 74HC595 serial-in shift registers (outputs), a hub type on the hub base
(components/hub, whose ShiftRegister it is). The YAML and the C: README.md here.
"""
from components.hub import ShiftRegister

HUB_TYPES = (ShiftRegister("sn74hc595", "GM_HUB_HC595", 1, out=True, c_ops="gm_sn74hc595_ops"),)
