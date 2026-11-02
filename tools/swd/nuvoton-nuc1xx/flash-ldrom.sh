#!/usr/bin/env bash
# WRITES LDROM (the boot ROM area the chip starts from while CONFIG0 CBS=0). Guarded.
#
# Usage: flash-ldrom.sh <boot.bin> --i-have-a-backup <backupdir> [--yes] [--dry-run] [--probe <p>]
#        flash-ldrom.sh --verify-only <boot.bin> [--resume <verifydir>] [--dry-run] [--probe <p>]
#   <backupdir> must contain a SHA256SUMS that verifies and a 4096-byte ldrom.bin (make one with dump.sh).
#   <boot.bin> must be <= 4096 bytes with a sane vector table (SP in SRAM, odd reset vector inside it).
#   --probe buspirate|cmsis-dap   probe to use (default buspirate, or $PROBE).
# Erases all 4 KB of LDROM, programs the file, resets, then verifies by ISP read-back (LDROM is not
# readable as plain memory, so verify_image can't be used). CONFIG is never written.
#
# --verify-only  READ-ONLY: no erase, no write. Reads LDROM back by ISP and compares it with the file.
# --resume <dir> continue a verify that stopped, in its folder: only the words not read yet are read.
#                A failed verify prints this exact command.
source "$(dirname "$0")/common.sh"

usage() { sed -n '2,14p' "$0"; }
file=""; backup=""; verify_only=0; resume=""
while [ $# -gt 0 ]; do
  common_opt "$@"; if [ "$COMMON_SHIFT" -gt 0 ]; then shift "$COMMON_SHIFT"; continue; fi
  case "$1" in
    --i-have-a-backup) backup="${2:-}"; shift ;;
    --verify-only) verify_only=1 ;;
    --resume) [ $# -ge 2 ] || die "--resume needs the verify folder"; resume="$2"; shift ;;
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$file" ] || die "only one file"; file="$1" ;;
  esac
  shift
done
[ -n "$file" ] || { usage; exit 2; }
[ -f "$file" ] || die "$file not found"
file="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"
size=$(stat -c %s "$file")

# --- verify only: read-only, never erases or writes ---
if [ "$verify_only" = 1 ]; then
  [ "$size" -le 4096 ] || die "$file is $size bytes; LDROM is 4096"
  if [ -n "$resume" ]; then
    [ -d "$resume" ] || die "no such verify folder: $resume"
    resume="$(cd "$resume" && pwd)"
  fi
  echo
  echo "PLAN: verify LDROM (read-only)"
  plan_header
  echo "File:   $file ($size bytes, sha256 $(sha256sum "$file" | cut -c1-16)...)"
  echo "Action: read LDROM back by ISP and compare it with the file. Nothing is erased or written."
  [ -n "$resume" ] && echo "Resume: $resume (only the words not read yet)"
  echo
  preflight
  isp_verify "$file" 0x00100000 ldrom "$resume"
  rc=$?
  is_dry && exit 0
  [ $rc = 0 ] && echo "LDROM matches $(basename "$file") (read-back: $VERIFY_DIR)"
  exit $rc
fi

[ -z "$resume" ] || die "--resume only works with --verify-only (a write always starts from scratch)"
[ -n "$backup" ] || die "refusing: pass --i-have-a-backup <dir> (a dump.sh folder with ldrom.bin and SHA256SUMS)"
backup="$(cd "$backup" 2>/dev/null && pwd)" || die "no such folder: $backup"

info "checking backup $backup"
"$PY" "$SWD_DIR/backup-check.py" "$backup" --require ldrom.bin:4096 || die "refusing: backup check failed"
"$PY" "$SWD_DIR/bincheck.py" "$file" --region ldrom || die "refusing: $file is not a plausible LDROM image"

recovery() {
  cat <<EOF
RECOVERY (if the new bootloader doesn't come up, or USB is dead):
  1. The ICE pins (ICE_DAT/ICE_CK) are dedicated: no firmware can turn SWD off, so the probe still works.
  2. Put the stock LDROM back:   $SWD_DIR/restore-stock.sh $backup --yes
     (or just LDROM: $0 $backup/ldrom.bin --i-have-a-backup $backup)
  3. If SWD won't attach (code stops clocks or resets in a loop): hold /RESET (U1 pin 46, left edge,
     3rd from the bottom with the dot at bottom left) to GND while openocd connects, release after init.
  4. CONFIG0 isn't touched here, so the chip still boots LDROM (CBS=0) and the stock app stays in APROM.
EOF
}

echo
echo "PLAN: write LDROM"
plan_header
echo "File:   $file ($size bytes, sha256 $(sha256sum "$file" | cut -c1-16)...)"
echo "Backup: $backup (verified; stock LDROM = $backup/ldrom.bin)"
echo "Action: erase LDROM 0x00100000..0x00100FFF (all 8 pages), program the file, reset and run,"
echo "        then read LDROM back by ISP and compare. CONFIG, APROM and data flash are NOT touched."
echo "        With CONFIG0 CBS=0 (stock) the chip boots this code on every reset from now on."
echo
recovery
echo
confirm "Write $(basename "$file") to LDROM?"
preflight

log="$(new_log flash-ldrom)"
run_openocd "$log" -c init -c gemifi_check_pdid -c halt \
  -c "gemifi_write_isp_region ldrom 0x00100000 0x1000 {$file}" -c "reset run" -c shutdown \
  || { recovery; die "LDROM write failed (log: $log)"; }
log_has "$log" "GEMIFI WROTE ldrom" || { recovery; die "openocd didn't report a finished write (log: $log)"; }
isp_verify "$file" 0x00100000 ldrom
case $? in
  0) ;;
  2) die "LDROM was written (openocd confirmed it), but the verify stopped. Resume it with the command above; don't rerun the write." ;;
  *) recovery; die "LDROM verify failed: the words listed above differ from $(basename "$file")" ;;
esac
is_dry || echo "LDROM written and verified: $(basename "$file")"
