"""Drive the real bootloader main.c (built natively as build/libboot*.so) with frames
from tools/host/gemifi_proto.py. Run via `make test`."""
import ctypes, sys, zlib, struct, os
from pathlib import Path
HERE = Path(__file__).resolve().parent
R = str(HERE.parents[3])                              # repo root
BUILD = Path(os.environ.get("SIM_BUILD", HERE / "build"))
sys.path.insert(0, R + "/tools/host")
import gemifi_proto as gp
import gemifi_image as gi

GFW = open(R + "/firmware/nuvoton-nuc1xx/hello-led/build/hello-led.gfw", "rb").read()
fails = 0
def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails += 1

class Dev:
    def __init__(self, lib):
        self.L = ctypes.CDLL(lib)
        self.L.sim_serial.restype = ctypes.c_char_p
        self.flash = (ctypes.c_uint8 * 0x20000).in_dll(self.L, "sim_flash")
        self.seq = 0
    def setflash(self, base, data):
        ctypes.memmove(ctypes.addressof(self.flash) + base, data, len(data))
    def getflash(self, base, n):
        return bytes(self.flash[base:base+n])
    def boot(self, buttons=0, mailbox=None):
        ctypes.c_uint32.in_dll(self.L, "sim_buttons").value = buttons
        mb = (ctypes.c_uint32 * 4).in_dll(self.L, "sim_mailbox")
        if mailbox: mb[0], mb[1] = mailbox, (~mailbox) & 0xFFFFFFFF
        else: mb[0] = mb[1] = 0x12345678
        r = self.L.sim_boot()
        return r, mb[0]
    def raw(self, msg):
        buf = (ctypes.c_uint8 * 4096)()
        n = self.L.sim_feed(bytes(msg), len(msg), buf)
        data = bytes(buf[:n])
        out = []
        a = gp.SysexAssembler()
        for m in a.feed(data): out.append(gp.parse_frame(m))
        return out
    def req(self, cmd, payload=b"", seq=None):
        if seq is None:
            self.seq = (self.seq + 1) & 0x7F; seq = self.seq
        r = self.raw(gp.build_frame(cmd, seq, payload))
        assert len(r) == 1, r
        f = r[0]
        assert gp.reply_matches(f, cmd, seq), (f, cmd, seq)
        return f

def upload(d, img, base, mc, tamper=None, wrongver=None):
    crc = zlib.crc32(img[:0xCC] + b"\0\0\0\0" + img[0xD0:]) & 0xFFFFFFFF
    ver = struct.unpack_from("<I", img, 0xD0)[0]
    f = d.req(gp.FW_BEGIN, gp.req_fw_begin(len(img), crc, 1, ver if wrongver is None else wrongver))
    if f.status: return ("begin", f.status)
    for off, n in gp.chunk_plan(len(img), mc):
        chunk = img[off:off+n]
        if tamper and off == tamper: chunk = bytes([chunk[0] ^ 1]) + chunk[1:]
        f = d.req(gp.FW_DATA, gp.req_fw_data(off, chunk))
        if f.status: return ("data", off, f.status)
        check_ok = gp.dec_u28(f.payload[1:5]) == off
        if not check_ok: return ("echo", off)
    f = d.req(gp.FW_END)
    return ("end", f.status)

for variant in ["libboot.so", "libboot_test.so"]:
    test = "test" in variant
    base = 0x1F000 if test else 0
    print("====", variant)
    d = Dev(str(BUILD / variant))
    d.setflash(0, b"\xff" * 0x20000)
    r, mb = d.boot()
    check(r == 3, f"blank flash -> update mode (r={r})")
    check(mb == 0, "mailbox magic cleared")
    d.L.sim_enter_update()
    print("     serial:", d.L.sim_serial())
    f = d.req(gp.HELLO)
    h = gp.parse_hello(f.payload)
    print("     hello:", h)
    check(f.status == 0 and h.role == 1 and h.board_id == 1 and h.max_chunk == 256 and gp.valid_max_chunk(h.max_chunk), "HELLO")
    # FW_DATA before BEGIN
    f = d.req(gp.FW_DATA, gp.req_fw_data(0x200, b"\0"*4)); check(f.status == gp.BAD_STATE if hasattr(gp,'BAD_STATE') else f.status == 4, "FW_DATA without BEGIN -> BAD_STATE")
    f = d.req(gp.FW_BEGIN, gp.req_fw_begin(len(GFW), 0, 2, 0)); check(f.status == 8, "wrong board -> BAD_BOARD")
    f = d.req(gp.FW_BEGIN, gp.req_fw_begin(0x8004, 0, 1, 0)); check(f.status == 5, "size too big -> BAD_ADDR")
    f = d.req(gp.FW_BEGIN, gp.req_fw_begin(0x102, 0, 1, 0)); check(f.status == 5, "size not multiple of 4 -> BAD_ADDR")
    f = d.req(0x05); check(f.status == 2, "unknown cmd -> BAD_CMD")
    # bad checksum
    m = bytearray(gp.build_frame(gp.HELLO, 99)); m[-2] ^= 1
    r = d.raw(m); check(len(r) == 1 and r[0].payload == bytes([1]) and r[0].seq == 99, "bad checksum -> BAD_CHECKSUM, status only")
    # reply frames are ignored
    r = d.raw(gp.build_frame(0x41, 5, b"\0")); check(r == [], "reply-range frame ignored")
    r = d.raw(bytes([0xF0,0x7E,0x7F,0x06,0x01,0xF7])); check(r == [], "identity request ignored by bootloader")
    # upload hello-led
    res = upload(d, GFW, base, 256)
    check(res == ("end", 0), f"upload hello-led 256-byte chunks: {res}")
    check(d.getflash(base, len(GFW)) == GFW, "flash content matches image")
    raw = bytearray(open(R + "/firmware/nuvoton-nuc1xx/hello-led/build/hello-led.bin","rb").read())
    raw += bytes((i * 37) & 0xFF for i in range(3400 - len(raw)))
    BIG = gi.make_image(bytes(raw), 1, 0x00010000)
    res = upload(d, BIG, base, 256)
    check(res == ("end", 0) and d.getflash(base, len(BIG)) == BIG, f"multi-page image ({len(BIG)} B, 7 pages): {res}")
    res = upload(d, BIG, base, 512)
    check(res[0] == "data" and res[2] == 3, f"512-byte chunks exceed max_chunk 256 -> BAD_LEN: {res}")
    res = upload(d, GFW, base, 64, wrongver=0x00020000)
    check(res == ("end", 7), f"FW_BEGIN fw_version != header -> BAD_IMAGE: {res}")
    r = d.raw(gp.build_frame(gp.FW_DATA, 3, gp.u28(0x200) + gp.pack7(bytes(512))))
    check(len(r) == 1 and r[0].payload == bytes([3]) and r[0].seq == 3, "oversized frame -> BAD_LEN reply")
    res = upload(d, GFW, base, 64)
    check(res == ("end", 0), f"re-upload with 64-byte chunks: {res}")
    # corrupted chunk -> BAD_IMAGE
    big = GFW
    res = upload(d, GFW, base, 128, tamper=0x200)
    check(res == ("end", 7), f"tampered data -> BAD_IMAGE: {res}")
    # duplicate FW_DATA: same seq must not reprogram; different seq out of order -> BAD_ADDR
    crc = zlib.crc32(GFW[:0xCC] + b"\0"*4 + GFW[0xD0:]) & 0xFFFFFFFF
    d.req(gp.FW_BEGIN, gp.req_fw_begin(len(GFW), crc, 1, 0x00010000))
    plan = gp.chunk_plan(len(GFW), 256)
    off, n = plan[0]
    f1 = d.req(gp.FW_DATA, gp.req_fw_data(off, GFW[off:off+n]))
    s = d.seq
    f2 = d.req(gp.FW_DATA, gp.req_fw_data(off, GFW[off:off+n]), seq=s)
    check(f1.status == 0 and f2.status == 0 and f2.payload == f1.payload, "same-seq FW_DATA resend -> same reply, no redo")
    f3 = d.req(gp.FW_DATA, gp.req_fw_data(off, GFW[off:off+n]))
    check(f3.status == 5, "new-seq repeat of old offset -> BAD_ADDR")
    # bad checksum then resend with same seq is executed (not treated as duplicate)
    off2, n2 = plan[1]
    fr = bytearray(gp.build_frame(gp.FW_DATA, 77, gp.req_fw_data(off2, GFW[off2:off2+n2]))); fr[-2] ^= 1
    r = d.raw(fr); check(r[0].payload == bytes([1]), "corrupted FW_DATA -> BAD_CHECKSUM")
    f = d.req(gp.FW_DATA, gp.req_fw_data(off2, GFW[off2:off2+n2]), seq=77)
    check(f.status == 0, "resend after BAD_CHECKSUM is executed")
    for off3, n3 in plan[2:]:
        f = d.req(gp.FW_DATA, gp.req_fw_data(off3, GFW[off3:off3+n3])); assert f.status == 0, f
    f = d.req(gp.FW_END); check(f.status == 0, "FW_END OK after resend dance")
    # chunk crossing a page / misaligned / too long
    d.req(gp.FW_BEGIN, gp.req_fw_begin(0x800, 0, 1, 0))
    f = d.req(gp.FW_DATA, gp.u28(0x200) + gp.pack7(b"\0"*6)); check(f.status == 3, "data not multiple of 4 -> BAD_LEN")
    f = d.req(gp.FW_DATA, gp.req_fw_data(0x300, b"\0"*4)); check(f.status == 5, "out of order -> BAD_ADDR")
    f = d.req(gp.FW_DATA, gp.req_fw_data(0x200, b"\0"*260)); check(f.status == 3, "chunk > max_chunk -> BAD_LEN")
    f = d.req(gp.FW_DATA, gp.req_fw_data(0x0, b"\0"*4)); check(f.status == 5, "page 0 before upper pages -> BAD_ADDR")
    f = d.req(gp.FW_END); check(f.status == 4, "FW_END before all data -> BAD_STATE")
    f = d.req(gp.FW_ABORT); check(f.status == 0, "FW_ABORT")
    f = d.req(gp.FW_DATA, gp.req_fw_data(0x200, b"\0"*4)); check(f.status == 4, "FW_DATA after abort -> BAD_STATE")
    # restore a good image
    check(upload(d, GFW, base, 256) == ("end", 0), "good image again")
    bw = ctypes.c_int.in_dll(d.L, "sim_bad_writes").value
    check(bw == 0, f"no flash access outside the target region ({bw})")
    f = d.req(gp.REBOOT, b"\x01"); check(f.status == 0 and d.L.sim_reboot_mode() == 2, "REBOOT(1) accepted")
    f = d.req(gp.REBOOT, b"\x05"); check(f.status == 2, "REBOOT(5) -> BAD_CMD")
    if not test:
        r, mb = d.boot(); check(r == 1, f"valid app -> handoff to APROM (r={r})")
        r, mb = d.boot(buttons=(1<<5)|(1<<6)); check(r == 3, "Pitch combo -> update mode")
        r, mb = d.boot(buttons=(1<<5)); check(r == 1, "only Pitch-down -> handoff")
        r, mb = d.boot(mailbox=0x544F4F42); check(r == 3 and mb == 0, "mailbox BOOT -> update mode, magic cleared")
        r, mb = d.boot(); check(r == 1, "next boot -> app again")
        img = bytearray(d.getflash(0, len(GFW))); img[0x200] ^= 0x10; d.setflash(0, bytes(img))
        r, mb = d.boot(); check(r == 3, "corrupted app -> update mode")
        img[0x200] ^= 0x10; img[4] = 0xFE; d.setflash(0, bytes(img))
        r, mb = d.boot(); check(r == 3, "even reset vector -> update mode")
    else:
        r, mb = d.boot(); check(r == 3, "test build always stays")
        check(d.getflash(0, 0x8000) == b"\xff"*0x8000, "test build never touched APROM")
print("FAILS:", fails)
sys.exit(1 if fails else 0)
