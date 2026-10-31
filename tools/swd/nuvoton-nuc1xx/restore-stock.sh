#!/usr/bin/env bash
# WRITES APROM + LDROM from a backup (stock firmware), verifies both, then checks CONFIG0 against the
# backed-up value. CONFIG is never written unless --write-config is given.
#
# Usage: restore-stock.sh <backupdir> [--with-dataflash] [--write-config] [--yes] [--dry-run] [--probe <p>]
#   <backupdir>        a dump.sh folder (or the original research backup): SHA256SUMS must verify;
#                      needs aprom.bin or aprom-read1.bin (32 KB), ldrom.bin (4 KB), config.txt.
#   --with-dataflash   also restore dataflash.bin (stock settings; gemifi firmware may have used it).
#   --write-config     if CONFIG0/1 differ from config.txt, show both decoded and offer to write the
#                      backed-up values. Needs typing "write config" at the prompt; --yes does not skip it.
#   --yes              skip the "type yes" prompt for the APROM/LDROM/data flash writes.
#   --probe buspirate|cmsis-dap   probe to use (default buspirate, or $PROBE).
# If an ISP read-back verify stops partway, the write is done: resume just the verify with the
# command it prints (no erase, no write) instead of rerunning the whole restore.
# Each region is written in its own short OpenOCD session (long Bus Pirate sessions lock up).
source "$(dirname "$0")/common.sh"

backup=""; dataflash=0; writecfg=0
while [ $# -gt 0 ]; do
  common_opt "$@"; if [ "$COMMON_SHIFT" -gt 0 ]; then shift "$COMMON_SHIFT"; continue; fi
  case "$1" in
    --with-dataflash) dataflash=1 ;;
    --write-config) writecfg=1 ;;
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) [ -z "$backup" ] || die "only one backup dir"; backup="$1" ;;
  esac
  shift
done
[ -n "$backup" ] || { sed -n '2,15p' "$0"; exit 2; }
backup="$(cd "$backup" && pwd)" || die "no such folder"

req=(--require aprom:32768 --require ldrom.bin:4096)
[ $dataflash = 1 ] && req+=(--require dataflash.bin:4096)
info "checking backup $backup"
aprom="$("$PY" "$SWD_DIR/backup-check.py" "$backup" "${req[@]}" --print-aprom)" || die "backup check failed; not writing"
[ -f "$backup/config.txt" ] || die "$backup/config.txt missing"
want0="$("$PY" "$SWD_DIR/nuc1xx-config.py" get CONFIG0 --file "$backup/config.txt")" || die "bad config.txt"
want1="$("$PY" "$SWD_DIR/nuc1xx-config.py" get CONFIG1 --file "$backup/config.txt")" || die "bad config.txt"
"$PY" "$SWD_DIR/bincheck.py" "$aprom" --region aprom >/dev/null || die "backup APROM image looks wrong"
"$PY" "$SWD_DIR/bincheck.py" "$backup/ldrom.bin" --region ldrom >/dev/null || die "backup LDROM image looks wrong"

echo
echo "PLAN: restore stock firmware"
plan_header
echo "From:   $backup"
echo "  1. APROM  <- $(basename "$aprom") (32 KB): erase, program, verify_image"
echo "  2. LDROM  <- ldrom.bin (4 KB): erase all, program, verify by ISP read-back"
[ $dataflash = 1 ] && echo "  3. data flash <- dataflash.bin (4 KB): erase all, program, verify by ISP read-back"
echo "  then read CONFIG0/1 and compare with config.txt (CONFIG0 $want0, CONFIG1 $want1)."
if [ $writecfg = 1 ]; then
  echo "  CONFIG will be written ONLY if it differs, after showing both values and a typed confirmation."
else
  echo "  CONFIG is NOT written (no --write-config)."
fi
echo
confirm "Restore the stock firmware from this backup?"
preflight

info "1/3 APROM"
log="$(new_log restore-aprom)"
OPENOCD_TIMEOUT="${OPENOCD_TIMEOUT_WRITE:-1800}" run_openocd "$log" \
  -c init -c gemifi_check_pdid -c halt -c "gemifi_write_aprom {$aprom}" -c "reset run" -c shutdown \
  || die "APROM restore failed (log: $log); fix the link and rerun"
log_has "$log" "GEMIFI WROTE aprom" || die "APROM write not confirmed (log: $log)"

info "2/3 LDROM"
log="$(new_log restore-ldrom)"
run_openocd "$log" -c init -c gemifi_check_pdid -c halt \
  -c "gemifi_write_isp_region ldrom 0x00100000 0x1000 {$backup/ldrom.bin}" -c "reset run" -c shutdown \
  || die "LDROM restore failed (log: $log); fix the link and rerun"
log_has "$log" "GEMIFI WROTE ldrom" || die "LDROM write not confirmed (log: $log)"
isp_verify "$backup/ldrom.bin" 0x00100000 ldrom \
  || die "LDROM verify did not pass (see above). Steps after it (CONFIG check) were skipped; run read-config.sh after."

if [ $dataflash = 1 ]; then
  info "3/3 data flash"
  log="$(new_log restore-dataflash)"
  run_openocd "$log" -c init -c gemifi_check_pdid -c halt \
    -c "gemifi_write_isp_region dataflash 0x0001F000 0x1000 {$backup/dataflash.bin}" -c "reset run" -c shutdown \
    || die "data flash restore failed (log: $log)"
  log_has "$log" "GEMIFI WROTE dataflash" || die "data flash write not confirmed (log: $log)"
  isp_verify "$backup/dataflash.bin" 0x0001F000 dataflash \
    || die "data flash verify did not pass (see above). The CONFIG check was skipped; run read-config.sh after."
fi

info "CONFIG check"
read_config_values || die "couldn't read CONFIG"
if is_dry; then
  echo "[dry-run] would compare CONFIG0/1 with $want0 / $want1$([ $writecfg = 1 ] && echo ' and offer to write them if different')"
  exit 0
fi
if [ $((CFG0)) -eq $((want0)) ] && [ $((CFG1)) -eq $((want1)) ]; then
  echo "CONFIG0 $CFG0 / CONFIG1 $CFG1 match the backup. Stock firmware restored."
  echo "Power-cycle the keyboard (unplug it first, before the probe) and check it works as a MIDI keyboard."
  exit 0
fi

echo
warn "CONFIG on the chip ($CFG0 / $CFG1) differs from the backup ($want0 / $want1)."
"$PY" "$SWD_DIR/nuc1xx-config.py" diff --old "$CFG0" "$CFG1" --new "$want0" "$want1"
drc=$?
if [ $writecfg != 1 ]; then
  echo "CONFIG was NOT written. To write the backed-up values, rerun with --write-config."
  exit 3
fi
[ $drc = 0 ] || die "refusing to write that CONFIG value"
[ -t 0 ] || die "--write-config needs an interactive terminal"
echo
echo "Writing CONFIG erases the CONFIG page and writes both words. An interrupted erase leaves"
echo "0xFFFFFFFF (APROM boot, internal RC, unlocked, no BOD), which is recoverable over SWD."
read -r -p "Type 'write config' to write CONFIG0=$want0 CONFIG1=$want1: " ans
[ "$ans" = "write config" ] || die "aborted; CONFIG not written"
log="$(new_log restore-config)"
run_openocd "$log" -c init -c gemifi_check_pdid -c halt \
  -c "gemifi_write_config $want0 $want1" -c shutdown || die "CONFIG write failed (log: $log). Read it with read-config.sh."
echo "CONFIG written and read back OK. CONFIG takes effect after a power cycle:"
echo "unplug the keyboard first, then plug it back in (probe stays plugged)."
