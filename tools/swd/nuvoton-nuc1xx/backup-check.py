#!/usr/bin/env python3
"""Verify a backup directory (offline). Exit 0 only if SHA256SUMS verifies and required files exist.

Usage: backup-check.py <dir> [--require NAME[:SIZE]]... [--print-aprom]

SHA256SUMS entries are resolved relative to <dir>; if that file doesn't exist, the bare file name
is tried (the original Keystation backup lists "backup/aprom-read1.bin" next to "ldrom.bin").
--require aprom accepts aprom.bin or aprom-read1.bin. --print-aprom prints the APROM image path.
"""
import argparse
import hashlib
import os
import sys


def resolve(d, name):
    p = os.path.join(d, name)
    if os.path.isfile(p):
        return p
    p = os.path.join(d, os.path.basename(name))
    return p if os.path.isfile(p) else None


def find_aprom(d):
    for n in ("aprom.bin", "aprom-read1.bin"):
        p = os.path.join(d, n)
        if os.path.isfile(p):
            return p
    return None


def verify(d, requires):
    errs, verified = [], set()
    sums = os.path.join(d, "SHA256SUMS")
    if not os.path.isfile(sums):
        return [f"{sums} not found"], verified
    with open(sums) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(None, 1)
            if len(parts) != 2:
                errs.append(f"bad SHA256SUMS line: {line!r}")
                continue
            want, name = parts[0].lower(), parts[1].lstrip("*")
            p = resolve(d, name)
            if p is None:
                errs.append(f"{name}: listed in SHA256SUMS but missing")
                continue
            with open(p, "rb") as g:
                got = hashlib.sha256(g.read()).hexdigest()
            if got != want:
                errs.append(f"{name}: SHA-256 MISMATCH")
            else:
                verified.add(os.path.realpath(p))
                print(f"  sha256 OK  {os.path.basename(p)}", file=sys.stderr)
    for req in requires:
        name, _, size = req.partition(":")
        p = find_aprom(d) if name == "aprom" else resolve(d, name)
        if p is None:
            errs.append(f"required file {name} not found in {d}")
            continue
        if os.path.realpath(p) not in verified:
            errs.append(f"{os.path.basename(p)} is not covered by SHA256SUMS")
        if size and os.path.getsize(p) != int(size, 0):
            errs.append(f"{os.path.basename(p)} is {os.path.getsize(p)} bytes, expected {int(size, 0)}")
    return errs, verified


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--require", action="append", default=[])
    ap.add_argument("--print-aprom", action="store_true")
    a = ap.parse_args()
    errs, _ = verify(a.dir, a.require)
    if errs:
        for e in errs:
            print(f"BACKUP NOT OK: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"  backup OK: {a.dir}", file=sys.stderr)
    if a.print_aprom:
        print(find_aprom(a.dir))


if __name__ == "__main__":
    main()
