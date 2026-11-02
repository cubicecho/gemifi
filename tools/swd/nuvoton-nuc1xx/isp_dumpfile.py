"""Shared reader for ISP dump pass files (offline). Used by isp-assemble.py and isp-compare.py.

A pass file holds one "aaaaaaaa vvvvvvvv" line per word (lower-case hex, written by isp-chunk.tcl).
Only lines of exactly that shape count. The resume logic in common.sh uses the same rule, so a line
cut short by a killed session (or glued to the next session's first line) is ignored and re-read.
"""
import re

LINE = re.compile(r"^([0-9a-fA-F]{8}) ([0-9a-fA-F]{8})$")


class PassFile:
    def __init__(self, words, conflicts, bad_lines):
        self.words = words          # addr -> value (first good read)
        self.conflicts = conflicts  # addr -> sorted list of the different values read
        self.bad_lines = bad_lines  # lines that didn't parse (partial writes)


def load(path):
    words, conflicts, bad = {}, {}, 0
    with open(path) as f:
        for line in f:
            m = LINE.match(line.strip())
            if not m:
                if line.strip():
                    bad += 1
                continue
            a, v = int(m.group(1), 16), int(m.group(2), 16)
            if a in words and words[a] != v:
                conflicts.setdefault(a, {words[a]}).add(v)
            words.setdefault(a, v)
    return PassFile(words, {a: sorted(v) for a, v in conflicts.items()}, bad)


def ranges(addrs):
    """Collapse word addresses into [(first, last)] runs of consecutive words."""
    out = []
    for a in sorted(addrs):
        if out and a == out[-1][1] + 4:
            out[-1][1] = a
        else:
            out.append([a, a])
    return [tuple(r) for r in out]


def fmt_ranges(addrs, limit=6):
    rs = ranges(addrs)
    s = ", ".join(f"0x{a:08x}" if a == b else f"0x{a:08x}..0x{b:08x}" for a, b in rs[:limit])
    if len(rs) > limit:
        s += f", ... ({len(rs) - limit} more ranges)"
    return s
