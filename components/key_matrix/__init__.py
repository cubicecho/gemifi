"""key_matrix: the key matrix (`key_matrix:`), a control: a keyboard's keys, with velocity from two contacts.
key_matrix.c scans it and plays the keys (README.md here). Its YAML is still validated in gemifi_device.py
(validate_matrix) until R10 moves it here.
"""


def in_device(dev):
    """Built when the device has a key matrix (GM_HAS_MATRIX 1)."""
    return dev.matrix is not None
