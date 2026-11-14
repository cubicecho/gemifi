"""mcp23017: an MCP23017 16-pin I/O expander on the I2C bus, a hub type on the hub base (components/hub,
whose I2CExpander it is). The YAML and the C: README.md here.
"""
from components.hub import I2CExpander

HUB_TYPES = (I2CExpander("mcp23017", "GM_HUB_MCP23017", 4, 16, range(0x20, 0x28), "0x20-0x27", pin_key="mcp23xxx",
                         c_ops="gm_mcp23017_ops"),)
