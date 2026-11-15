"""modes: the modes (`modes:`), a control: key and button modes, hold modes, layers and cycles. modes.c runs
them (README.md here). Its YAML is still validated in gemifi_device.py (validate_modes) until R10 moves it
here.
"""


def in_device(dev):
    """Built when the device has modes (GM_N_MODES > 0)."""
    return bool(dev.modes)
