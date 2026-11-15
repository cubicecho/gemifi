"""pads: the velocity pads (`pads:`, `pad_settings:`), a control. pads.c reads them and plays their notes
(README.md here). Its YAML is still validated in gemifi_device.py (validate_pads) until R10 moves it here.
"""


def in_device(dev):
    """Built when the device has pads (GM_N_PADS > 0)."""
    return bool(dev.pads)
