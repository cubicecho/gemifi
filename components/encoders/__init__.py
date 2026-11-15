"""encoders: the rotary encoders (`encoders:`, `encoder_settings:`), a control. encoders.c counts their detents
from the scan and sends them (README.md here). Its YAML is still validated in gemifi_device.py until R10
moves it here.
"""


def in_device(dev):
    """Built when the device has encoders (GM_N_ENCODERS > 0, twins included)."""
    return bool(dev.encoders)
