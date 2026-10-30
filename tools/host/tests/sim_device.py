"""A simulated gemifi bootloader/app that follows protocol/sysex.md, for host-tool tests.
It enforces the upload rules strictly (order, page 0 last, page boundaries, no double programming)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402

PAGE = gp.PAGE_SIZE


class SimDevice:
    def __init__(self, role=gp.ROLE_BOOT, board_id=1, max_chunk=256, drop_replies=(), drop_requests=(),
                 busy_on=()):
        self.role, self.board_id, self.max_chunk = role, board_id, max_chunk
        self.flash = bytearray(b"\xff" * gp.APROM_SIZE)
        self.program_count = [0] * (gp.APROM_SIZE // 4)
        self.state = "idle"
        self.last = None          # (frame bytes, reply bytes) of the last completed request
        self.n_req = 0
        self.n_reply = 0
        self.drop_replies, self.drop_requests, self.busy_on = set(drop_replies), set(drop_requests), set(busy_on)
        self.log = []
        self.reboot_to = None

    # -- transport side --
    def handle(self, msg):
        """Returns reply bytes or None."""
        self.n_req += 1
        if self.n_req in self.drop_requests:
            return None
        try:
            f = gp.parse_frame(msg)
        except gp.ProtocolError:
            return None
        if self.last and self.last[0] == bytes(msg):
            reply = self.last[1]                     # duplicate seq: reply again, don't redo the work
        else:
            if self.n_req in self.busy_on:
                return gp.build_frame(f.cmd | gp.REPLY, f.seq, bytes([gp.BUSY]))
            payload = self.dispatch(f)
            reply = gp.build_frame(f.cmd | gp.REPLY, f.seq, payload)
            self.last = (bytes(msg), reply)
        self.n_reply += 1
        if self.n_reply in self.drop_replies:
            return None
        return reply

    # -- protocol --
    def dispatch(self, f):
        self.log.append(gp.CMD_NAMES.get(f.cmd, hex(f.cmd)))
        p = f.payload
        if f.cmd == gp.HELLO:
            h = gp.Hello(self.role, self.board_id, 0x00010000, 1,
                         self.max_chunk if self.role == gp.ROLE_BOOT else 0,
                         gp.CAP_FW if self.role == gp.ROLE_BOOT else gp.CAP_LOG | gp.CAP_EVENT, bytes(range(12)))
            return gp.build_hello_reply(h)
        if f.cmd == gp.REBOOT:
            self.reboot_to = p[0]
            return bytes([gp.OK])
        if f.cmd in (gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT) and self.role != gp.ROLE_BOOT:
            return bytes([gp.BAD_STATE])
        if f.cmd == gp.FW_BEGIN:
            if len(p) != 17:
                return bytes([gp.BAD_LEN])
            size, crc, board, ver = gp.dec_u32(p[0:5]), gp.dec_u32(p[5:10]), gp.dec_u14(p[10:12]), gp.dec_u32(p[12:17])
            if board != self.board_id:
                return bytes([gp.BAD_BOARD])
            if size == 0 or size % 4 or size > gp.APROM_SIZE:
                return bytes([gp.BAD_ADDR])
            self.size, self.crc, self.ver = size, crc, ver
            self.erase(0)
            self.expect = PAGE if size > PAGE else 0
            self.state = "loading"
            return bytes([gp.OK]) + gp.u14(self.max_chunk)
        if f.cmd == gp.FW_DATA:
            if self.state != "loading":
                return bytes([gp.BAD_STATE])
            off = gp.dec_u28(p[0:4])
            data = gp.unpack7(p[4:])
            n = len(data)
            if n == 0 or n % 4 or n > self.max_chunk:
                return bytes([gp.BAD_LEN])
            if off != self.expect or off + n > self.size or off // PAGE != (off + n - 1) // PAGE:
                return bytes([gp.BAD_ADDR])
            if off % PAGE == 0 and off != 0:
                self.erase(off)
            for i in range(0, n, 4):
                w = (off + i) // 4
                if self.flash[off + i:off + i + 4] != b"\xff\xff\xff\xff":
                    return bytes([gp.FLASH_ERR])
                self.program_count[w] += 1
            self.flash[off:off + n] = data
            nxt = off + n
            if self.expect >= PAGE:
                self.expect = nxt if nxt < self.size else 0
            else:
                self.expect = nxt if nxt < min(PAGE, self.size) else None
            return bytes([gp.OK]) + gp.u28(off)
        if f.cmd == gp.FW_END:
            if self.state != "loading" or self.expect is not None:
                return bytes([gp.BAD_STATE])
            img = bytes(self.flash[:self.size])
            self.state = "idle"
            try:
                h = gi.check_image(img, self.board_id)
            except gi.ImageError:
                return bytes([gp.BAD_IMAGE])
            if h.crc32 != self.crc or gi.image_crc(img) != self.crc:
                return bytes([gp.BAD_IMAGE])
            return bytes([gp.OK])
        if f.cmd == gp.FW_ABORT:
            self.state = "idle"
            return bytes([gp.OK])
        return bytes([gp.BAD_CMD])

    def erase(self, off):
        self.flash[off:off + PAGE] = b"\xff" * PAGE


class SimPort:
    def __init__(self, dev, unsolicited=()):
        self.dev = dev
        self.q = list(unsolicited)
        self.sent = []

    def send(self, msg):
        self.sent.append(bytes(msg))
        r = self.dev.handle(bytes(msg))
        if r is not None:
            self.q.append(r)

    def recv(self, timeout):
        out, self.q = self.q, []
        return out

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *e):
        pass

