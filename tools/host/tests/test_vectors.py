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

    def test_image(self):
        t = self.v["image"]
        img = gi.make_image(b(t["raw"]), t["fields"]["board_id"], t["fields"]["fw_version"])
        self.assertEqual(img, b(t["gfw"]))
        h = gi.check_image(img)
        self.assertEqual(h.crc32, t["fields"]["crc32"])
        self.assertEqual(img[0xC0:0xE0], b(t["header_hex"]))


if __name__ == "__main__":
    unittest.main()
