"""Drive firmware/nuvoton-nuc1xx/usb-log/main.c (built natively as build/libapp.so):
HELLO, Identity Request, FW_* rejection, REBOOT, button EVENT/LOG. Run via `make test`."""
import ctypes, os, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / "tools" / "host"))
import gemifi_proto as gp
BUILD = Path(os.environ.get("SIM_BUILD", HERE / "build"))
L = ctypes.CDLL(str(BUILD / "libapp.so")); L.sim_init()
fails = 0
def check(c, m):
    global fails; print(("ok   " if c else "FAIL ") + m); fails += (not c)
def feed(b):
    buf = (ctypes.c_uint8*4096)(); n = L.sim_feed(bytes(b), len(b), buf); return bytes(buf[:n])
def frames(data):
    return [gp.parse_frame(m) for m in gp.SysexAssembler().feed(data)]
r = frames(feed(gp.build_frame(gp.HELLO, 1)))
h = gp.parse_hello(r[0].payload); print("    ", h)
check(gp.reply_matches(r[0], gp.HELLO, 1) and h.role == 0 and h.max_chunk == 0 and h.caps == 0x0C, "HELLO app")
idr = feed(gp.IDENTITY_REQUEST); print("     identity:", idr.hex(" "))
check(idr[:5] == bytes([0xF0,0x7E,0x7F,0x06,0x02]) and idr[-1] == 0xF7 and len(idr) == 15, "Identity reply")
for c in (gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT):
    r = frames(feed(gp.build_frame(c, 2 + c, b"")))
    check(r[0].payload == bytes([4]), f"cmd {c:#x} -> BAD_STATE")
r = frames(feed(gp.build_frame(gp.REBOOT, 9, b"\x01")))
check(gp.reply_matches(r[0], gp.REBOOT, 9) and r[0].status == 0 and L.sim_reboot_mode() == 2, "REBOOT(1)")
buf = (ctypes.c_uint8*4096)(); n = L.sim_press(3, 1, buf); d = bytes(buf[:n])
check(d[:4] == bytes([0x09, 0x90, 51, 100]), "note on for oct-")
# remaining bytes are USB-MIDI events; unpack sysex
raw = d[4:]   # the stub midi_tx_sysex records raw SysEx bytes
fs = frames(bytes(raw))
ev = gp.parse_event(fs[0].payload); lg = gp.parse_log(fs[1].payload)
print("    ", ev, lg)
check(fs[0].cmd == gp.EVENT and ev.index == 3 and ev.value == 1 and fs[1].cmd == gp.LOG and lg.text == "oct- down", "EVENT + LOG")
print("FAILS:", fails); sys.exit(1 if fails else 0)
