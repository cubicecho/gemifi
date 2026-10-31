#!/usr/bin/env bash
# READ-ONLY resumable ISP dump (one OpenOCD session per 256 words; long Bus Pirate sessions lock up).
# Usage: isp-dump.sh <dumpdir> <name> <base> <size> <pass> [--probe buspirate|cmsis-dap] [--dry-run]
#   e.g. isp-dump.sh ~/kms32-backup ldrom 0x00100000 0x1000 1
# Appends "addr value" lines to <dumpdir>/isp/<name>-pass<pass>.txt; addresses already there are skipped,
# so rerun the same command to resume after a failure. Then:
#   isp-assemble.py <dumpdir> <name> <base> <size>            backup of unknown content (needs 2 passes)
#   isp-compare.py <dumpdir>/isp/<name>-pass1.txt <file> --base <base>   check against a known file (1 pass)
# dump.sh does all of this for a full backup; this is for one region by hand.
source "$(dirname "$0")/common.sh"
args=()
while [ $# -gt 0 ]; do
  common_opt "$@"; if [ "$COMMON_SHIFT" -gt 0 ]; then shift "$COMMON_SHIFT"; continue; fi
  case "$1" in
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) args+=("$1") ;;
  esac
  shift
done
[ ${#args[@]} -eq 5 ] || { sed -n '2,9p' "$0"; exit 2; }
preflight
isp_dump_range "${args[@]}"
