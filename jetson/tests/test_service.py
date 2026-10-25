"""The Jetson side without hardware: doubles settled by bid, replies streamed to the right tile,
commands repeated until acknowledged.   python -m unittest discover -s tests"""
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from dust import proto  # noqa: E402
from dust.tts import resample  # noqa: E402
from dust.voice import Conversations  # noqa: E402
from dust.wall import Wall  # noqa: E402


class FakeBridge:
    mac = b"\x24\x0a\xc4\x12\x34\x56"
    alive = True

    def __init__(self):
        self.sent = []
        self.lock = threading.Lock()

    def send(self, data, mac=b"\xff" * 6):
        with self.lock:
            self.sent.append((bytes(mac), bytes(data)))

    def packets(self, typ):
        with self.lock:
            return [(m, d) for m, d in self.sent if proto.head(d) and proto.head(d)[1] == typ]


class FakeWall:
    def __init__(self):
        self.sent = []

    def send_to(self, uid, data):
        self.sent.append((uid, data))

    def voices(self):
        out = []
        for uid, d in self.sent:
            if proto.head(d)[1] == proto.PKT_VOICE:
                f = proto.VOICE.unpack(d)
                out.append((uid, f[6], f[7], f[8]))   # uid, utt, op, arg
        return out


def audio_up(uid, utt, seq, score, end=False, samples=None):
    e = proto.Adpcm()
    pred, index = e.pred, e.index
    payload = e.encode(samples or [((i * 37) % 2000) - 1000 for i in range(proto.AUDIO_FRAME)])
    flags = (proto.AUDIO_START if seq == 0 else 0) | (proto.AUDIO_END if end else 0)
    return proto.AUDIO.pack(proto.MAGIC, proto.PROTO, proto.PKT_AUDIO, 4, 0, uid, utt, seq, flags, 160, int(score * 10), pred, index, payload)


class Stt:
    def __init__(self, text):
        self.text = text

    def transcribe(self, pcm):
        self.n = len(pcm)
        return self.text


class Brain:
    def reply(self, heard):
        return "Hello there."


class Voice:
    last_engine = "fake"

    def stream(self, text):
        yield bytes(640 * 10)   # 200 ms of silence


class Settling(unittest.TestCase):
    def test_two_tiles_streaming_the_same_words(self):
        wall = FakeWall()
        conv = Conversations(wall, Stt("make a spiral"), Brain(), Voice())
        conv.on_packet(None, 0, audio_up(0x10, 1, 0, 14.0))
        conv.on_packet(None, 0, audio_up(0x20, 5, 0, 21.0))   # a closer tile, a moment later
        self.assertIn((0x10, 1, proto.VOICE_YIELD, 0), wall.voices())
        self.assertNotIn((0x20, 5, proto.VOICE_YIELD, 0), wall.voices())
        for seq in range(1, 30):
            conv.on_packet(None, 0, audio_up(0x10, 1, seq, 14.0))
            conv.on_packet(None, 0, audio_up(0x20, 5, seq, 21.0, end=seq == 29))
        deadline = time.time() + 5
        while time.time() < deadline and not any(v[2] == proto.VOICE_REPLY_END for v in wall.voices()):
            time.sleep(0.02)
        ops = [(u, op) for u, utt, op, arg in wall.voices()]
        self.assertIn((0x20, proto.VOICE_THINKING), ops)
        self.assertIn((0x20, proto.VOICE_REPLY_END), ops)
        self.assertFalse(any(u == 0x10 and op != proto.VOICE_YIELD for u, op in ops), "the farther tile got a reply")
        down = [proto.parse_audio(d) for u, d in wall.sent if proto.head(d)[1] == proto.PKT_AUDIO]
        self.assertEqual(len(down), 10)
        self.assertTrue(all(a.uid == 0x20 and a.utt == 5 and a.flags & proto.AUDIO_DOWN for a in down))
        self.assertEqual([a.seq for a in down], list(range(10)))

    def test_nothing_said(self):
        wall = FakeWall()
        conv = Conversations(wall, Stt(""), Brain(), Voice())
        for seq in range(30):
            conv.on_packet(None, 0, audio_up(0x30, 2, seq, 10.0, end=seq == 29))
        deadline = time.time() + 3
        while time.time() < deadline and not any(v[2] == proto.VOICE_FAIL for v in wall.voices()):
            time.sleep(0.02)
        self.assertIn((0x30, 2, proto.VOICE_FAIL, proto.CLIP_UNKNOWN), wall.voices())


class Commands(unittest.TestCase):
    def test_repeated_until_acknowledged(self):
        bridge = FakeBridge()
        wall = Wall(bridge)
        hello = proto.HELLO.pack(proto.MAGIC, proto.PROTO, proto.PKT_HELLO, 0, 0, 0x55, 0, 0, 1, 300, 100, 255, -128, 0, 0, 1, 0, 0, 0)
        wall.on_packet(b"\x01" * 6, -50, hello)   # a leader on the wall
        wall.form("spiral")
        time.sleep(0.3)
        cmds = bridge.packets(proto.PKT_CMD)
        self.assertGreaterEqual(len(cmds), 3, "not repeated while unacknowledged")
        seq = proto.CMD.unpack(cmds[0][1])[6]
        wall.on_packet(b"\x01" * 6, -50, proto.ACK.pack(proto.MAGIC, proto.PROTO, proto.PKT_ACK, 0, 0, wall.uid, seq, 1))
        n = len(bridge.packets(proto.PKT_CMD))
        time.sleep(0.3)
        self.assertEqual(len(bridge.packets(proto.PKT_CMD)), n, "still repeating after the ack")
        self.assertTrue(bridge.packets(proto.PKT_BEACON), "no beacon")
        wall.close()


class Resampling(unittest.TestCase):
    def test_chunks_join_smoothly(self):
        ramp = list(range(0, 22050 * 2, 2))   # one second at 22.05 kHz
        whole, _ = resample(ramp, 22050, 16000)
        parts, pos, tail = [], 0.0, []
        for i in range(0, len(ramp), 2205):
            samples = tail + ramp[i:i + 2205]
            out, pos = resample(samples, 22050, 16000, pos)
            tail = samples[-1:]
            parts += out
        self.assertEqual(len(parts), len(whole))
        self.assertLessEqual(max(abs(a - b) for a, b in zip(parts, whole)), 1)


if __name__ == "__main__":
    unittest.main()
