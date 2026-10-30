#!/usr/bin/env python3
"""Build a binary (or config.txt) from two ISP dump passes, only if both are complete and identical.

Usage: isp-assemble.py <dumpdir> <name> <base> <size> [--config] [--out FILE] [--passes 1|2]
Reads <dumpdir>/isp/<name>-pass1.txt and -pass2.txt ("addr value" lines, hex).
--passes 1 uses pass1 only (read-back verify after a write, compared against the file written).
Writes <dumpdir>/<name>.bin, or with --config <dumpdir>/config.txt as "CONFIGn 0x........" lines.
"""
import argparse
import os
import sys


def load(path):
    d = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2:
                continue  # a line cut short by a killed session; the dump will re-read it
            d[int(parts[0], 16)] = int(parts[1], 16)
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumpdir")
    ap.add_argument("name")
    ap.add_argument("base", type=lambda s: int(s, 0))
    ap.add_argument("size", type=lambda s: int(s, 0))
    ap.add_argument("--config", action="store_true", help="write config.txt instead of a .bin")
    ap.add_argument("--out", help="output file (default <dumpdir>/<name>.bin or config.txt)")
    ap.add_argument("--passes", type=int, choices=(1, 2), default=2)
    a = ap.parse_args()

    p = [load(os.path.join(a.dumpdir, "isp", f"{a.name}-pass{n}.txt")) for n in range(1, a.passes + 1)]
    if a.passes == 1:
        p.append(p[0])
    want = range(a.base, a.base + a.size, 4)
    missing = [x for x in want if x not in p[0] or x not in p[1]]
    diff = [x for x in want if x in p[0] and x in p[1] and p[0][x] != p[1][x]]
    if missing or diff:
        print(f"NOT OK: {a.name}: {len(missing)} missing, {len(diff)} differ",
              [hex(x) for x in (missing + diff)[:10]])
        sys.exit(1)
    if a.config:
        out = a.out or os.path.join(a.dumpdir, "config.txt")
        with open(out, "w") as f:
            for i, x in enumerate(want):
                f.write(f"CONFIG{i} 0x{p[0][x]:08x}\n")
    else:
        out = a.out or os.path.join(a.dumpdir, f"{a.name}.bin")
        with open(out, "wb") as f:
            f.write(b"".join(p[0][x].to_bytes(4, "little") for x in want))
    print(f"OK: {out} ({a.size} bytes, {'passes identical' if a.passes == 2 else 'single pass'})")


if __name__ == "__main__":
    main()
