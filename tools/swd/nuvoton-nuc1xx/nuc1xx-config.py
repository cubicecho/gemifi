#!/usr/bin/env python3
"""Decode NUC100/120 CONFIG0/CONFIG1 (user configuration words). Offline; touches no hardware.

Usage:
  nuc1xx-config.py decode <CONFIG0> [<CONFIG1>]
  nuc1xx-config.py decode --file config.txt
  nuc1xx-config.py get CONFIG0 --file config.txt        (prints the value, for scripts)
  nuc1xx-config.py diff --old C0 C1 --new C0 C1         (side-by-side; exit 3 if the new value locks the chip)

Field layout per the NUC100/NUC120 Technical Reference Manual (V2.02), "User Configuration":
  CONFIG0 bit 28    CKF     XT1 clock filter      1 = enabled
          bit 26:24 CFOSC   reset clock source    000 = external 4-24 MHz crystal, 111 = internal 22.1184 MHz RC
          bit 23    CBODEN  brown-out detector    0 = enabled
          bit 22:21 CBOV    brown-out voltage     00 = 2.2 V, 01 = 2.7 V, 10 = 3.8 V, 11 = 4.5 V
          bit 20    CBORST  brown-out reset       0 = enabled
          bit 7     CBS     boot select           0 = LDROM, 1 = APROM
          bit 1     LOCK    security lock         0 = LOCKED (flash reads as 0xFFFFFFFF), 1 = unlocked
          bit 0     DFEN    data flash enable     0 = enabled (only used on 128 KB parts; the
                                                  NUC120LC1BN has a fixed 4 KB data flash at 0x1F000)
  CONFIG1 bit 19:0  DFBADR  data flash base (128 KB parts only)
Other bits are reserved and should read 1.
Stock Keystation Mini 32 MK3: CONFIG0 0xFF4FFF7F, CONFIG1 0xFFFFFFFF.
"""
import argparse
import sys

KNOWN0 = (1 << 28) | (7 << 24) | (1 << 23) | (3 << 21) | (1 << 20) | (1 << 7) | (1 << 1) | 1
CFOSC = {0: "external 4-24 MHz crystal (XT1)", 7: "internal 22.1184 MHz RC"}
CBOV = {0: "2.2 V", 1: "2.7 V", 2: "3.8 V", 3: "4.5 V"}


def fields(c0):
    return {
        "CBS": (c0 >> 7) & 1,
        "LOCK": (c0 >> 1) & 1,
        "DFEN": c0 & 1,
        "CFOSC": (c0 >> 24) & 7,
        "CKF": (c0 >> 28) & 1,
        "CBODEN": (c0 >> 23) & 1,
        "CBOV": (c0 >> 21) & 3,
        "CBORST": (c0 >> 20) & 1,
    }


def decode_lines(c0, c1=None):
    f = fields(c0)
    out = [f"CONFIG0 = 0x{c0:08X}"]
    out.append(f"  CBS    (bit 7)     = {f['CBS']}  boot from {'APROM' if f['CBS'] else 'LDROM'}")
    out.append(f"  LOCK   (bit 1)     = {f['LOCK']}  {'unlocked' if f['LOCK'] else 'LOCKED: flash reads back as 0xFFFFFFFF; only a chip erase unlocks'}")
    out.append(f"  DFEN   (bit 0)     = {f['DFEN']}  (128 KB parts only; NUC120LC1BN data flash is fixed at 0x1F000)")
    out.append(f"  CFOSC  (bits 26:24)= {f['CFOSC']:03b}  {CFOSC.get(f['CFOSC'], 'RESERVED value')}")
    out.append(f"  CKF    (bit 28)    = {f['CKF']}  XT1 clock filter {'enabled' if f['CKF'] else 'disabled'}")
    out.append(f"  CBODEN (bit 23)    = {f['CBODEN']}  brown-out detector {'disabled' if f['CBODEN'] else 'enabled'}")
    out.append(f"  CBOV   (bits 22:21)= {f['CBOV']:02b}  brown-out level {CBOV[f['CBOV']]}")
    out.append(f"  CBORST (bit 20)    = {f['CBORST']}  brown-out reset {'disabled' if f['CBORST'] else 'enabled'}")
    reserved = (~c0) & ~KNOWN0 & 0xFFFFFFFF
    if reserved:
        out.append(f"  WARNING: reserved bits are 0: mask 0x{reserved:08X}")
    if c1 is not None:
        out.append(f"CONFIG1 = 0x{c1:08X}")
        out.append(f"  DFBADR (bits 19:0) = 0x{c1 & 0xFFFFF:05X}  (128 KB parts only)")
    return out


def parse_config_txt(path):
    vals = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 2 and p[0].upper().startswith("CONFIG"):
                vals[p[0].upper()] = int(p[1], 0)
    return vals


def u32(s):
    v = int(s, 0)
    if not 0 <= v <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("not a 32-bit value")
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode")
    d.add_argument("values", nargs="*", type=u32)
    d.add_argument("--file")
    g = sub.add_parser("get")
    g.add_argument("name")
    g.add_argument("--file", required=True)
    x = sub.add_parser("diff")
    x.add_argument("--old", nargs=2, type=u32, required=True)
    x.add_argument("--new", nargs=2, type=u32, required=True)
    a = ap.parse_args()

    if a.cmd == "decode":
        vals = a.values
        if a.file:
            c = parse_config_txt(a.file)
            vals = [c["CONFIG0"], c.get("CONFIG1")]
        if not vals:
            ap.error("give CONFIG0 [CONFIG1] or --file")
        print("\n".join(decode_lines(vals[0], vals[1] if len(vals) > 1 else None)))
    elif a.cmd == "get":
        c = parse_config_txt(a.file)
        if a.name.upper() not in c:
            sys.exit(f"{a.name} not in {a.file}")
        print(f"0x{c[a.name.upper()]:08X}")
    else:
        print("OLD (on the chip now):")
        print("\n".join("  " + l for l in decode_lines(*a.old)))
        print("NEW (will be written):")
        print("\n".join("  " + l for l in decode_lines(*a.new)))
        fo, fn = fields(a.old[0]), fields(a.new[0])
        changed = [k for k in fo if fo[k] != fn[k]]
        print("Changed fields: " + (", ".join(changed) if changed else "none") +
              ("" if a.old[1] == a.new[1] else ", CONFIG1"))
        if fn["LOCK"] == 0:
            print("REFUSED: the new CONFIG0 sets LOCK = 0. gemifi never locks the chip.")
            sys.exit(3)
        if fn["CFOSC"] not in CFOSC:
            print("REFUSED: the new CONFIG0 has a reserved CFOSC value.")
            sys.exit(3)


if __name__ == "__main__":
    main()
