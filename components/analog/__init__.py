"""analog: the analog inputs (`analog:`: pots, faders, wheels, pressure strips), a control. analog.c reads them
and sends what they're set to (README.md here). Its YAML is still validated in gemifi_device.py until R10
moves it here.
"""


def in_device(dev):
    """Built when the device has analog inputs (GM_N_ANALOGS > 0)."""
    return bool(dev.analogs)
