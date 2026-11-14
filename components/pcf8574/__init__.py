"""pcf8574: a PCF8574 8-pin quasi-bidirectional I/O expander on the I2C bus, a hub type on the hub base
(components/hub, whose I2CExpander it is). The YAML and the C: README.md here.
"""
from components.hub import I2CExpander

HUB_TYPES = (I2CExpander("pcf8574", "GM_HUB_PCF8574", 5, 8, list(range(0x20, 0x28)) + list(range(0x38, 0x40)),
                         "0x20-0x27, PCF8574A 0x38-0x3F", c_ops="gm_pcf8574_ops"),)
