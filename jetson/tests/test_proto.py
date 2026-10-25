"""The Jetson's copy of the protocol must match the tiles' byte for byte. host/net_test.c checks
the same sizes and the same encoded bytes on the C side.   python -m unittest discover -s tests"""
import math
import os
import random
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from dust import proto  # noqa: E402
from dust.link import LF_TX, cobs_decode, cobs_encode, decode_frame, encode_frame  # noqa: E402


class Packets(unittest.TestCase):
    def test_sizes_match_the_c_structs(self):
        self.assertEqual(proto.SIZES, {"head": 8, "hello": 33, "cmd": 36, "ack": 18, "claim": 17, "audio": 183, "beacon": 14, "voice": 16})

    def test_hello_round_trip(self):
        raw = proto.HELLO.pack(proto.MAGIC, proto.PROTO, proto.PKT_HELLO, 4, proto.HEAD_SESSION, 0xA1B2C3D4, 0x1F, 1, 7,
                               235, 180, 87, 41, (1 << 2) | (1 << 7), 1, 1, 183, 52, 215)
        h = proto.parse_hello(raw)
        self.assertEqual((h.slot, h.uid, h.epoch, h.fps, h.battery, h.temp_c), (4, 0xA1B2C3D4, 7, 23.5, 87, 41))
        self.assertEqual(h.errors(), ["mic", "overheating"])
        self.assertTrue(h.docked)
        self.assertAlmostEqual(h.session_db, 18.3)

    def test_cmd_layout(self):
        pkt = proto.make_cmd(0x01020304, 513, proto.CMD_FX, a=proto.FX["gather"], x=0.5, y=0.25, strength=1, radius=0.3, seconds=4)
        self.assertEqual(len(pkt), 36)
        f = proto.CMD.unpack(pkt)
        self.assertEqual(f[5:9], (0x01020304, 513, proto.CMD_F_ACK, proto.CMD_FX))
        self.assertEqual(f[10:13], (b"\0" * 10, 8192, 4096))
        self.assertEqual(f[-1], 40)

    def test_audio_end_frames_without_payload(self):
        full = proto.AUDIO.pack(proto.MAGIC, proto.PROTO, proto.PKT_AUDIO, 2, 0, 77, 3, 41, proto.AUDIO_END, 0, 155, 0, 0, b"")
        a = proto.parse_audio(full[:proto.AUDIO.size - 160])
        self.assertEqual((a.uid, a.utt, a.seq, a.flags, a.nbytes, a.data), (77, 3, 41, proto.AUDIO_END, 0, b""))
        self.assertAlmostEqual(a.score_db, 15.5)


class Codec(unittest.TestCase):
    def signal(self):
        # the same voice-like test signal as host/net_test.c
        out = []
        for i in range(16000):
            t = i / 16000.0
            env = 0.5 + 0.5 * math.sin(6.283185307 * 3 * t)
            x = math.sin(6.283185307 * 180 * t) + 0.5 * math.sin(6.283185307 * 360 * t) + 0.25 * math.sin(6.283185307 * 1250 * t)
            out.append(int(round(9000 * env * x / 1.75)))
        return out

    def test_same_bytes_as_the_firmware(self):
        enc = proto.Adpcm().encode(self.signal())
        self.assertEqual(zlib.crc32(enc), 0x7C0D394D, "ADPCM differs from main/dust_adpcm.c")

    def test_round_trip_quality(self):
        pcm = self.signal()
        out = proto.Adpcm().decode(proto.Adpcm().encode(pcm))
        sig = sum(v * v for v in pcm)
        err = sum((a - b) ** 2 for a, b in zip(pcm, out))
        self.assertGreater(10 * math.log10(sig / (err + 1)), 20)

    def test_frame_decodes_from_its_own_state(self):
        pcm = self.signal()[:640]
        e = proto.Adpcm()
        first = e.encode(pcm[:320])
        state = (e.pred, e.index)
        second = e.encode(pcm[320:])
        whole = proto.Adpcm().decode(first + second)
        self.assertEqual(proto.Adpcm(*state).decode(second), whole[320:])


class Link(unittest.TestCase):
    def test_same_frame_as_the_bridge(self):
        frame = encode_frame(LF_TX, b"\xff" * 6, 0, bytes([1, 0, 2, 0, 0]))
        self.assertEqual(zlib.crc32(frame), 0x876AAB18, "framing differs from bridge/main/link_frame.c")

    def test_cobs_round_trips(self):
        rnd = random.Random(1)
        for n in list(range(0, 300)) + [254, 255, 508]:
            for fill in ("zeros", "ff", "random"):
                data = bytes(n) if fill == "zeros" else b"\xff" * n if fill == "ff" else bytes(rnd.randrange(256) for _ in range(n))
                enc = cobs_encode(data)
                self.assertNotIn(0, enc)
                self.assertEqual(cobs_decode(enc), data)

    def test_damaged_frames_are_rejected(self):
        frame = bytearray(encode_frame(LF_TX, b"\x01\x02\x03\x04\x05\x06", -40, b"hello dust"))
        self.assertEqual(decode_frame(bytes(frame[:-1]))[3], b"hello dust")
        frame[5] ^= 0x20
        self.assertIsNone(decode_frame(bytes(frame[:-1])))


if __name__ == "__main__":
    unittest.main()
