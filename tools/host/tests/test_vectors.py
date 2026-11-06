"""protocol/test-vectors.json must agree with the codec, decoded independently of how it was generated."""
import json
import os
import sys
import unittest

HOST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HOST)
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
import make_test_vectors  # noqa: E402

PATH = os.path.join(HOST, "..", "..", "protocol", "test-vectors.json")


def b(s):
    return bytes.fromhex(s)


@unittest.skipUnless(os.path.exists(PATH), "protocol/test-vectors.json not generated")
class TestVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PATH) as f:
            cls.v = json.load(f)

    def test_file_is_current(self):
        self.assertEqual(json.loads(json.dumps(make_test_vectors.build())), self.v,
                         "run tools/host/make_test_vectors.py")

    def test_ints(self):
        for name, enc, dec in (("u14", gp.u14, gp.dec_u14), ("u28", gp.u28, gp.dec_u28), ("u32", gp.u32, gp.dec_u32)):
            for t in self.v[name]:
                self.assertEqual(enc(t["value"]), b(t["bytes"]))
                self.assertEqual(dec(b(t["bytes"])), t["value"])

    def test_pack(self):
        for t in self.v["pack7"]:
            self.assertEqual(gp.pack7(b(t["raw"])), b(t["packed"]))
            self.assertEqual(gp.unpack7(b(t["packed"])), b(t["raw"]))

    def test_checksum(self):
        for t in self.v["checksum"]:
            self.assertEqual(gp.checksum(b(t["body"])), t["chk"])

    def test_frames(self):
        for t in self.v["frames"]:
            f = gp.parse_frame(b(t["frame"]))
            self.assertEqual((f.cmd, f.seq, f.payload), (t["cmd"], t["seq"], b(t["payload"])), t["desc"])
            self.assertEqual(gp.build_frame(t["cmd"], t["seq"], b(t["payload"])), b(t["frame"]))
        for t in self.v["bad_frames"]:
            with self.assertRaisesRegex(gp.ProtocolError, t["error"]):
                gp.parse_frame(b(t["frame"]))

    def test_chunk_plan(self):
        for t in self.v["chunk_plan"]:
            self.assertEqual([list(c) for c in gp.chunk_plan(t["size"], t["max_chunk"])], t["chunks"])

    def test_feedback(self):
        """The feedback section, decoded by hand from sysex.md 4.6 (not with the codec's parsers), then checked
        against the codec too."""
        fb = self.v["feedback"]
        self.assertEqual((fb["cap_feedback"], fb["none_index"]), (64, 0x3FFF))
        self.assertEqual(fb["kinds"], {"button": 1, "keys": 2, "analog": 3, "encoder": 4, "led": 5, "display": 6})

        def u14(p, i):
            return p[i] | p[i + 1] << 7

        def lstr(p, i):
            n = p[i]
            self.assertLessEqual(n, 31)
            return p[i + 1:i + 1 + n].decode("ascii"), i + 1 + n

        def nn(v):
            return None if v == 0x3FFF else v

        def decode(cmd, p):
            if cmd == 0x11:
                return {"kind": p[0], "index": u14(p, 1)}
            if cmd == 0x51:
                if p[0] != 0:
                    self.assertEqual(len(p), 1)
                    return {"status": p[0]}
                f = {"status": 0, "kind": p[1], "index": u14(p, 2), "count": u14(p, 4)}
                i = 6
                for k in ("id", "name", "role"):
                    f[k], i = lstr(p, i)
                k, rest = p[1], p[i:]

                def opt(j):                 # an added field (4.6.1): absent in older firmware
                    return rest[j] if len(rest) > j else None
                extra = {1: lambda: {"led": nn(u14(rest, 0))},
                         2: lambda: {"keys": u14(rest, 0), "first_note": rest[2]},
                         3: lambda: {"takeover": opt(0)},
                         4: lambda: {"ring": rest[0], "center": nn(u14(rest, 1)), "mode": opt(3)},
                         5: lambda: {"colours": rest[0], "owner": opt(1)},
                         6: lambda: {"cols": rest[0], "rows": rest[1], "type": rest[2]}}[k]()
                self.assertIn(len(rest), {1: [2], 2: [3], 3: [0, 1], 4: [3, 4], 5: [1, 2], 6: [3]}[k])
                f.update(extra)
                return f
            if cmd == 0x64:                 # BOOTED: HELLO's fields without the status (4.6.3)
                n = p[27] if len(p) > 27 else 0
                return {"role": p[0], "board_id": u14(p, 1),
                        "fw_version": p[3] | p[4] << 7 | p[5] << 14 | p[6] << 21 | p[7] << 28,
                        "proto_version": p[8], "max_chunk": u14(p, 9), "caps": u14(p, 11),
                        "uid": gp.unpack7(p[13:27]).hex().upper(), "node": p[28:28 + n].decode("ascii") or None}
            if cmd == 0x20:
                self.assertEqual(len(p), 3)
                return {"led": u14(p, 0), "level": p[2]}
            if cmd == 0x21:
                self.assertEqual(len(p), 6)
                return {"led": u14(p, 0), "r": p[2], "g": p[3], "b": p[4], "flags": p[5]}
            if cmd == 0x22:
                self.assertEqual(len(p), 3 + p[2])
                return {"first": u14(p, 0), "n": p[2], "levels": list(p[3:])}
            if cmd == 0x23:
                self.assertEqual(len(p), 5)
                return {"kind": p[0], "index": u14(p, 1), "value": u14(p, 3)}
            if cmd == 0x24:
                self.assertEqual(len(p), 6)
                return {"encoder": u14(p, 0), "mode": p[2], "value": u14(p, 3), "flags": p[5]}
            if cmd == 0x25:
                return {"display": u14(p, 0), "row": p[2], "col": p[3], "text": p[4:].decode("ascii")}
            if cmd == 0x26:
                self.assertEqual(len(p), 1)
                return {"what": p[0]}
            self.fail(f"unexpected cmd 0x{cmd:02x}")

        seen = set()
        for t in fb["frames"]:
            f = gp.parse_frame(b(t["frame"]))
            p = b(t["payload"])
            self.assertEqual((f.cmd, f.seq, f.payload), (t["cmd"], t["seq"], p), t["desc"])
            self.assertEqual(decode(t["cmd"], p), t["fields"], t["desc"])
            seen.add(t["cmd"])
            if t["cmd"] == gp.GET_COMPONENT | gp.REPLY and p[0] == gp.OK:
                c = gp.parse_component(p)
                self.assertEqual(gp.build_component_reply(c), p, t["desc"])
                self.assertTrue(gp.reply_matches(f, gp.GET_COMPONENT, t["seq"]))
                seen.add(("kind", c.kind))
            elif gp.is_notification(t["cmd"]):
                self.assertTrue(f.is_notification)
                gp.parse_notification(t["cmd"], p)
            elif t["cmd"] == gp.BOOTED:
                h = gp.parse_booted(p)
                self.assertEqual((h.board_id, h.caps, h.node), (t["fields"]["board_id"], t["fields"]["caps"],
                                                                t["fields"]["node"]))
                self.assertEqual(gp.build_booted(h), p)
        self.assertTrue({0x11, 0x51, 0x64, *range(0x20, 0x27)} <= seen)
        self.assertTrue({("kind", k) for k in range(1, 7)} <= seen)

    def test_layout(self):
        """The layout section, decoded by hand from sysex.md 4.7, then with the codec."""
        lv = self.v["layout"]
        self.assertEqual(lv["cap_layout"], 1 << 8)
        blob = b(lv["layout"]["hex"])
        self.assertEqual((blob[0], blob[1], blob[2], blob[3] | blob[4] << 8),
                         (1, lv["layout"]["width"], lv["layout"]["height"], len(lv["layout"]["items"])))
        pos, items = 5, []
        while pos < len(blob):
            n = blob[pos]
            body = blob[pos + 1:pos + 1 + n]
            j = dict(zip(("kind", "index", "shape", "x", "y", "w", "h"), body[:7]))
            known = 7
            if j["shape"] == 8:
                j.update(n_keys=body[7], first_note=body[8])
                known = 9
            elif j["shape"] == 9:
                j["y_index"] = body[7]
                known = 8
            if body[known:]:
                j["extra"] = body[known:].hex(" ").upper()
            items.append(j)
            pos += 1 + n
        self.assertEqual(items, lv["layout"]["items"])
        lay = gp.parse_layout(blob)
        self.assertEqual(gp.build_layout(lay), blob)
        keys = next(it for it in lay.items if it.shape == gp.SHAPE_KEYS)
        self.assertEqual([[k, bl, round(x, 4), y, round(w, 4), round(h, 4)] for k, bl, x, y, w, h in gp.key_rects(keys)],
                         lv["keys"])
        white = [k for k in lv["keys"] if not k[1]]
        self.assertEqual(len(white), 15)                  # 25 keys from C: two octaves and a C
        self.assertAlmostEqual(sum(k[4] for k in white), keys.w)
        for t in lv["frames"]:
            f = gp.parse_frame(b(t["frame"]))
            p = f.payload
            if t["cmd"] == 0x14:
                self.assertEqual((p[0] | p[1] << 7 | p[2] << 14 | p[3] << 21, p[4] | p[5] << 7),
                                 (t["fields"]["offset"], t["fields"]["max"]))
            elif p[0] == 0:
                total, off, data = gp.parse_layout_reply(p)
                self.assertEqual((total, off, data.hex(" ").upper()),
                                 (t["fields"]["total"], t["fields"]["offset"], t["fields"]["data"]))

    def test_monitor(self):
        """The monitor section, decoded by hand from sysex.md 4.8, then with the codec."""
        mv = self.v["monitor"]
        self.assertEqual((mv["cap_monitor"], mv["no_reading"]), (1 << 9, 0x3FFF))

        def bits(p, n):
            return [(p[i // 7] >> (i % 7)) & 1 for i in range(n)]

        seen = set()
        for t in mv["frames"]:
            f = gp.parse_frame(b(t["frame"]))
            p, want = f.payload, t["fields"]
            seen.add(t["cmd"])
            if t["cmd"] == 0x15:
                self.assertEqual(len(p), 1)
                self.assertEqual(p[0], want["kinds"])
            elif t["cmd"] == 0x55:
                self.assertEqual(len(p), 6)
                self.assertEqual((p[0], p[1], p[2] | p[3] << 7, p[4] | p[5] << 7),
                                 (want["status"], want["kinds"], want["interval_ms"], want["timeout_ms"]))
                self.assertEqual(gp.parse_monitor_reply(p), (want["kinds"], want["interval_ms"], want["timeout_ms"]))
            else:
                self.assertEqual(t["cmd"], 0x65)
                m = gp.parse_raw(p)
                self.assertEqual(gp.build_raw(m), p)
                if p[0] == 2:
                    self.assertEqual((p[1], p[2], p[3]), (want["row"], want["rows"], want["cols"]))
                    self.assertEqual(bits(p[4:], p[2] * p[3]), want["values"])
                    self.assertEqual(len(p), 4 + (p[2] * p[3] + 6) // 7)
                else:
                    first, n = p[1] | p[2] << 7, p[3]
                    vals = bits(p[4:], n) if p[0] == 1 else [p[4 + 2 * i] | p[5 + 2 * i] << 7 for i in range(n)]
                    self.assertEqual((p[0], first, vals), (want["kind"], want["first"], want["values"]))
                    self.assertEqual(m.values, vals)
                seen.add(("raw", p[0]))
        self.assertTrue({0x15, 0x55, 0x65, *(("raw", k) for k in (1, 2, 3, 4, 7))} <= seen)

    def test_image(self):
        t = self.v["image"]
        img = gi.make_image(b(t["raw"]), t["fields"]["board_id"], t["fields"]["fw_version"])
        self.assertEqual(img, b(t["gfw"]))
        h = gi.check_image(img)
        self.assertEqual(h.crc32, t["fields"]["crc32"])
        self.assertEqual(img[0xC0:0xE0], b(t["header_hex"]))


if __name__ == "__main__":
    unittest.main()
