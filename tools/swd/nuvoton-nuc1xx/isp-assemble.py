#!/usr/bin/env python3
"""Build a binary (or config.txt) from two ISP dump passes, only if both are complete and identical.

Usage: isp-assemble.py <dumpdir> <name> <base> <size> [--config] [--out FILE] [--passes 1|2]
Reads <dumpdir>/isp/<name>-pass1.txt and -pass2.txt ("addr value" lines, hex).
Writes <dumpdir>/<name>.bin, or with --config <dumpdir>/config.txt as "CONFIGn 0x........" lines.

Two passes are the rule for backups: the content is unknown, so the only check is that two reads agree.
--passes 1 trusts a single pass. To check a read-back against the file you wrote, use
isp-compare.py instead: one pass is enough there, and it lists the differing addresses.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isp_dumpfile  # noqa: E402


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

    paths = [os.path.join(a.dumpdir, "isp", f"{a.name}-pass{n}.txt") for n in range(1, a.passes + 1)]
    absent = [p for p in paths if not os.path.isfile(p)]
    if absent:
        print(f"NOT OK: {a.name}: {', '.join(absent)} not found.")
        if a.passes == 2 and absent == paths[1:]:
            print("  A backup needs two passes. To check a read-back against a known file, use isp-compare.py.")
        sys.exit(1)
    p = [isp_dumpfile.load(x) for x in paths]
    if a.passes == 1:
        p.append(p[0])
    want = range(a.base, a.base + a.size, 4)
    w0, w1 = p[0].words, p[1].words
    missing = [x for x in want if x not in w0 or x not in w1]
    diff = [x for x in want if x in w0 and x in w1 and w0[x] != w1[x]]
    conflicts = sorted({x for x in want for q in p if x in q.conflicts})
    if missing or diff or conflicts:
        print(f"NOT OK: {a.name}: {len(missing)} missing, {len(diff)} differ between passes, "
              f"{len(conflicts)} read twice with different values")
        for label, xs in (("missing", missing), ("differ", diff), ("conflicting", conflicts)):
            if xs:
                print(f"  {label}: {isp_dumpfile.fmt_ranges(xs)}")
        sys.exit(1)
    if a.config:
        out = a.out or os.path.join(a.dumpdir, "config.txt")
        with open(out, "w") as f:
            for i, x in enumerate(want):
                f.write(f"CONFIG{i} 0x{w0[x]:08x}\n")
    else:
        out = a.out or os.path.join(a.dumpdir, f"{a.name}.bin")
        with open(out, "wb") as f:
            f.write(b"".join(w0[x].to_bytes(4, "little") for x in want))
    print(f"OK: {out} ({a.size} bytes, {'passes identical' if a.passes == 2 else 'single pass'})")


if __name__ == "__main__":
    main()
