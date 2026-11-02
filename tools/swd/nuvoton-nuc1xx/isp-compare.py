#!/usr/bin/env python3
"""Compare ONE ISP dump pass with a known image (offline). For read-back verify after a write.

Usage: isp-compare.py <pass.txt> <image.bin> --base ADDR [--size N] [--out FILE] [--show N]

<pass.txt>  an isp-dump.sh pass file, e.g. <dir>/isp/ldrom-pass1.txt ("addr value" lines, hex).
<image.bin> the file that was written. It is padded with 0xFF to a multiple of 4 (erased flash).
--base      address of the image's first byte (LDROM 0x00100000, data flash 0x0001F000).
--size      bytes to check (default: the image size rounded up to 4). A larger size also checks
            that the rest of the region is erased (0xFF).
--out       write the bytes read to FILE once every word has been read.
--show      how many differing words to list (default 20).

One pass is enough here: the image is the reference. For a backup of unknown content use
isp-assemble.py, which needs two identical passes.

Exit 0 = every word read and equal; 1 = words differ (or one address was read with two values);
3 = no differences so far, but some words are not read yet (resume the dump, then run this again).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isp_dumpfile  # noqa: E402

EXIT_OK, EXIT_DIFF, EXIT_INCOMPLETE = 0, 1, 3


def expected_words(image, base, size):
    data = image + b"\xff" * (size - len(image))
    return {base + i: int.from_bytes(data[i:i + 4], "little") for i in range(0, size, 4)}


def compare(pass_path, image, base, size=None, show=20, out=None, log=print):
    """Returns an exit code (EXIT_*) and prints a report through log()."""
    if size is None:
        size = (len(image) + 3) // 4 * 4
    if size % 4 or size < len(image):
        raise ValueError(f"--size {size} must be a multiple of 4 and at least the image size ({len(image)})")
    want = expected_words(image, base, size)
    name = os.path.basename(pass_path)
    if os.path.isfile(pass_path):
        pf = isp_dumpfile.load(pass_path)
    else:
        pf = isp_dumpfile.PassFile({}, {}, 0)
        log(f"{name}: not found, so nothing has been read yet")
    got = pf.words
    missing = [a for a in want if a not in got]
    diff = [a for a in want if a in got and got[a] != want[a]]
    conflicts = [a for a in want if a in pf.conflicts]
    total = len(want)
    span = f"0x{base:08x}..0x{base + size - 1:08x}"
    if pf.bad_lines:
        log(f"note: {pf.bad_lines} cut-short line(s) in {name} ignored (those words are read again on resume)")

    if conflicts:
        log(f"VERIFY FAILED: {len(conflicts)} address(es) were read twice with different values "
            f"(unreliable SWD link): {isp_dumpfile.fmt_ranges(conflicts)}")
    if diff:
        log(f"VERIFY FAILED: {len(diff)} of {total} words differ ({span}, {total - len(missing)} read)")
        log("  address     offset   expected  read")
        for a in diff[:show]:
            log(f"  0x{a:08x}  +0x{a - base:04x}  {want[a]:08x}  {got[a]:08x}")
        if len(diff) > show:
            log(f"  ... and {len(diff) - show} more: {isp_dumpfile.fmt_ranges(diff[show:])}")
    if missing:
        log(f"{'INCOMPLETE' if not (diff or conflicts) else 'Also incomplete'}: "
            f"{len(missing)} of {total} words not read yet: {isp_dumpfile.fmt_ranges(missing)}")
    if diff or conflicts:
        return EXIT_DIFF
    if missing:
        log(f"No differences in the {total - len(missing)} words read so far. Resume the dump, then compare again.")
        return EXIT_INCOMPLETE
    if out:
        with open(out, "wb") as f:
            f.write(b"".join(got[a].to_bytes(4, "little") for a in sorted(want)))
    log(f"VERIFY OK: all {total} words ({size} bytes, {span}) match")
    return EXIT_OK


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("passfile")
    ap.add_argument("image")
    ap.add_argument("--base", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--size", type=lambda s: int(s, 0))
    ap.add_argument("--out")
    ap.add_argument("--show", type=int, default=20)
    a = ap.parse_args()
    with open(a.image, "rb") as f:
        image = f.read()
    try:
        rc = compare(a.passfile, image, a.base, a.size, a.show, a.out)
    except ValueError as e:
        ap.error(str(e))
    sys.exit(rc)


if __name__ == "__main__":
    main()
