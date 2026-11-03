#!/usr/bin/env bash
# SWD helpers for the M-Audio Code 61 (STM32F103RBT6) through a Bus Pirate on header JMP1.
# Wiring and power: see openocd-buspirate.cfg next to this script.
#
#   code61.sh hold                 halt, drive PC15 low (keeps the board on), print chip info, leave it halted
#   code61.sh run                  like hold, then let the app run with PC15 already low
#   code61.sh backup OUT.bin       hold, then read all 128 KB (private: never commit it)
#   code61.sh write-app IMG.bin    hold, then write IMG at 0x08002400 and reset: a .gfw, or a full app area
#                                  (maudio_update.py flashbin) under M-Audio's bootloader
#   code61.sh write-boot BOOT.bin  hold, then write the gemifi bootloader (bootloader/stm32f1) at 0x08000000
#
# Options (before the command):
#   --port /dev/ttyUSBn   Bus Pirate port (default /dev/ttyUSB0)
#   --bp-power            power the board from the Bus Pirate's 3.3 V: BP +3.3V -> JMP1-4, keyboard USB unplugged
#   --tries N             connection attempts (default 40, about 1-2 s each)
#
# The board powers itself off a moment after the power button is pressed, so each command retries the
# connection: start it, then press the power button (again if needed) until it connects. For writes, keep
# the power button held (or use --bp-power): the board has dropped power mid-write without it.
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BP_PORT=/dev/ttyUSB0
BP_VREG=0
TRIES=40

while [ $# -gt 0 ]; do
  case "$1" in
    --port) BP_PORT="$2"; shift 2 ;;
    --bp-power) BP_VREG=1; shift ;;
    --tries) TRIES="$2"; shift 2 ;;
    *) break ;;
  esac
done
[ $# -ge 1 ] || { sed -n '2,17p' "$0"; exit 2; }
cmd="$1"; shift

case "$cmd" in
  hold)      steps=(-c code61_hold_power -c code61_info) ;;
  run)       steps=(-c code61_hold_power -c code61_info -c resume) ;;
  backup)    [ $# -eq 1 ] || { echo "usage: code61.sh backup OUT.bin" >&2; exit 2; }
             steps=(-c code61_hold_power -c code61_info -c "code61_backup {$(realpath -m "$1")}") ;;
  write-app) [ $# -eq 1 ] || { echo "usage: code61.sh write-app IMG.bin" >&2; exit 2; }
             steps=(-c code61_hold_power -c code61_info -c "code61_write_app {$(realpath "$1")}" -c "reset run") ;;
  write-boot) [ $# -eq 1 ] || { echo "usage: code61.sh write-boot BOOT.bin" >&2; exit 2; }
             steps=(-c code61_hold_power -c code61_info -c "code61_write_boot {$(realpath "$1")}" -c "reset run") ;;
  *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac

base=(openocd -s "$DIR" -c "set BP_PORT {$BP_PORT}" -c "set BP_VREG $BP_VREG"
      -f "$DIR/openocd-buspirate.cfg" -f "$DIR/lib.tcl" -c init)

echo "== press the keyboard's power button now (and again if it goes dark); $TRIES attempts"
for i in $(seq 1 "$TRIES"); do
  if "${base[@]}" "${steps[@]}" -c exit 2>&1 | tee /tmp/code61-swd.log | grep -E "GEMIFI|Error|error" ; then :; fi
  if grep -q "GEMIFI IDCODE" /tmp/code61-swd.log && ! grep -q "GEMIFI ABORT" /tmp/code61-swd.log; then
    if [ "$cmd" = backup ] && ! grep -q "GEMIFI BACKUP" /tmp/code61-swd.log; then
      echo "== connected, but the backup didn't finish (log: /tmp/code61-swd.log)"; exit 1
    fi
    if [ "${cmd#write-}" != "$cmd" ] && ! grep -q "GEMIFI WROTE" /tmp/code61-swd.log; then
      echo "== connected, but the write didn't finish (log: /tmp/code61-swd.log); retry"; exit 1
    fi
    echo "== done (attempt $i); full log: /tmp/code61-swd.log"
    exit 0
  fi
  grep -q "GEMIFI ABORT" /tmp/code61-swd.log && exit 1
  echo "-- attempt $i: no target yet"
done
echo "== gave up after $TRIES attempts; last log: /tmp/code61-swd.log"
exit 1
