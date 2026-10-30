# shellcheck shell=bash
# Shared setup for the NUC1xx SWD scripts. Sourced, not run.
#
# Environment:
#   PROBE=buspirate|cmsis-dap   probe to use (default buspirate)
#   BP_PORT=/dev/ttyUSB0        Bus Pirate serial port
#   DRY_RUN=1                   print the openocd commands instead of running them (touches nothing)
#   OPENOCD_TIMEOUT=900         seconds before an openocd session is killed (long APROM sessions use more)
#   LOG_DIR=<dir>               where session logs go (default: logs/ next to these scripts)

set -u
set -o pipefail

SWD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SWD_DIR/../../.." && pwd)"
HOST_DIR="$REPO_ROOT/tools/host"
PROBE="${PROBE:-buspirate}"
BP_PORT="${BP_PORT:-/dev/ttyUSB0}"
DRY_RUN="${DRY_RUN:-0}"
OPENOCD_TIMEOUT="${OPENOCD_TIMEOUT:-900}"
LOG_DIR="${LOG_DIR:-$SWD_DIR/logs}"
ASSUME_YES="${ASSUME_YES:-0}"
PDID_EXPECTED=0x10012008
PY="${PYTHON:-python3}"

case "$PROBE" in
  buspirate) PROBE_CFG="$SWD_DIR/openocd-buspirate.cfg" ;;
  cmsis-dap|pico|debugprobe) PROBE=cmsis-dap; PROBE_CFG="$SWD_DIR/openocd-cmsis-dap.cfg" ;;
  *) echo "Unknown PROBE='$PROBE' (use buspirate or cmsis-dap)" >&2; exit 2 ;;
esac

OPENOCD_BASE=(openocd -s "$SWD_DIR")
[ "$PROBE" = buspirate ] && OPENOCD_BASE+=(-c "set BP_PORT $BP_PORT")
OPENOCD_BASE+=(-f "$PROBE_CFG" -f "$SWD_DIR/lib.tcl")

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARNING: $*" >&2; }
info() { echo "== $*"; }
is_dry() { [ "$DRY_RUN" = 1 ]; }

stuck_hint() {
  cat >&2 <<'EOF'
If the Bus Pirate stops responding (an openocd session was killed, or it timed out), it is stuck in
binary mode. Recover it: unplug the KEYBOARD first, then the Bus Pirate; plug the Bus Pirate back in
first and the keyboard last. Then rerun (dumps resume where they stopped).
EOF
}

power_reminder() {
  if [ "$PROBE" = buspirate ]; then
    echo "   Power order: Bus Pirate plugged in FIRST, keyboard LAST. When done: unplug the keyboard FIRST."
  else
    echo "   Power order: probe plugged in FIRST, keyboard LAST. When done: unplug the keyboard FIRST."
  fi
}

# Probe checks. Reads sysfs and lsusb only; never opens the serial port. Skipped in DRY_RUN.
preflight() {
  if is_dry; then
    info "DRY_RUN=1: skipping probe checks; openocd commands are printed, not run"
    return 0
  fi
  command -v openocd >/dev/null || die "openocd not found in PATH"
  if pgrep -x openocd >/dev/null 2>&1; then
    die "another openocd is already running (pid $(pgrep -x openocd | tr '\n' ' ')). Stop it first; two sessions on one probe will wedge it."
  fi
  if [ "$PROBE" = buspirate ]; then
    [ -e "$BP_PORT" ] || die "$BP_PORT not found. Plug in the Bus Pirate (before the keyboard), or set BP_PORT."
    local lt="/sys/bus/usb-serial/devices/$(basename "$BP_PORT")/latency_timer"
    if [ -r "$lt" ]; then
      local v; v="$(cat "$lt")"
      if [ "$v" != 1 ]; then
        warn "FTDI latency timer is ${v} ms ($lt); it must be 1 ms or SWD is ~16x slower and long sessions time out."
        warn "Fix now: echo 1 | sudo tee $lt   (the udev rule should do this on plug-in; see tools/README.md)"
      else
        echo "   latency timer: 1 ms OK"
      fi
    else
      warn "can't read $lt; check the FTDI latency timer is 1 ms"
    fi
  else
    if command -v lsusb >/dev/null && ! lsusb | grep -qiE '2e8a:000c|cmsis-dap|debugprobe'; then
      warn "no Raspberry Pi debugprobe (2e8a:000c) in lsusb; is the Pico plugged in?"
    fi
  fi
  power_reminder
}

# Print a command so it can be pasted into a shell.
print_cmd() {
  local out="" a
  for a in "$@"; do out+="$(printf '%q' "$a") "; done
  echo "${out% }"
}

# run_openocd <logfile> <openocd args after the probe config + lib.tcl...>
# Output goes to the terminal and is appended to <logfile>. Returns openocd's exit status.
run_openocd() {
  local log="$1"; shift
  local cmd=("${OPENOCD_BASE[@]}" "$@")
  if is_dry; then
    echo "[dry-run] $(print_cmd "${cmd[@]}")"
    return 0
  fi
  mkdir -p "$(dirname "$log")"
  echo "\$ $(print_cmd "${cmd[@]}")" >>"$log"
  trap 'echo; warn "interrupted while openocd was running."; stuck_hint' INT
  timeout -s KILL "$OPENOCD_TIMEOUT" "${cmd[@]}" 2>&1 | tee -a "$log"
  local rc=${PIPESTATUS[0]}
  trap - INT
  if [ "$rc" = 137 ]; then
    warn "openocd was killed after ${OPENOCD_TIMEOUT} s."
    stuck_hint
  elif [ "$rc" != 0 ]; then
    warn "openocd exited with status $rc (log: $log)"
    [ "$PROBE" = buspirate ] && stuck_hint
  fi
  return "$rc"
}

# confirm "<question>": --yes / ASSUME_YES=1 skips it; DRY_RUN skips it; otherwise needs a tty.
confirm() {
  if is_dry; then echo "[dry-run] would ask: $1"; return 0; fi
  if [ "$ASSUME_YES" = 1 ]; then echo "(--yes given) $1"; return 0; fi
  [ -t 0 ] || die "not a terminal and --yes not given; refusing to write."
  local ans
  read -r -p "$1 Type 'yes' to continue: " ans
  [ "$ans" = yes ] || die "aborted; nothing was written."
}

new_log() { echo "$LOG_DIR/$1-$(date +%Y%m%d-%H%M%S).log"; }

# Require a marker line in a log (proof that a step really ran to the end).
log_has() { is_dry || grep -q "$2" "$1"; }

# Read a "GEMIFI <KEY> <value>" line from a log.
log_value() { grep -a "^GEMIFI $2 " "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }

# isp_dump_range <dumpdir> <name> <base> <size> <pass>
# Resumable ISP read of [base, base+size) into <dumpdir>/isp/<name>-pass<pass>.txt, one OpenOCD session
# per 256 words (long Bus Pirate sessions lock up). Addresses already in the file are skipped.
isp_dump_range() {
  local dir="$1" name="$2" base=$(($3)) size=$(($4)) pass="$5" chunk=1024
  local out="$dir/isp/$name-pass$pass.txt" s e start
  is_dry || { mkdir -p "$dir/isp"; touch "$out"; }
  for ((s = base; s < base + size; s += chunk)); do
    e=$((s + chunk)); [ $e -gt $((base + size)) ] && e=$((base + size))
    start=$s
    while [ $start -lt $e ] && [ -f "$out" ] && grep -q "^$(printf %08x $start) [0-9a-f]\{8\}$" "$out"; do
      start=$((start + 4))
    done
    [ $start -ge $e ] && continue
    echo "$(date +%T) $name pass$pass: $(printf 0x%08x $start)..$(printf 0x%08x $e)"
    local log; log="$(new_log "isp-$name")"
    if ! run_openocd "$log" -d1 -c "set START $start; set END $e; set OUT {$out}" -f "$SWD_DIR/isp-chunk.tcl"; then
      echo "ISP read stopped at $(printf 0x%08x $start). Rerun the same command to resume." >&2
      return 1
    fi
    if ! is_dry && grep -qE 'Error|GEMIFI ABORT' "$log"; then
      echo "OpenOCD reported an error (log: $log). Replug as below, then rerun to resume." >&2
      stuck_hint
      return 1
    fi
  done
  is_dry || echo "$name pass$pass: $(wc -l <"$out") of $((size / 4)) words"
}

# isp_verify <file> <base> <name>: read back len(file) bytes (rounded up to 4) by ISP and compare.
isp_verify() {
  local file="$1" base="$2" name="$3"
  local len; len=$(( ($(stat -c %s "$file") + 3) / 4 * 4 ))
  local vdir; vdir="$LOG_DIR/verify-$name-$(date +%Y%m%d-%H%M%S)"
  info "verifying $name by ISP read-back ($len bytes from $(printf 0x%08x "$base"))"
  isp_dump_range "$vdir" "$name" "$base" "$len" 1 || return 1
  is_dry && return 0
  "$PY" "$SWD_DIR/isp-assemble.py" "$vdir" "$name" "$base" "$len" --passes 1 --out "$vdir/readback.bin" || return 1
  # Pad the source with 0xFF to a multiple of 4 (erased flash) before comparing.
  "$PY" - "$file" "$vdir/readback.bin" <<'EOF' || return 1
import sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read()
a += b"\xff" * (len(b) - len(a))
if a != b:
    bad = next(i for i in range(len(a)) if a[i] != b[i])
    print(f"VERIFY FAILED: first difference at +0x{bad:x} (wrote {a[bad]:02x}, read {b[bad]:02x})"); sys.exit(1)
print(f"VERIFY OK: {len(b)} bytes match")
EOF
}

# Read CONFIG0/1 by ISP in one short session; sets CFG0/CFG1 (empty in DRY_RUN).
read_config_values() {
  local log; log="$(new_log read-config)"
  CFG0=""; CFG1=""
  run_openocd "$log" -c init -c gemifi_check_pdid -c halt -c gemifi_read_config -c "reset run" -c shutdown || return 1
  is_dry && return 0
  CFG0="$(log_value "$log" CONFIG0)"; CFG1="$(log_value "$log" CONFIG1)"
  [ -n "$CFG0" ] && [ -n "$CFG1" ] || { warn "CONFIG not found in the openocd output (log: $log)"; return 1; }
}

# Show what a write will do: probe, chip, files.
plan_header() {
  echo "Probe:  $PROBE ($(basename "$PROBE_CFG"))$([ "$PROBE" = buspirate ] && echo ", port $BP_PORT")"
  echo "Chip:   must read PDID $PDID_EXPECTED (NUC120LC1BN) or nothing is written"
}
