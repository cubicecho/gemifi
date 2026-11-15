"""buttons: the buttons (`buttons:`), a control. buttons.c samples and debounces them and runs their actions
(README.md here). Its YAML is still validated in gemifi_device.py until R10 moves it here.
"""


def in_device(dev):
    """Built when the device has buttons (GM_N_BUTTONS > 0, twins included)."""
    return bool(dev.buttons)
