"""Speech to text, on the Jetson. Two ways to run Whisper:

  faster-whisper   in this process (CTranslate2; on a Jetson use a CUDA build, or it runs on the CPU)
  whisper.cpp      its HTTP server (examples/server), built with CUDA for the Orin
"""
import io
import json
import logging
import struct
import urllib.request
import uuid

log = logging.getLogger("dust.stt")


def wav_bytes(pcm, rate=16000):
    data = struct.pack(f"<{len(pcm)}h", *pcm)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
            + b"data" + struct.pack("<I", len(data)) + data)


class FasterWhisper:
    def __init__(self, model="small.en"):
        from faster_whisper import WhisperModel
        try:
            self.model = WhisperModel(model, device="cuda", compute_type="float16")
            log.info("whisper %s on the GPU", model)
        except Exception as e:
            log.warning("whisper on the GPU failed (%s): using the CPU", e)
            self.model = WhisperModel(model, device="cpu", compute_type="int8")

    def transcribe(self, pcm):
        import numpy as np
        audio = np.asarray(pcm, dtype=np.float32) / 32768.0
        segments, _ = self.model.transcribe(audio, language="en", beam_size=1, condition_on_previous_text=False)
        return " ".join(s.text.strip() for s in segments).strip()


class WhisperCpp:
    def __init__(self, url="http://127.0.0.1:8080/inference"):
        self.url = url

    def transcribe(self, pcm):
        boundary = uuid.uuid4().hex
        body = io.BytesIO()
        for name, value in (("response_format", "json"), ("temperature", "0")):
            body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode())
        body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n".encode())
        body.write(wav_bytes(pcm))
        body.write(f"\r\n--{boundary}--\r\n".encode())
        req = urllib.request.Request(self.url, data=body.getvalue(), headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r).get("text", "").strip()


def make(kind, model, url):
    return WhisperCpp(url) if kind == "whispercpp" else FasterWhisper(model)
