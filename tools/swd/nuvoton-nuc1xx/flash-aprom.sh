#!/usr/bin/env bash
# WRITES APROM. Programs a raw binary or a .gfw image at 0x0, verifies it, then resets the chip.
# LDROM, data flash and CONFIG are not touched.
#
# Usage: flash-aprom.sh <file.bin|file.gfw> [--yes] [--dry-run] [--board <id>] [--no-reset]
#   .gfw (or any file with a 'GMFI' header at 0xC0) is checked with tools/host/mkimage.py --check first.
#   A plain .bin must fit 32 KB and have a sane vector table (SP in SRAM, odd reset vector inside it).
#   --yes skips the "type yes" prompt.
#
# With the stock LDROM still installed (CONFIG0 CBS=0), the chip boots LDROM, which hands off to APROM.
# Undo: restore-stock.sh <backupdir>   (or flash-aprom.sh <backup>/aprom-read1.bin)
source "$(dirname "$0")/common.sh"

file=""; board=1; reset=1
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) ASSUME_YES=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --board) board="$2"; shift ;;
    --no-reset) reset=0 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$file" ] || die "only one file"; file="$1" ;;
  esac
  shift
done
[ -n "$file" ] || { sed -n '2,12p' "$0"; exit 2; }
[ -f "$file" ] || die "$file not found"
file="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"

has_gfw_header() { cmp -s <(dd if="$1" bs=1 skip=192 count=4 2>/dev/null) <(printf GMFI); }
if [[ "$file" == *.gfw ]] || has_gfw_header "$file"; then
  "$PY" "$HOST_DIR/mkimage.py" --check "$file" --board "$board" || die "image check failed; not writing"
else
  "$PY" "$SWD_DIR/bincheck.py" "$file" --region aprom || die "binary check failed; not writing"
fi

size=$(stat -c %s "$file")
echo
echo "PLAN: write APROM"
plan_header
echo "File:   $file"
echo "        $size bytes, sha256 $(sha256sum "$file" | cut -c1-16)..."
echo "Action: erase the APROM pages covering 0x00000000..$(printf 0x%08x $((size - 1))), program, verify_image,"
echo "        then $([ $reset = 1 ] && echo 'reset and run' || echo 'leave the CPU halted')."
echo "        LDROM, data flash and CONFIG are NOT touched."
echo
confirm "Write $(basename "$file") to APROM?"
preflight

log="$(new_log flash-aprom)"
endcmd=(-c "reset run")
[ $reset = 1 ] || endcmd=()
OPENOCD_TIMEOUT="${OPENOCD_TIMEOUT_WRITE:-1800}" run_openocd "$log" \
  -c init -c gemifi_check_pdid -c halt -c "gemifi_write_aprom {$file}" "${endcmd[@]}" -c shutdown \
  || die "APROM write failed (log: $log). The stock image can be put back with restore-stock.sh."
log_has "$log" "GEMIFI WROTE aprom" || die "openocd didn't report a finished write (log: $log)"
is_dry || echo "APROM written and verified: $(basename "$file")"
