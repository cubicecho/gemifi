#!/usr/bin/env python3
"""Spike for research/core/osc-design.md: can the core build OSC float32 bits
for value = num / den (0 <= num <= den < 2^16) with integer ops only (the
core's no-floating-point rule; the ESP32-S2 has no FPU)?

The function below is written the way the C would be (uint32/uint64 only).
It is checked against IEEE round-to-nearest-even (struct.pack('>f')) for every
num/den with den in a set of real gemifi ranges. Run: python3 float_bits.py
"""
import struct
from fractions import Fraction


def f32_ratio(num: int, den: int) -> int:
    """IEEE-754 single bits of num/den, round to nearest even. 0 <= num <= den."""
    if num == 0:
        return 0
    # Normalise: find e with 2^e <= num/den < 2^(e+1), e in [-16, 0].
    e = 0
    n = num
    while n < den:          # at most 16 steps
        n <<= 1
        e -= 1
    if n >= den << 1:       # only when num == den and loop didn't run
        e += 1
    # 24 significant bits plus guard: q = floor(num * 2^(23 - e) / den)
    sh = 23 - e             # 23..39 -> needs 64-bit intermediates
    t = num << sh
    q, r = divmod(t, den)   # q in [2^23, 2^24)
    # round to nearest, ties to even
    if 2 * r > den or (2 * r == den and (q & 1)):
        q += 1
        if q == 1 << 24:
            q >>= 1
            e += 1
    return ((e + 127) << 23) | (q & 0x7FFFFF)


def ref(num, den):
    return struct.unpack(">I", struct.pack(">f", float(Fraction(num, den))))[0]


def main():
    bad = 0
    total = 0
    for den in (1, 2, 100, 127, 1023, 4095, 16383, 65535):
        for num in range(den + 1):
            total += 1
            if f32_ratio(num, den) != ref(num, den):
                bad += 1
                if bad < 5:
                    print("mismatch", num, den, hex(f32_ratio(num, den)), hex(ref(num, den)))
    print(f"{total} values checked, {bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
