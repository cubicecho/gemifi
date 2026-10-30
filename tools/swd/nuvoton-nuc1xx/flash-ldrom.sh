#!/usr/bin/env bash
# WRITES LDROM (the boot ROM area the chip starts from while CONFIG0 CBS=0). Guarded.
#
# Usage: flash-ldrom.sh <boot.bin> --i-have-a-backup <backupdir> [--yes] [--dry-run]
#   <backupdir> must contain a SHA256SUMS that verifies and a 4096-byte ldrom.bin (make one with dump.sh).
#   <boot.bin> must be <= 4096 bytes with a sane vector table (SP in SRAM, odd reset vector inside it).
# Erases all 4 KB of LDROM, programs the file, resets, then verifies by ISP read-back (LDROM is not
# readable as plain memory, so verify_image can't be used). CONFIG is never written.
source "$(dirname "$0")/common.sh"

file=""; backup=""
while [ $# -gt 0 ]; do
  case "$1" in
    --i-have-a-backup) backup="${2:-}"; shift ;;
    -y|--yes) ASSUME_YES=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$file" ] || die "only one file"; file="$1" ;;
  esac
  shift
done
[ -n "$file" ] || { sed -n '2,9p' "$0"; exit 2; }
[ -f "$file" ] || die "$file not found"
[ -n "$backup" ] || die "refusing: pass --i-have-a-backup <dir> (a dump.sh folder with ldrom.bin and SHA256SUMS)"
file="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"
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

size=$(stat -c %s "$file")
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
isp_verify "$file" 0x00100000 ldrom || { recovery; die "LDROM verify failed"; }
is_dry || echo "LDROM written and verified: $(basename "$file")"
