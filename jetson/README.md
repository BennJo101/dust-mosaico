# Dust's brain on a Jetson Orin Nano

The tiles hear you and decide which of them answers (closest wins; see the main README). The
winner streams what you say to the Jetson through the bridge ESP32. Here it becomes text
(Whisper), a reply (a local LLM through Ollama, with tools to move the dust and check on the
tiles), and Dust's voice (ElevenLabs, or Piper when the internet is down). The voice streams back to
the same tile, so Dust answers from where he heard you.

Nothing leaves the Jetson except the reply text, which goes to ElevenLabs.

```
tile ──ESP-NOW──► bridge ESP32 ──USB──► python -m dust ─► Whisper ─► Ollama ─► ElevenLabs / Piper
  ▲                                                                                   │
  └──────────────────────────── the reply, 20 ms at a time ───────────────────────────┘
```

## Set up

1. **Bridge.** Flash `../bridge` to any ESP32 dev board (see `../bridge/README.md`) and plug it
   into the Jetson. It shows up as `/dev/ttyUSB0` (USB-to-serial chip) or `/dev/ttyACM0`
   (native USB). Add yourself to `dialout`: `sudo usermod -aG dialout $USER`, then log in again.
2. **LLM.** Install [Ollama](https://ollama.com) (it supports Jetson) and pull a small model
   that can call tools:
   ```
   curl -fsSL https://ollama.com/install.sh | sh
   ollama pull qwen2.5:3b          # or qwen3:4b, llama3.2:3b
   ```
3. **Speech to text**, either one of these:
   - `pip install faster-whisper`. This is the simplest option, but the pip build runs on the CPU
     (`small.en` takes about a second for a short sentence). For the GPU, use the
     `faster-whisper` container from [jetson-containers](https://github.com/dusty-nv/jetson-containers).
   - [whisper.cpp](https://github.com/ggml-org/whisper.cpp) built with CUDA
     (`cmake -B build -DGGML_CUDA=1 && cmake --build build -j`), running its server:
     `./build/bin/whisper-server -m models/ggml-small.en.bin --port 8080`. Then pass
     `--stt whispercpp`.
4. **Voice.** In ElevenLabs, add the Edward voice to *My Voices* and copy its voice ID. Put
   your key somewhere only you can read:
   ```
   sudo install -m 600 /dev/null /etc/dust.env
   echo 'ELEVENLABS_API_KEY=...'   | sudo tee -a /etc/dust.env
   echo 'ELEVENLABS_VOICE_ID=...'  | sudo tee -a /etc/dust.env
   ```
   Optional offline voice: `pip install piper-tts`, download a voice (e.g. `en_GB-alan-medium.onnx`
   and its `.json`) and pass `--piper-model /path/to/en_GB-alan-medium.onnx`.
5. **Run it:**
   ```
   pip install -r requirements.txt
   set -a; . /etc/dust.env; set +a
   python3 -m dust --port /dev/ttyUSB0
   ```
   To start it at boot: copy `dust.service` to `/etc/systemd/system/`, edit the paths, then
   `sudo systemctl enable --now dust`.

## What it tells the tiles

Twice a second it sends a beacon saying what it can do. Tiles only stream to it when it can do
the whole job: speech to text, the LLM, and a voice. Otherwise they use their own offline
commands, which beats waiting for a reply that never comes. If ElevenLabs fails before saying
anything, Piper takes over for that reply. If Whisper hears nothing useful, or the LLM fails,
the tile plays one of its recorded lines instead.

## Health

Every minute, and whenever a tile's problems change, the log shows each tile: where it is,
fps, battery, temperature and errors. The LLM has a `wall_health` tool, so you can just ask
Dust how he's feeling.

## Choosing a model

Memory is shared between the CPU and GPU. On the 8 GB Orin Nano, a 3–4B model at 4-bit
(about 3 GB), Whisper small (about 1 GB) and Piper fit comfortably. A 7–8B model fits but
replies more slowly. ElevenLabs runs in the cloud, so it costs no memory here.

## Tests

`python3 -m unittest discover -s tests` checks, with no hardware, that:
- packets and the speech codec are byte-identical to the firmware's
- the USB framing matches the bridge's
- doubles are settled by bid
- replies go to the right tile, in order
- commands repeat until acknowledged
