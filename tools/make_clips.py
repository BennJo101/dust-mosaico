#!/usr/bin/env python3
"""Record Dust's offline lines in his ElevenLabs voice and pack them for the tiles.

The tiles play these when they can't reach the Jetson (see CLIP_* in main/dust_voice.h; the
order here must match). The result, clips/clips.bin, is flashed into the "clips" partition
along with the app by `idf.py flash`.

    ELEVENLABS_API_KEY=... python3 tools/make_clips.py --voice-id <Edward's voice id>
    python3 tools/make_clips.py --wav-dir my_lines/      # or pack your own 0.wav .. 5.wav (16 kHz mono)

Format: "DCLP", count (u16), sample rate (u16), then (offset, samples) per clip, then each clip
as IMA ADPCM from a zero state (the same coder as main/dust_adpcm.c).
"""
import argparse
import json
import os
import struct
import sys
import urllib.request
import wave

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "jetson"))
from dust.proto import Adpcm  # noqa: E402

LINES = [
    "Yes?",                                    # CLIP_YES       wake word heard, Jetson offline: say a command
    "As you wish.",                            # CLIP_OK        command done
    "I didn't catch that.",                    # CLIP_UNKNOWN
    "My thoughts are far away.",               # CLIP_OFFLINE
    "I lost my train of thought.",             # CLIP_NO_REPLY
    "I'm fading. Put me back on the wall.",    # CLIP_LOW_BATT
]
RATE = 16000
PARTITION = 1024 * 1024   # partitions.csv: clips, 1M


def from_elevenlabs(text, voice_id, key, model):
    url = f"https://api.elevenlabs.io/v1/text-to-speech/{voice_id}?output_format=pcm_16000"
    body = json.dumps({"text": text, "model_id": model, "voice_settings": {"stability": 0.5, "similarity_boost": 0.8}}).encode()
    req = urllib.request.Request(url, data=body, headers={"xi-api-key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
    return list(struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2]))


def from_wav(path):
    with wave.open(path) as w:
        if w.getframerate() != RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit(f"{path}: needs 16 kHz, mono, 16-bit")
        raw = w.readframes(w.getnframes())
    return list(struct.unpack(f"<{len(raw) // 2}h", raw))


def trim(pcm, threshold=300, pad=RATE // 20):
    """Cut leading and trailing silence, keeping 50 ms either side."""
    loud = [i for i, v in enumerate(pcm) if abs(v) > threshold]
    if not loud:
        return pcm
    return pcm[max(0, loud[0] - pad):min(len(pcm), loud[-1] + pad)]


def pack(clips):
    head = struct.pack("<4sHH", b"DCLP", len(clips), RATE)
    table, data = b"", b""
    offset = len(head) + 8 * len(clips)
    for pcm in clips:
        if len(pcm) % 2:
            pcm = pcm + [0]
        enc = Adpcm().encode(pcm)
        table += struct.pack("<II", offset + len(data), len(pcm))
        data += enc
    return head + table + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice-id", default=os.environ.get("ELEVENLABS_VOICE_ID", ""))
    ap.add_argument("--model", default="eleven_multilingual_v2", help="ElevenLabs model (these are recorded once, so quality over speed)")
    ap.add_argument("--wav-dir", help="pack 0.wav .. 5.wav from here instead of calling ElevenLabs")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "clips", "clips.bin"))
    args = ap.parse_args()

    clips = []
    for i, text in enumerate(LINES):
        if args.wav_dir:
            pcm = from_wav(os.path.join(args.wav_dir, f"{i}.wav"))
        else:
            key = os.environ.get("ELEVENLABS_API_KEY")
            if not key or not args.voice_id:
                sys.exit("set ELEVENLABS_API_KEY and --voice-id (or use --wav-dir)")
            pcm = from_elevenlabs(text, args.voice_id, key, args.model)
        pcm = trim(pcm)
        clips.append(pcm)
        print(f"{i}: {len(pcm) / RATE:4.1f} s  {text}")
    blob = pack(clips)
    if len(blob) > PARTITION:
        sys.exit(f"{len(blob)} bytes: more than the 1 MB clips partition")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"wrote {args.out}: {len(blob) / 1024:.0f} KB of {PARTITION // 1024} KB; flash it with idf.py flash")


if __name__ == "__main__":
    main()
