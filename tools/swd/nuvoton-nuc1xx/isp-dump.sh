#!/usr/bin/env bash
# READ-ONLY resumable ISP dump (one OpenOCD session per 256 words; long Bus Pirate sessions lock up).
# Usage: isp-dump.sh <dumpdir> <name> <base> <size> <pass>
#   e.g. isp-dump.sh ~/kms32-backup ldrom 0x00100000 0x1000 1
# Appends "addr value" lines to <dumpdir>/isp/<name>-pass<pass>.txt; addresses already there are skipped,
# so rerun the same command to resume after a failure. Assemble with:
#   isp-assemble.py <dumpdir> <name> <base> <size>
# dump.sh does all of this for a full backup; this is for one region by hand.
source "$(dirname "$0")/common.sh"
[ $# -eq 5 ] || { sed -n '2,9p' "$0"; exit 2; }
preflight
isp_dump_range "$@"
