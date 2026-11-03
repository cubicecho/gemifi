import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gemifi_proto as gp  # noqa: E402


class TestInts(unittest.TestCase):
    def test_u14(self):
        self.assertEqual(gp.u14(0x1234), bytes([0x34, 0x24]))
        self.assertEqual(gp.u14(0x3FFF), bytes([0x7F, 0x7F]))
        for v in (0, 1, 127, 128, 0x3FFF):
            self.assertEqual(gp.dec_u14(gp.u14(v)), v)
        with self.assertRaises(ValueError):
            gp.u14(0x4000)

    def test_u28(self):
        self.assertEqual(gp.u28(0x200), bytes([0x00, 0x04, 0x00, 0x00]))
        self.assertEqual(gp.u28(0xFFFFFFF), bytes([0x7F] * 4))
        for v in (0, 0x200, 0x7E00, 0xFFFFFFF):
            self.assertEqual(gp.dec_u28(gp.u28(v)), v)
        with self.assertRaises(ValueError):
            gp.u28(1 << 28)

    def test_u32(self):
        # 5 bytes, LS 7 bits first, last byte carries the top 4 bits
        self.assertEqual(gp.u32(0xFFFFFFFF), bytes([0x7F, 0x7F, 0x7F, 0x7F, 0x0F]))
        self.assertEqual(gp.u32(0x49464D47), bytes([0x47, 0x1A, 0x19, 0x4A, 0x04]))
        for v in (0, 0x80, 0xDEADBEEF, 0xFFFFFFFF):
            self.assertEqual(gp.dec_u32(gp.u32(v)), v)
        with self.assertRaises(gp.ProtocolError):
            gp.dec_u32(bytes([0, 0, 0, 0, 0x10]))
        with self.assertRaises(gp.ProtocolError):
            gp.dec_u14(bytes([0x80, 0]))


class TestPack(unittest.TestCase):
    def test_known(self):
        self.assertEqual(gp.pack7(b""), b"")
        self.assertEqual(gp.pack7(b"\x80"), bytes([0x01, 0x00]))
        self.assertEqual(gp.pack7(bytes([0xFF] * 7)), bytes([0x7F] + [0x7F] * 7))
        # 8 bytes: one full group (8 out) + one group of 1 (2 out)
        self.assertEqual(gp.pack7(bytes(range(0x7E, 0x86))),
                         bytes([0x7C, 0x7E, 0x7F, 0x00, 0x01, 0x02, 0x03, 0x04, 0x01, 0x05]))

    def test_round_trip(self):
        rnd = random.Random(1)
        for n in list(range(0, 40)) + [256, 512]:
            data = bytes(rnd.randrange(256) for _ in range(n))
            p = gp.pack7(data)
            self.assertEqual(len(p), gp.packed_len(n))
            self.assertTrue(all(b < 0x80 for b in p))
            self.assertEqual(gp.unpacked_len(len(p)), n)
            self.assertEqual(gp.unpack7(p), data)

    def test_uid_is_14(self):
        self.assertEqual(len(gp.pack7(bytes(12))), 14)

    def test_bad(self):
        with self.assertRaises(gp.ProtocolError):
            gp.unpack7(bytes([0, 1, 2, 3, 4, 5, 6, 7, 0]))   # lone MSB byte at the end
        with self.assertRaises(gp.ProtocolError):
            gp.unpack7(bytes([0, 0x80]))


class TestFrame(unittest.TestCase):
    def test_checksum(self):
        self.assertEqual(gp.checksum(bytes([1, 0x10, 5, 1, 0, 1])), 0x18)
        self.assertEqual(gp.checksum(bytes([0x7F] * 10)), (0x7F * 10) & 0x7F)

    def test_build_hello(self):
        self.assertEqual(gp.build_frame(gp.HELLO, 0),
                         bytes([0xF0, 0x7D, 0x67, 0x6D, 1, 1, 0, 0, 0, 2, 0xF7]))

    def test_build_reboot(self):
        self.assertEqual(gp.build_frame(gp.REBOOT, 5, gp.req_reboot(1)),
                         bytes([0xF0, 0x7D, 0x67, 0x6D, 1, 0x10, 5, 1, 0, 1, 0x18, 0xF7]))

    def test_round_trip(self):
        payload = gp.req_fw_data(0x200, bytes(range(0x80, 0x80 + 64)))
        f = gp.parse_frame(gp.build_frame(gp.FW_DATA, 99, payload))
        self.assertEqual((f.ver, f.cmd, f.seq, f.payload), (1, gp.FW_DATA, 99, payload))
        self.assertEqual(gp.dec_u28(f.payload[:4]), 0x200)
        self.assertEqual(gp.unpack7(f.payload[4:]), bytes(range(0x80, 0xC0)))

    def test_long_payload_length(self):
        payload = bytes(300)
        msg = gp.build_frame(gp.FW_DATA, 1, payload)
        self.assertEqual(msg[7:9], bytes([300 & 0x7F, 300 >> 7]))
        self.assertEqual(gp.parse_frame(msg).payload, payload)

    def test_parse_errors(self):
        good = bytearray(gp.build_frame(gp.REBOOT, 5, b"\x01"))
        bad = bytearray(good); bad[-2] ^= 1
        with self.assertRaisesRegex(gp.ProtocolError, "checksum"):
            gp.parse_frame(bad)
        bad = bytearray(good); bad[7] = 2
        with self.assertRaisesRegex(gp.ProtocolError, "length"):
            gp.parse_frame(bad)
        with self.assertRaises(gp.ProtocolError):
            gp.parse_frame(gp.IDENTITY_REQUEST)
        with self.assertRaises(ValueError):
            gp.build_frame(gp.HELLO, 0, b"\x80")

    def test_command_numbers(self):
        # sysex.md 1/4 after the 2026-10-30 renumbering: requests 0x00-0x1F, replies 0x40-0x5F
        self.assertEqual((gp.HELLO, gp.REBOOT, gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT),
                         (0x01, 0x10, 0x18, 0x19, 0x1A, 0x1B))
        for c in (gp.HELLO, gp.REBOOT, gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT):
            self.assertLess(c, 0x20)
            self.assertLess(c | gp.REPLY, gp.LOG)

    def test_reply_matching(self):
        f = gp.parse_frame(gp.build_frame(gp.FW_DATA | gp.REPLY, 3, b"\x00" + gp.u28(0)))
        self.assertEqual(f.cmd, 0x59)
        self.assertTrue(f.is_reply)
        self.assertFalse(f.is_unsolicited)
        self.assertEqual(f.status, gp.OK)
        self.assertTrue(gp.reply_matches(f, gp.FW_DATA, 3))
        self.assertFalse(gp.reply_matches(f, gp.FW_DATA, 4))
        self.assertFalse(gp.reply_matches(f, gp.FW_BEGIN, 3))
        ev = gp.parse_frame(gp.build_frame(gp.EVENT, 3, gp.build_event(2, 1, 2, 3)))
        self.assertTrue(ev.is_unsolicited)
        self.assertFalse(gp.reply_matches(ev, gp.FW_DATA, 3))
        short = gp.parse_frame(gp.build_frame(gp.FW_BEGIN | gp.REPLY, 3, b"\x00"))   # OK without max_chunk
        self.assertFalse(gp.reply_matches(short, gp.FW_BEGIN, 3))
        long_err = gp.parse_frame(gp.build_frame(gp.FW_BEGIN | gp.REPLY, 3, b"\x05\x00"))  # error + extra
        self.assertFalse(gp.reply_matches(long_err, gp.FW_BEGIN, 3))
        err = gp.parse_frame(gp.build_frame(gp.FW_BEGIN | gp.REPLY, 3, bytes([gp.BAD_BOARD])))
        self.assertTrue(gp.reply_matches(err, gp.FW_BEGIN, 3))


class TestMessages(unittest.TestCase):
    def test_hello(self):
        h = gp.Hello(1, 1, 0x01020003, 1, 256, 1, bytes(range(12)))
        p = gp.build_hello_reply(h)
        self.assertEqual(len(p), gp.HELLO_REPLY_LEN)
        self.assertEqual(gp.parse_hello(p), h)
        self.assertIsNone(gp.parse_hello(p).node)

    def test_hello_node_name(self):
        h = gp.Hello(0, 1, 0x01020003, 1, 0, 0x2E, bytes(range(12)), "keystation-mini32")
        p = gp.build_hello_reply(h)
        self.assertEqual(len(p), gp.HELLO_REPLY_LEN + 1 + 17)
        self.assertEqual(gp.parse_hello(p), h)
        self.assertEqual(gp.parse_hello(p + b"\x05\x06").node, "keystation-mini32", "later fields are ignored")
        base = p[:gp.HELLO_REPLY_LEN]
        for bad in (b"\x00", b"\x05abc", b"\x03A-b", b"\x04-ab-"):
            self.assertIsNone(gp.parse_hello(base + bad).node, bad)
        with self.assertRaises(gp.ProtocolError):
            gp.parse_hello(base[:-1])

    def test_fw_begin(self):
        p = gp.req_fw_begin(0x100, 0x89ABCDEF, 1, 0x01020003)
        self.assertEqual(len(p), 17)
        self.assertEqual(gp.dec_u32(p[5:10]), 0x89ABCDEF)

    def test_log_event(self):
        m = gp.parse_log(gp.build_log(2, 1234, "oct- down"))
        self.assertEqual((m.level, m.t_ms, m.text), (2, 1234, "oct- down"))
        self.assertIn("INFO", str(m))
        e = gp.parse_event(gp.build_event(2, 12, 100, 70000))
        self.assertEqual((e.type, e.index, e.value, e.t_ms), (2, 12, 100, 70000))

    def test_assembler(self):
        a = gp.SysexAssembler()
        msg = gp.build_frame(gp.HELLO, 1)
        self.assertEqual(a.feed(b"\x90\x3c\x40" + msg[:5]), [])
        self.assertEqual(a.feed(b"\xf8" + msg[5:] + b"\x80\x3c\x00"), [msg])
        self.assertEqual(a.feed(msg[:4] + b"\x90" + msg), [msg])   # status byte aborts the partial one


class TestChunkPlan(unittest.TestCase):
    def check(self, size, mc):
        plan = gp.chunk_plan(size, mc)
        covered = []
        for off, n in plan:
            self.assertEqual(n % 4, 0)
            self.assertLessEqual(n, mc)
            self.assertEqual(off // 512, (off + n - 1) // 512, f"chunk {off:#x}+{n} crosses a page")
            covered += range(off, off + n)
        self.assertEqual(sorted(covered), list(range(size)))
        page0 = [i for i, (o, _) in enumerate(plan) if o < 512]
        self.assertEqual(page0, list(range(len(plan) - len(page0), len(plan))), "page 0 must be last")
        rest = [o for o, _ in plan if o >= 512]
        self.assertEqual(rest, sorted(rest))
        if size > 512:
            self.assertEqual(plan[0][0], 0x200)
        return plan

    def test_plans(self):
        for size in (4, 0x100, 0x200, 0x204, 0x500, 0x620, 0x7FFC, 0x8000):
            for mc in (4, 8, 64, 128, 256, 512):
                self.check(size, mc)

    def test_example(self):
        self.assertEqual(gp.chunk_plan(0x500, 256),
                         [(0x200, 256), (0x300, 256), (0x400, 256), (0x000, 256), (0x100, 256)])

    def test_bad(self):
        for size in (0, 3, 0x8004):
            with self.assertRaises(ValueError):
                gp.chunk_plan(size, 256)
        for mc in (0, 3, 60, 100, 200, 1000, 1024):    # must be a multiple of 4 dividing 512
            with self.assertRaises(ValueError):
                gp.chunk_plan(0x100, mc)
        self.assertEqual([m for m in range(1025) if gp.valid_max_chunk(m)], [4, 8, 16, 32, 64, 128, 256, 512])


if __name__ == "__main__":
    unittest.main()
