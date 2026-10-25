"""The wall's packets (main/mosaic_net.h) and the voice codec (main/dust_adpcm.c), in Python.

Every layout here must match the C structs byte for byte; host/net_test.c and
jetson/tests/test_proto.py check the same sizes and the same encoded bytes."""
import struct
from dataclasses import dataclass

MAGIC = 0x54535544          # "DUST"
PROTO = 6
SLOT_UNKNOWN = 255

PKT_HELLO, PKT_SYNC, PKT_CMD, PKT_LAYOUT, PKT_ACK, PKT_CLAIM, PKT_AUDIO, PKT_BEACON, PKT_VOICE = range(1, 10)
HEAD_LIFTED, HEAD_SETUP, HEAD_SPEAKING, HEAD_SESSION = 1, 2, 4, 8

CMD_STATE, CMD_TALK, CMD_FORM, CMD_FX, CMD_SCATTER, CMD_CALM, CMD_CAL_MIC = range(1, 8)
CMD_F_ACK = 1
CLAIM_TAKING, CLAIM_DOCKED = 1, 2

AUDIO_RATE = 16000
AUDIO_FRAME = 320           # samples per packet (20 ms)
AUDIO_START, AUDIO_END, AUDIO_DOWN = 1, 2, 4

BRAIN_STT, BRAIN_LLM, BRAIN_TTS_CLOUD, BRAIN_TTS_LOCAL = 1, 2, 4, 8
VOICE_YIELD, VOICE_THINKING, VOICE_REPLY_START, VOICE_REPLY_END, VOICE_FAIL = range(1, 6)
# recorded lines on the tiles (main/dust_voice.h), for VOICE_FAIL
CLIP_YES, CLIP_OK, CLIP_UNKNOWN, CLIP_OFFLINE, CLIP_NO_REPLY, CLIP_LOW_BATT = range(6)

STATES = {"idle": 0, "listening": 1, "thinking": 2, "speaking": 3}
FORMS = {"body": 0, "orb": 1, "ring": 2, "wave": 3, "spiral": 4, "cloud": 5, "text": 6}
FX = {"flow": 1, "swirl": 2, "gather": 3, "pulse": 4}
TEXT_MAX = 10
UV_Q = 16384

ERRORS = ["duplicate slot", "firmware mismatch", "mic", "speaker", "magnetometers", "motion sensor",
          "commands not acknowledged", "overheating", "battery low", "crashed and restarted",
          "no recorded voice lines", "no speech models"]

HEAD = struct.Struct("<IBBBB")
HELLO = struct.Struct("<IBBBBIBBIHHBbHBBhBh")
CMD = struct.Struct("<IBBBBIHBBB10shhbbBBB")
ACK = struct.Struct("<IBBBBIHI")
CLAIM = struct.Struct("<IBBBBIhhB")
AUDIO = struct.Struct("<IBBBBIHHBBhhB160s")
BEACON = struct.Struct("<IBBBBIBB")
VOICE = struct.Struct("<IBBBBIHBB")
SIZES = {"head": HEAD.size, "hello": HELLO.size, "cmd": CMD.size, "ack": ACK.size, "claim": CLAIM.size,
         "audio": AUDIO.size, "beacon": BEACON.size, "voice": VOICE.size}


def head(data):
    """(proto, type, slot, flags) of any Dust packet, or None."""
    if len(data) < HEAD.size:
        return None
    magic, proto, typ, slot, flags = HEAD.unpack_from(data)
    return (proto, typ, slot, flags) if magic == MAGIC else None


@dataclass
class Hello:
    slot: int
    flags: int
    uid: int
    sides: int
    rot: int
    epoch: int
    fps: float
    heap_kb: int
    battery: int        # 255 unknown
    temp_c: int         # -128 unknown
    err: int
    reset: int
    can_listen: bool
    session_db: float
    session_left_s: float
    mic_cal_db: float

    @property
    def lifted(self):
        return bool(self.flags & HEAD_LIFTED)

    @property
    def docked(self):
        return not self.flags & (HEAD_LIFTED | HEAD_SETUP) and self.slot < 9

    def errors(self):
        return [name for bit, name in enumerate(ERRORS) if self.err >> bit & 1]


def parse_hello(data):
    if len(data) < HELLO.size:
        return None
    (_, _, _, slot, flags, uid, sides, rot, epoch, fps, heap, batt, temp, err, reset, listen, sess, left, mic) = HELLO.unpack_from(data)
    return Hello(slot, flags, uid, sides, rot, epoch, fps / 10, heap, batt, temp, err, reset, bool(listen), sess / 10, left / 10, mic / 10)


@dataclass
class Audio:
    slot: int
    uid: int
    utt: int
    seq: int
    flags: int
    nbytes: int
    score_db: float
    pred: int
    index: int
    data: bytes


def parse_audio(data):
    if len(data) < AUDIO.size - 160:
        return None
    data = data + bytes(AUDIO.size - len(data))   # END frames are sent without their empty payload
    (_, _, _, slot, _, uid, utt, seq, flags, nbytes, score, pred, index, payload) = AUDIO.unpack_from(data)
    return Audio(slot, uid, utt, seq, flags, nbytes, score / 10, pred, index, payload[:nbytes])


def parse_ack(data):
    if len(data) < ACK.size:
        return None
    _, _, _, _, _, sender, seq, epoch = ACK.unpack_from(data)
    return sender, seq, epoch


def _head(typ, flags=0):
    return (MAGIC, PROTO, typ, SLOT_UNKNOWN, flags)


def make_beacon(uid, caps, busy):
    return BEACON.pack(*_head(PKT_BEACON), uid, caps, busy)


def make_cmd(sender, seq, op, a=0, text=b"", x=0.0, y=0.0, dx=0.0, dy=0.0, strength=0.0, radius=0.0, seconds=0.0, numbered=True):
    q = lambda v: int(max(-1.9, min(1.9, v)) * UV_Q)
    q01 = lambda v: int(max(0.0, min(1.0, v)) * 255)
    return CMD.pack(*_head(PKT_CMD), sender, seq if numbered else 0, CMD_F_ACK if numbered else 0, op, a,
                    text[:TEXT_MAX].ljust(TEXT_MAX, b"\0"), q(x), q(y), int(dx * 127), int(dy * 127),
                    q01(strength), q01(radius), int(max(1, min(255, seconds * 10))))


def make_voice(uid, utt, op, arg=0):
    return VOICE.pack(*_head(PKT_VOICE), uid, utt, op, arg)


def make_audio_down(uid, utt, seq, pred, index, payload, end=False):
    flags = AUDIO_DOWN | (AUDIO_END if end else 0) | (AUDIO_START if seq == 0 else 0)
    return AUDIO.pack(*_head(PKT_AUDIO), uid, utt, seq, flags, len(payload), 0, pred, index, payload.ljust(160, b"\0"))


# ---------- IMA ADPCM (same as main/dust_adpcm.c) ----------
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
        24623, 27086, 29794, 32767]
INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


class Adpcm:
    """Coder state: pred, index. encode/decode carry it from call to call, like the C version."""

    def __init__(self, pred=0, index=0):
        self.pred, self.index = pred, index

    def _step(self, code):
        step = STEP[self.index]
        diff = step >> 3
        if code & 4:
            diff += step
        if code & 2:
            diff += step >> 1
        if code & 1:
            diff += step >> 2
        p = self.pred - diff if code & 8 else self.pred + diff
        self.pred = max(-32768, min(32767, p))
        self.index = max(0, min(88, self.index + INDEX_ADJ[code & 7]))
        return self.pred

    def _encode_one(self, x):
        step, diff, code = STEP[self.index], x - self.pred, 0
        if diff < 0:
            code, diff = 8, -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        self._step(code)
        return code

    def encode(self, pcm):
        """Samples (ints, even count) → bytes, low nibble first."""
        out = bytearray(len(pcm) // 2)
        for i in range(0, len(pcm) - 1, 2):
            out[i // 2] = self._encode_one(pcm[i]) | (self._encode_one(pcm[i + 1]) << 4)
        return bytes(out)

    def decode(self, data, n=None):
        n = len(data) * 2 if n is None else n
        out = [0] * n
        for i in range(0, n - 1, 2):
            b = data[i // 2]
            out[i] = self._step(b & 0xF)
            out[i + 1] = self._step(b >> 4)
        return out
