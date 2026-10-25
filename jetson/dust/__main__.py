"""Run Dust's brain:  python -m dust --port /dev/ttyUSB0 --voice-id <ElevenLabs voice id>

Environment: ELEVENLABS_API_KEY (needed for the ElevenLabs voice), ELEVENLABS_VOICE_ID (instead of --voice-id)."""
import argparse
import logging
import os
import threading
import time

from . import proto, stt as stt_mod
from .link import Bridge
from .llm import Brain
from .tts import ElevenLabs, Piper, Voice
from .voice import Conversations
from .wall import Wall

log = logging.getLogger("dust")


def main():
    ap = argparse.ArgumentParser(prog="python -m dust", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0", help="serial port of the bridge ESP32 (/dev/ttyACM0 for native USB)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--channel", type=int, default=0, help="move the bridge to this Wi-Fi channel (0: leave it)")
    ap.add_argument("--model", default="qwen2.5:3b", help="Ollama model (one that can call tools)")
    ap.add_argument("--ollama", default="http://127.0.0.1:11434")
    ap.add_argument("--stt", choices=["faster-whisper", "whispercpp"], default="faster-whisper")
    ap.add_argument("--whisper-model", default="small.en")
    ap.add_argument("--whisper-url", default="http://127.0.0.1:8080/inference", help="whisper.cpp server, for --stt whispercpp")
    ap.add_argument("--voice-id", default=os.environ.get("ELEVENLABS_VOICE_ID", ""), help="ElevenLabs voice (Edward's id from My Voices)")
    ap.add_argument("--eleven-model", default="eleven_flash_v2_5")
    ap.add_argument("--piper-model", default=os.environ.get("PIPER_MODEL", ""), help="Piper .onnx voice, used when ElevenLabs can't be reached")
    ap.add_argument("--log", default="INFO")
    args = ap.parse_args()
    logging.basicConfig(level=args.log.upper(), format="%(asctime)s %(name)s: %(message)s", datefmt="%H:%M:%S")

    state = {"stt": False, "llm": False}
    conv = None
    cloud = ElevenLabs(args.voice_id, model=args.eleven_model)
    local = Piper(args.piper_model)

    def caps():
        c = 0
        tts_ok = cloud.ready or local.ready
        # tiles only stream to the Jetson when it can do the whole job; otherwise they use
        # their own offline commands, which is better than a reply that never comes
        if state["stt"] and state["llm"] and tts_ok:
            c |= proto.BRAIN_STT | proto.BRAIN_LLM
        c |= proto.BRAIN_TTS_CLOUD if cloud.ready else 0
        c |= proto.BRAIN_TTS_LOCAL if local.ready else 0
        return c

    bridge = Bridge(args.port, args.baud)
    wall = Wall(bridge, caps=caps, busy=lambda: conv.replying if conv else 0)
    bridge.on_packet = wall.on_packet
    if args.channel:
        bridge.set_channel(args.channel)

    log.info("loading speech to text (%s)", args.stt)
    stt = stt_mod.make(args.stt, args.whisper_model, args.whisper_url)
    state["stt"] = True
    brain = Brain(wall, args.model, args.ollama)
    conv = Conversations(wall, stt, brain, Voice(cloud, local))
    wall.on_voice_packet = conv.on_packet
    log.info("voice: %s%s", "ElevenLabs" if cloud.ready else "no ElevenLabs key/voice", ", Piper fallback" if local.ready else "")

    def watch_llm():
        while True:
            ok = brain.available()
            if ok != state["llm"]:
                log.info("LLM %s %s", args.model, "ready" if ok else f"not available (ollama pull {args.model}?)")
            state["llm"] = ok
            time.sleep(10)
    threading.Thread(target=watch_llm, daemon=True).start()

    last_report = 0.0
    try:
        while True:
            time.sleep(1)
            if not bridge.alive:
                log.warning("no status from the bridge on %s", args.port)
            if time.monotonic() - last_report > 60:
                last_report = time.monotonic()
                st = bridge.status
                log.info("wall:\n%s", wall.health())
                if st:
                    log.info("bridge: channel %d, heard %d, sent %d, failed %d, dropped %d, bad frames %d", st[1], st[3], st[4], st[5], st[6], bridge.bad_frames)
    except KeyboardInterrupt:
        pass
    finally:
        wall.close()
        bridge.close()


if __name__ == "__main__":
    main()
