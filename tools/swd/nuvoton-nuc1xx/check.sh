#!/usr/bin/env bash
# READ-ONLY. Connects over SWD with the core left running, reads the chip ID (PDID) and the first
# 4 words of APROM (lock check: all 0xFFFFFFFF = locked or blank, a vector table = unlocked).
# Writes nothing to the chip. For CONFIG0/1 (needs a brief halt) use read-config.sh.
#
# Usage: check.sh [--dry-run] [--probe buspirate|cmsis-dap] [-h]
#   PROBE=buspirate|cmsis-dap  BP_PORT=/dev/ttyUSB0  DRY_RUN=1   (see common.sh)
source "$(dirname "$0")/common.sh"
while [ $# -gt 0 ]; do
  common_opt "$@"; if [ "$COMMON_SHIFT" -gt 0 ]; then shift "$COMMON_SHIFT"; continue; fi
  case "$1" in
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

preflight
log="$(new_log check)"
run_openocd "$log" -c init -c gemifi_check_pdid -c gemifi_show_aprom_head -c "mdw 0x0 4" -c "flash banks" -c shutdown
rc=$?
is_dry && exit 0
echo
pdid="$(log_value "$log" PDID)"; lock="$(log_value "$log" LOCK)"
echo "Summary (log: $log)"
echo "  PDID: ${pdid:-not read}  $([ "${pdid,,}" = "${PDID_EXPECTED,,}" ] && echo '= NUC120LC1BN OK' || echo '!= expected 0x10012008')"
case "$lock" in
  unlocked) echo "  APROM: readable (chip unlocked)" ;;
  locked-or-erased) echo "  APROM: reads all 0xFFFFFFFF: LOCKED or blank. Do not write anything; run read-config.sh and read tools/README.md." ;;
  *) echo "  APROM: not read" ;;
esac
exit $rc
