"""tca9555: a TCA9555 16-pin I/O expander on the I2C bus, as an external component (core/README.md
"External components"): the hub type the core tests and tools/builder/tests/test_external.py load.
"""
from components.hub import I2CExpander

GEMIFI_API = 1

HUB_TYPES = (I2CExpander("tca9555", "", 0, 16, list(range(0x20, 0x28)), "0x20-0x27", c_ops="gm_tca9555_ops"),)
