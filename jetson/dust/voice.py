"""Conversations: what you say, streamed up from whichever tile won the bid; Whisper; the LLM;
and the reply streamed back down to that same tile (so Dust answers from where he heard you),
paced to real time.

The tiles settle among themselves who answers (main/dust_arb.c). If radio loss means two of
them both stream the same words anyway, this is the last word: the better bid keeps it and the
other is told to stand down."""
import logging
import queue
import struct
import threading
import time

from . import proto

log = logging.getLogger("dust.voice")

DOUBLE_S = 0.8          # two streams starting this close together are the same words
LEAD_S = 0.3            # send the reply this far ahead of real time (the tile buffers up to 4 s)
FRAME_BYTES = proto.AUDIO_FRAME * 2


class Stream:
    """One utterance from one tile."""

    def __init__(self, uid, utt, score, slot):
        self.uid, self.utt, self.score, self.slot = uid, utt, score, slot
        self.started = time.monotonic()
        self.frames = {}
        self.ended = self.yielded = False

    def add(self, a):
        self.frames[a.seq] = proto.Adpcm(a.pred, a.index).decode(a.data)

    def pcm(self):
        if not self.frames:
            return []
        out = []
        for seq in range(max(self.frames) + 1):
            out += self.frames.get(seq, [0] * proto.AUDIO_FRAME)   # a lost frame is 20 ms of silence
        return out

    def where(self):
        return f"screen {self.slot + 1}" if self.slot is not None and self.slot < 9 else f"tile {self.uid:08x}"


class Conversations:
    def __init__(self, wall, stt, brain, voice):
        self.wall, self.stt, self.brain, self.voice = wall, stt, brain, voice
        self.streams = {}
        self.replying = 0
        self._lock = threading.Lock()
        self._todo = queue.Queue()
        threading.Thread(target=self._worker, name="conversation", daemon=True).start()

    # ---------- audio coming up from the tiles ----------
    def on_packet(self, mac, rssi, data):
        h = proto.head(data)
        if not h or h[1] != proto.PKT_AUDIO:
            return
        a = proto.parse_audio(data)
        if not a or a.flags & proto.AUDIO_DOWN:
            return
        with self._lock:
            s = self.streams.get((a.uid, a.utt))
            if s is None:
                s = Stream(a.uid, a.utt, a.score_db if a.flags & proto.AUDIO_START else -99.0, a.slot)
                self.streams[(a.uid, a.utt)] = s
                self._settle(s)
                self._forget_old()
            if s.yielded or s.ended:
                return
            if a.nbytes:
                s.add(a)
            if a.flags & proto.AUDIO_END:
                s.ended = True
                self._todo.put(s)

    def _settle(self, new):
        for other in list(self.streams.values()):
            if other is new or other.yielded or other.uid == new.uid or abs(other.started - new.started) > DOUBLE_S:
                continue
            loser = new if (new.score, -new.uid) < (other.score, -other.uid) else other
            winner = other if loser is new else new
            loser.yielded = True
            log.info("%s and %s both heard it: %s is closer (%.1f vs %.1f dB)", loser.where(), winner.where(),
                     winner.where(), winner.score, loser.score)
            self._tell(loser, proto.VOICE_YIELD)
            if loser is new:
                return

    def _forget_old(self):
        now = time.monotonic()
        for k in [k for k, s in self.streams.items() if now - s.started > 60]:
            del self.streams[k]

    def _tell(self, s, op, arg=0, times=2):
        for _ in range(times):   # small and unacknowledged: send it twice
            self.wall.send_to(s.uid, proto.make_voice(s.uid, s.utt, op, arg))

    # ---------- answering ----------
    def _worker(self):
        while True:
            s = self._todo.get()
            if s.yielded:
                continue
            self.replying += 1
            try:
                self._answer(s)
            except Exception:
                log.exception("answering %s", s.where())
                self._tell(s, proto.VOICE_FAIL, proto.CLIP_NO_REPLY)
            finally:
                self.replying -= 1

    def _answer(self, s):
        self._tell(s, proto.VOICE_THINKING)
        pcm = s.pcm()
        if len(pcm) < proto.AUDIO_RATE * 3 // 10:
            self._tell(s, proto.VOICE_FAIL, proto.CLIP_UNKNOWN)
            return
        t0 = time.monotonic()
        heard = self.stt.transcribe(pcm)
        log.info("%s heard (%.1f s of audio, %.2f s to transcribe): %r", s.where(), len(pcm) / proto.AUDIO_RATE, time.monotonic() - t0, heard)
        if len(heard.strip(" .?!,-")) < 2:
            self._tell(s, proto.VOICE_FAIL, proto.CLIP_UNKNOWN)
            return
        if s.yielded:
            return
        t1 = time.monotonic()
        reply = self.brain.reply(heard)
        if reply is None:
            self._tell(s, proto.VOICE_FAIL, proto.CLIP_OFFLINE)
            return
        log.info("Dust says (%.2f s to think): %r", time.monotonic() - t1, reply)
        if not reply:   # it only moved the dust
            self._tell(s, proto.VOICE_FAIL, proto.CLIP_OK)
            return
        self._speak(s, reply)

    def _speak(self, s, text):
        enc, seq, buf, t0 = proto.Adpcm(), 0, b"", None
        try:
            for chunk in self.voice.stream(text):
                buf += chunk
                while len(buf) >= FRAME_BYTES:
                    if t0 is None:   # the first sound: now the tile can start
                        self._tell(s, proto.VOICE_REPLY_START, times=3)
                        t0 = time.monotonic()
                    self._send_frame(s, enc, buf[:FRAME_BYTES], seq, t0)
                    buf, seq = buf[FRAME_BYTES:], seq + 1
        except RuntimeError as e:
            log.error("%s", e)
            self._tell(s, proto.VOICE_FAIL, proto.CLIP_NO_REPLY)
            return
        if t0 is None:
            self._tell(s, proto.VOICE_REPLY_START, times=3)
            t0 = time.monotonic()
        if len(buf) >= 4:
            self._send_frame(s, enc, buf[:len(buf) // 4 * 4], seq, t0)
            seq += 1
        self._tell(s, proto.VOICE_REPLY_END, times=3)
        log.info("spoke %.1f s through %s (%s)", seq * 0.02, s.where(), self.voice.last_engine)

    def _send_frame(self, s, enc, frame, seq, t0):
        n = len(frame) // 2
        pcm = struct.unpack(f"<{n}h", frame)
        pred, index = enc.pred, enc.index
        pkt = proto.make_audio_down(s.uid, s.utt, seq, pred, index, enc.encode(pcm))
        wait = t0 + seq * 0.02 - LEAD_S - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        self.wall.send_to(s.uid, pkt)
