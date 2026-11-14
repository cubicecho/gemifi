"""sn74hc165: a chain of 74HC165 parallel-in shift registers (inputs), a hub type on the hub base
(components/hub, whose ShiftRegister it is). The YAML and the C: README.md here.
"""
from components.hub import ShiftRegister

HUB_TYPES = (ShiftRegister("sn74hc165", "GM_HUB_HC165", 2, out=False, c_ops="gm_sn74hc165_ops"),)
