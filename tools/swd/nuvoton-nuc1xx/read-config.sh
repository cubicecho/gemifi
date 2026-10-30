#!/usr/bin/env bash
# READ-ONLY. Reads CONFIG0/CONFIG1 by ISP READ (CPU halted for a moment, then reset/run) and decodes
# them (CBS, LOCK, CFOSC, BOD ... per the NUC100/120 TRM). Writes nothing to flash.
#
# Usage: read-config.sh [--dry-run]
#        read-config.sh --decode <CONFIG0> [<CONFIG1>]     (offline, no hardware)
#        read-config.sh --decode-file <config.txt>          (offline, e.g. a backup's config.txt)
source "$(dirname "$0")/common.sh"
case "${1:-}" in
  --decode) shift; exec "$PY" "$SWD_DIR/nuc1xx-config.py" decode "$@" ;;
  --decode-file) exec "$PY" "$SWD_DIR/nuc1xx-config.py" decode --file "$2" ;;
  --dry-run) DRY_RUN=1 ;;
  -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
  "") ;;
  *) die "unknown argument: $1" ;;
esac

preflight
read_config_values || exit 1
is_dry && exit 0
echo
"$PY" "$SWD_DIR/nuc1xx-config.py" decode "$CFG0" "$CFG1"
