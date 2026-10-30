#!/usr/bin/env bash
# READ-ONLY full backup of the NUC120: APROM, LDROM, data flash and CONFIG0/1. Writes nothing to the chip.
#
# Usage: dump.sh <outdir> [--resume] [--compare <old-backup-dir>] [--dry-run]
#
#   APROM (32 KB)      dump_image over the AHB, read twice in two sessions and compared
#                      -> aprom-read1.bin, aprom-read2.bin
#   LDROM (4 KB)       ISP READ, two passes, 256 words per OpenOCD session, resumable -> ldrom.bin
#   data flash (4 KB)  same -> dataflash.bin
#   CONFIG0/1          same -> config.txt
#   SHA256SUMS         over all of the above; the .bin files are made read-only.
#
# Refuses an existing non-empty <outdir> unless --resume is given and the folder was made by dump.sh.
# With the Bus Pirate expect ~7 min per APROM read and ~10-20 min for the ISP passes.
# If a session fails: unplug the keyboard, then the Bus Pirate; plug the Bus Pirate in, then the
# keyboard; rerun with --resume.
source "$(dirname "$0")/common.sh"

out=""; resume=0; compare=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) resume=1 ;;
    --compare) compare="$2"; shift ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$out" ] || die "only one <outdir>"; out="$1" ;;
  esac
  shift
done
[ -n "$out" ] || { sed -n '2,18p' "$0"; exit 2; }

if [ -d "$out" ] && [ -n "$(ls -A "$out" 2>/dev/null)" ]; then
  [ "$resume" = 1 ] || die "$out exists and is not empty. Pick a new folder (or --resume a dump.sh folder)."
  [ -f "$out/.gemifi-dump" ] || die "$out wasn't made by dump.sh (no .gemifi-dump); refusing to resume into it."
fi
is_dry || { mkdir -p "$out/isp" && date -Is >>"$out/.gemifi-dump"; } || die "can't create $out"
out="$(cd "$out" 2>/dev/null && pwd || echo "$out")"

if ! is_dry && [ -f "$out/SHA256SUMS" ] && "$PY" "$SWD_DIR/backup-check.py" "$out" --require aprom:32768 \
     --require ldrom.bin:4096 --require dataflash.bin:4096 --require config.txt 2>/dev/null; then
  echo "$out is already a complete, verified backup. Nothing to do."
  exit 0
fi

info "full backup -> $out (read-only on the chip)"
preflight

# --- APROM: two independent reads ---
aprom_ok() { [ -f "$1" ] && [ "$(stat -c %s "$1")" = 32768 ]; }
for n in 1 2; do
  f="$out/aprom-read$n.bin"
  if ! is_dry && aprom_ok "$f"; then echo "aprom-read$n.bin already done"; continue; fi
  is_dry || rm -f "$f"
  info "APROM read $n of 2 (32 KB; slow on the Bus Pirate)"
  log="$(new_log aprom-read$n)"
  OPENOCD_TIMEOUT="${OPENOCD_TIMEOUT_APROM:-1800}" run_openocd "$log" \
    -c init -c gemifi_check_pdid -c gemifi_show_aprom_head -c "dump_image {$f} 0x00000000 0x8000" -c shutdown \
    || die "APROM read $n failed; rerun with --resume"
  is_dry && continue
  aprom_ok "$f" || die "APROM read $n is incomplete; rerun with --resume"
  if [ "$(log_value "$log" LOCK)" = locked-or-erased ]; then
    die "APROM reads as all 0xFFFFFFFF: the chip is LOCKED (or blank). This dump is worthless. Do not write anything; run read-config.sh and see tools/README.md."
  fi
done
if ! is_dry; then
  cmp -s "$out/aprom-read1.bin" "$out/aprom-read2.bin" || {
    rm -f "$out/aprom-read2.bin"
    die "the two APROM reads differ (unreliable SWD link?). aprom-read2.bin removed; rerun with --resume to read it again."
  }
  echo "APROM: both reads identical"
fi

# --- LDROM, data flash, CONFIG: ISP, two passes each ---
regions=("ldrom 0x00100000 0x1000" "dataflash 0x0001F000 0x1000" "config 0x00300000 0x8")
for pass in 1 2; do
  for r in "${regions[@]}"; do
    # shellcheck disable=SC2086
    set -- $r
    info "ISP $1 pass $pass"
    isp_dump_range "$out" "$1" "$2" "$3" "$pass" || die "ISP dump stopped; replug (keyboard out first), then rerun: $0 $out --resume"
  done
done
if ! is_dry; then
  "$PY" "$SWD_DIR/isp-assemble.py" "$out" ldrom 0x00100000 0x1000 || die "LDROM passes don't match; delete $out/isp/ldrom-pass2.txt and rerun --resume"
  "$PY" "$SWD_DIR/isp-assemble.py" "$out" dataflash 0x0001F000 0x1000 || die "data flash passes don't match; delete $out/isp/dataflash-pass2.txt and rerun --resume"
  "$PY" "$SWD_DIR/isp-assemble.py" "$out" config 0x00300000 0x8 --config || die "CONFIG passes don't match"
fi

# --- checksums ---
if is_dry; then
  echo "[dry-run] would write $out/SHA256SUMS and chmod a-w the .bin files"
  exit 0
fi
(cd "$out" && sha256sum aprom-read1.bin aprom-read2.bin ldrom.bin dataflash.bin config.txt >SHA256SUMS) || die "sha256sum failed"
chmod a-w "$out"/*.bin "$out/config.txt"
"$PY" "$SWD_DIR/backup-check.py" "$out" --require aprom:32768 --require ldrom.bin:4096 --require dataflash.bin:4096 || die "backup check failed"
echo
"$PY" "$SWD_DIR/nuc1xx-config.py" decode --file "$out/config.txt"

if [ -n "$compare" ]; then
  echo; info "compare with $compare"
  for f in aprom-read1.bin ldrom.bin dataflash.bin config.txt; do
    if [ -f "$compare/$f" ]; then
      if [ "$f" = config.txt ]; then
        a="$("$PY" "$SWD_DIR/nuc1xx-config.py" get CONFIG0 --file "$out/$f")"; b="$("$PY" "$SWD_DIR/nuc1xx-config.py" get CONFIG0 --file "$compare/$f")"
        [ "$a" = "$b" ] && echo "  same       $f (CONFIG0 $a)" || echo "  DIFFERENT  $f (now $a, then $b)"
      else
        cmp -s "$out/$f" "$compare/$f" && echo "  same       $f" || echo "  DIFFERENT  $f"
      fi
    else
      echo "  (missing)  $f in $compare"
    fi
  done
fi
echo
echo "Backup complete: $out"
echo "Keep a second copy somewhere off this machine (it is copyrighted firmware: don't commit or publish it)."
