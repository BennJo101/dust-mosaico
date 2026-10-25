"""Dust's voice: ElevenLabs over the internet (the Edward voice, streamed as it's made), and
Piper on the Jetson when ElevenLabs can't be reached. Both yield 16 kHz 16-bit mono PCM bytes."""
import json
import logging
import os
import shutil
import struct
import subprocess
import urllib.error
import urllib.request

log = logging.getLogger("dust.tts")
RATE = 16000


class ElevenLabs:
    def __init__(self, voice_id, api_key=None, model="eleven_flash_v2_5"):
        self.voice_id = voice_id
        self.api_key = api_key or os.environ.get("ELEVENLABS_API_KEY", "")
        self.model = model

    @property
    def ready(self):
        return bool(self.api_key and self.voice_id)

    def stream(self, text):
        url = f"https://api.elevenlabs.io/v1/text-to-speech/{self.voice_id}/stream?output_format=pcm_16000"
        body = json.dumps({"text": text, "model_id": self.model,
                           "voice_settings": {"stability": 0.5, "similarity_boost": 0.8}}).encode()
        req = urllib.request.Request(url, data=body, headers={"xi-api-key": self.api_key, "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            while True:
                chunk = r.read(3200)   # 100 ms
                if not chunk:
                    break
                yield chunk


class Piper:
    def __init__(self, model_path, exe="piper"):
        self.model_path, self.exe = model_path, exe
        self.rate = 22050
        cfg = (model_path or "") + ".json"
        if os.path.exists(cfg):
            with open(cfg) as f:
                self.rate = json.load(f).get("audio", {}).get("sample_rate", self.rate)

    @property
    def ready(self):
        return bool(self.model_path and os.path.exists(self.model_path) and shutil.which(self.exe))

    def stream(self, text):
        p = subprocess.Popen([self.exe, "--model", self.model_path, "--output_raw"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        p.stdin.write(text.replace("\n", " ").encode() + b"\n")
        p.stdin.close()
        odd, tail, pos = b"", [], 0.0
        while True:
            chunk = p.stdout.read(4410)
            if not chunk:
                break
            chunk = odd + chunk
            n = len(chunk) // 2
            odd = chunk[n * 2:]
            samples = tail + list(struct.unpack(f"<{n}h", chunk[:n * 2]))
            out, pos = resample(samples, self.rate, RATE, pos)
            tail = samples[-1:]   # the next chunk interpolates on from here
            yield struct.pack(f"<{len(out)}h", *out)
        p.wait()


def resample(samples, rate_in, rate_out, pos=0.0):
    """Linear resampling. pos is where the first output sample falls in `samples`; returns the
    output and where the next one falls, counted from the last input sample."""
    step, out = rate_in / rate_out, []
    while pos < len(samples) - 1:
        i = int(pos)
        f = pos - i
        out.append(int(samples[i] * (1 - f) + samples[i + 1] * f))
        pos += step
    return out, (pos - (len(samples) - 1)) if samples else pos


class Voice:
    """ElevenLabs first; Piper if ElevenLabs fails before it has said anything."""

    def __init__(self, cloud, local):
        self.cloud, self.local = cloud, local
        self.last_engine = None

    def stream(self, text):
        if self.cloud and self.cloud.ready:
            started = False
            try:
                for chunk in self.cloud.stream(text):
                    started = True
                    self.last_engine = "elevenlabs"
                    yield chunk
                return
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                if started:
                    log.error("ElevenLabs stopped mid-sentence: %s", e)
                    return
                log.warning("ElevenLabs unavailable (%s): using Piper", e)
        if self.local and self.local.ready:
            self.last_engine = "piper"
            yield from self.local.stream(text)
            return
        raise RuntimeError("no text-to-speech available")
