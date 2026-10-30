#!/usr/bin/env python3
"""Sanity-check a raw binary before it is written over SWD (NUC120LC1BN). Offline; touches no hardware.

Usage: bincheck.py <file> --region aprom|ldrom|dataflash [--allow-odd-vectors]
Checks the size against the region and, for code regions, that the vector table looks like code
for this chip (initial SP inside 4 KB SRAM, reset vector odd and inside the image).
Exit 0 = OK, 1 = refused.
"""
import argparse
import os
import struct
import sys

REGIONS = {"aprom": 32 * 1024, "ldrom": 4096, "dataflash": 4096}
SRAM_LO, SRAM_HI = 0x20000000, 0x20001000


def check(path, region, allow_odd=False):
    errs = []
    size = os.path.getsize(path)
    limit = REGIONS[region]
    if size == 0:
        errs.append("file is empty")
    if size > limit:
        errs.append(f"{size} bytes is larger than {region} ({limit} bytes)")
    if region != "dataflash" and size >= 8:
        with open(path, "rb") as f:
            sp, rv = struct.unpack("<II", f.read(8))
        if not (SRAM_LO < sp <= SRAM_HI):
            errs.append(f"initial SP 0x{sp:08x} is not inside SRAM (0x{SRAM_LO:08x}..0x{SRAM_HI:08x})")
        if not (rv & 1):
            errs.append(f"reset vector 0x{rv:08x} is even (not Thumb)")
        if (rv & ~1) >= size:
            errs.append(f"reset vector 0x{rv:08x} points outside the image ({size} bytes)")
        if errs and allow_odd:
            print("WARNING (allowed): " + "; ".join(errs), file=sys.stderr)
            errs = []
    return size, errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--region", required=True, choices=sorted(REGIONS))
    ap.add_argument("--allow-odd-vectors", action="store_true")
    a = ap.parse_args()
    size, errs = check(a.file, a.region, a.allow_odd_vectors)
    if errs:
        for e in errs:
            print(f"REFUSED: {a.file}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"OK: {a.file} ({size} bytes) fits {a.region}")


if __name__ == "__main__":
    main()
