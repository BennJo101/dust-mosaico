# Dust on ESP-Mosaico

Nine ESP-Mosaico screens in a 3×3 wall show one Dust. Lift a screen off the wall and Dust
shudders, a ninth of his dust tears away through that slot, and the lifted screen fills with
a ball of dust you can carry. Put it back and the dust flows home.

Dust behaves like liquid in a container. When screens are missing, powered off or in a hand,
he keeps his volume (minus the ninths that left), stretches to fill the screens that are
there, and bends out of every gap instead of standing behind it. Your AI can also pour him
into shapes and push him around (see **Letting your AI move the dust**).

Every screen runs the same firmware. The web simulator (Dust Mosaic) uses the same rules,
so it previews what the wall will do.

## Status

- **Renderer, layout and bend field:** build and run on a desktop (`host/`). Output checked
  panel by panel for the full wall, missing corners, two lifted screens, a missing centre,
  a single column, a ring, a word, a swirl and a gather.
- **Firmware:** written against ESP-IDF 6.2+ and Espressif's official
  [esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp) (display, touch, IMU, both
  magnetometers, power rails, motor). `board_port.c` is syntax-checked against the real BSP
  headers but **not yet compiled or run on an ESP-Mosaico**. One spot is marked `CHECK`: the
  BMM150 fixed-point scale.
- **Neighbour sensing:** the tiles touch and hold together with small edge magnets, and each
  tile's two magnetometers feel which edges have a neighbour (and whether it's on the wall
  plate). That gives lifting, auto layout and rotation. The classifier and layout are tested
  on the desktop (`host/topo_test`: 9,600 noisy readings, none wrong); `board_port_mag3()`
  still needs wiring to the real BMM150s. Until it is, hold a screen for 1.2 s to lift it.
- **Hardware:** printable magnetic sleeves and a wall plate with single-cable power are in
  `hardware/` (see `hardware/BUILD.md`). `tools/magnet_check.py` checks a magnet layout
  against the sensor positions before you print.
- **Voice:** every tile listens and speaks (wake word, offline commands, recorded lines in
  Dust's voice), and a Jetson Orin Nano does the rest (Whisper, a local LLM, ElevenLabs). The
  closest tile answers, on the wall or off it. See **Talking to Dust**.
- **Agreement and errors:** leaders are numbered by epoch, commands are acknowledged and never
  applied twice, duplicate slots and firmware mismatches are caught, every tile reports its
  health, and watchdogs restart a stuck tile. See **How the wall stays in sync**.
- **What has been tested, and what hasn't:** the agreement rules, reliable commands, the
  voice auction (20,000 utterances in a simulated room, at 0, 10 and 30 % packet loss), the
  audio codec and the USB framing all run on a desktop (`host/net_test`, `jetson/tests`), and
  the C and Python sides produce byte-identical packets. The ESP-specific files type-check
  against esp-sr's real headers and stand-ins for ESP-IDF's, but **none of it has been built with
  ESP-IDF or run on a board yet**. Spots to check on hardware are marked `CHECK`.

## Layout

```
main/
  main.c          app: setup screen, lift detection, render loop, USB console
  dust_render.c   Dust's dots for this screen's slice: shapes, effects, container, bloom, 240→480
  dust_layout.c   fluid fit: where Dust stands and how far he stretches when screens are missing
  dust_warp.c     the bend field that squeezes him out of gaps
  dust_topo.c     which sides have a neighbour (from the magnetometers), and auto layout
  mosaic_net.c    ESP-NOW wall sync and the leader's physics (slosh, chunks, shapes, effects)
  dust_consensus.c  who's on the wall, who leads (epochs), whose picture to follow, who listens
  dust_reliable.c   numbered commands: acknowledged, repeated, never applied twice
  dust_arb.c      who answers the wake word: the closest group (the wall, or a lifted tile)
  dust_voice.c    mic, wake word, offline commands, streaming to the Jetson, replies, clips
  dust_adpcm.c    4-bit speech codec for the radio and for the clips
  board_port.c    display, touch, IMU, magnetometers, motor, battery, AI button via the official BSP
  dust_points.h   12,500 baked dots (generated)
bridge/             firmware for any ESP32 on the USB port of the Jetson: ESP-NOW <-> USB
jetson/             Dust's brain: Whisper, Ollama, ElevenLabs/Piper, health (Python)
tools/bake_dust.py  regenerates dust_points.h (sizes, seed)
tools/magnet_check.py  checks an edge-magnet layout against the magnetometer positions
tools/make_clips.py    records Dust's offline lines with ElevenLabs into clips/clips.bin
hardware/           sleeves, wall plate, build guide
host/               desktop builds: the renderer, neighbour sensing, and the network/voice tests
partitions.csv      app 4 MB, speech models 6 MB, recorded lines 1 MB
```

## Build and flash

1. Install ESP-IDF **6.2 or newer** with ESP32-S31 support.
2. In this folder:
   ```
   idf.py --preview set-target esp32s31
   idf.py menuconfig                  # "Dust mosaic": channel and bezel gap
   idf.py -p /dev/ttyACM0 flash monitor
   ```
   The component manager fetches the BSP and esp-sr on the first build. `sdkconfig.defaults`
   builds the BSP without LVGL (Dust draws his own frames), picks the "Hi ESP" wake word and
   English MultiNet6 offline commands, and uses `partitions.csv`; `idf.py flash` writes the
   speech models too.
3. Optional, for Dust's voice when the Jetson is off:
   `ELEVENLABS_API_KEY=... python3 tools/make_clips.py --voice-id <voice id>` writes
   `clips/clips.bin`, and `idf.py flash` includes it from then on.
4. Flash the same build to all nine screens. The BSP reads each board's revision (V1.0 or V1.2)
   from eFuse and picks the right pins itself.
5. For the full voice: flash `bridge/` to any ESP32 plugged into the Jetson and start
   `jetson/` (see `jetson/README.md`).

## Before you print sleeves: the real Mosaico already snaps together

Espressif's own Mosaico enclosure (resin-printed, STLs on MakerWorld) already has edge
magnets, and the BSP ships `mosaico_interaction` (edge classification from the two BMM150s)
plus `mosaico_peer_link` / `mosaico_network` (ESP-NOW pairing, 3×3 topology, lowest-ID root,
display rotation). Three things in their docs matter for this wall:

- **No iron plate.** Their magnetic classifier is calibrated for non-ferromagnetic surfaces;
  steel near the sensors overlaps the contact fields. The sleeve washers and plate magnets in
  `hardware/` are the risky part.
- **Rotation is 0° or 180° only** with their magnets: TOP/BOTTOM edges can't bind to
  LEFT/RIGHT. The sleeves in `hardware/` are designed for any rotation, theirs aren't.
- **The left and right edges are module slots** (2×10 headers). A module plugged into a side
  stops a neighbour joining that side.

So there are two ways to build the wall: the sleeves here (any rotation, own calibration in
`dust_topo.c`), or the official enclosures with their magnets and topology code, mounted on a
non-magnetic plate. The second uses tested Espressif code; the first needs your own magnet
check.

## First boot: calibrate, then the wall lays itself out

1. **Calibrate each tile once** over USB: `cal alone` (off the wall, nothing nearby), then
   `cal top`, `cal right`, `cal bottom`, `cal left` with one other sleeve snapped to that edge,
   then `cal back` on the wall plate. Saved on the tile.
2. **Snap all nine onto the plate**, any order, any rotation. Each tile shows the sides it
   feels. When the nine form a full 3×3, the tile with the lowest ID works out every
   position from the side masks (no top neighbour = top row, and so on), each tile turns its
   picture to match how it's mounted (from the motion sensor), and they restart into the wall.

To set a slot by hand instead, hold the setup screen for 2 s, tap through 1–9 and hold to
save, or use `slot N` over USB, or fix it at build time with `CONFIG_DUST_SLOT`.

## Lining Dust up across the bezels

Set `CONFIG_DUST_GAP_PX` to the gap between the lit areas of two neighbouring screens, in
panel pixels. A 2.16" 480×480 panel is 38.8 mm across, about 12.4 px per mm. With the
magnetic sleeves that gap is two sleeve walls, two fit clearances and two screen bezels,
about 12 mm or 148 px (the default).

## Letting your AI move the dust

Everything Dust can do is a small command. Give these to your assistant as tools (the JSON
schema below drops straight into most LLM tool-calling APIs), and have your bridge forward
each call to the wall.

| Command | What it does |
|---|---|
| `state idle\|listening\|thinking\|speaking` | Dust's mood: listening draws dust in, thinking spins the core, speaking pulses the glow |
| `talk 0-255` | Voice level; send ~30 times a second from your TTS so the glow follows the audio |
| `form orb\|ring\|wave\|spiral\|cloud` | Pour Dust into a shape (it bends to fit whatever screens are there) |
| `form text WORD` | Spell up to 10 letters (A–Z, 0–9, space, `! ? . , -`) |
| `form body` | Back to himself |
| `gather X Y` | Draw the dust toward a point, e.g. toward whoever is talking |
| `swirl X Y` | A vortex at a point |
| `flow DX DY` | A current: dust streams in a direction |
| `pulse X Y` | A ring wave out from a point |
| `scatter` | Blow apart and re-form |
| `calm` | Ease every effect out |

X and Y run 0–1 across the whole wall (0,0 top-left). Effects stack: a new swirl starts on
top of the last one. When an effect ends nothing rewinds or stops dead: a swirl coasts
down with momentum while each dot eases into drifting back to its place on its own.

### Where commands come from

- **Your voice**, through the Jetson (see **Talking to Dust**): the LLM gets `dust_form`,
  `dust_move` and `wall_health` as tools.
- **USB console** on any screen: type the commands above.
- **ESP-NOW** from any ESP32 on the same channel: broadcast a `pkt_cmd_t` (`mosaic_net.h`,
  magic `0x54535544`, proto 6, type 3) with your own sender id and a counting `seq`, and flag
  it `CMD_F_ACK` to have the leader acknowledge it (`pkt_ack_t`); repeat it until it does.
  `jetson/dust/wall.py` does exactly this through `bridge/`.
- **Tap** a screen to cycle the state for demos.

### Tool schema for an LLM

```json
[
  {"name": "dust_state", "description": "Set Dust's mood on the wall.",
   "input_schema": {"type": "object", "properties": {"state": {"enum": ["idle", "listening", "thinking", "speaking"]}}, "required": ["state"]}},
  {"name": "dust_form", "description": "Pour Dust into a shape, spell a short word, or return to his body.",
   "input_schema": {"type": "object", "properties": {
     "shape": {"enum": ["body", "orb", "ring", "wave", "spiral", "cloud", "text"]},
     "text": {"type": "string", "maxLength": 10, "description": "Only for shape=text. A-Z, 0-9, space, ! ? . , -"}}, "required": ["shape"]}},
  {"name": "dust_move", "description": "Push the dust around. x, y are 0-1 across the wall (0,0 top-left). Moves stack and ease out on their own.",
   "input_schema": {"type": "object", "properties": {
     "move": {"enum": ["gather", "swirl", "flow", "pulse", "scatter", "calm"]},
     "x": {"type": "number"}, "y": {"type": "number"},
     "dx": {"type": "number", "description": "flow direction"}, "dy": {"type": "number"},
     "seconds": {"type": "number", "default": 4}}, "required": ["move"]}}
]
```

## Talking to Dust

```
 you ──"Hi ESP, make a spiral"──►  every tile that hears it bids how close you sound
                                   │  (the wall bids its tiles' average; each lifted tile bids alone)
                                   ▼
                        the closest group answers, through its loudest tile
             Jetson reachable                          Jetson off
   ┌──────────────────────────────────┐     ┌───────────────────────────────────┐
   │ tile streams your words (ADPCM)  │     │ MultiNet on the tile: 12 commands │
   │ → bridge → Whisper → Ollama LLM  │     │ ("make a spiral", "come here",    │
   │ (tools move the dust)            │     │  "calm down", ...) + Dust's       │
   │ → ElevenLabs (Piper offline)     │     │  recorded lines from flash        │
   │ → back to that same tile         │     └───────────────────────────────────┘
   └──────────────────────────────────┘
```

- **Every tile works alone.** Each has its own mic, speaker, wake word and offline commands.
  On the wall, the wall chooses which tiles listen (`CONFIG_DUST_WALL_EARS`, centre and
  corners first, sparing the leader, and any tile drawing under 10 fps steps out). A lifted
  tile always listens for itself and answers on its own ball.
- **Only the closest answers.** Tiles that hear the wake word broadcast a bid: the voice's
  level above that tile's own noise floor, plus 3 dB if the tile is in someone's hand. After
  200 ms each tile works out the same winner from the bids it heard. The winner says "taking",
  so a tile that missed a better bid stands down, and if radio loss still leaves two tiles
  streaming, the Jetson keeps the better bid. In the desktop simulation (`host/net_test`) exactly
  one answers every time once the Jetson settles doubles. The clearly closer group (3 dB or
  more) answers 99.7 % of the time, at up to 30 % packet loss.
- **Conversations stick.** Whoever answered keeps the conversation for 8 s after Dust last
  spoke; follow-ups need no wake word. Another group needs a 6 dB closer bid to take over.
- **Dust doesn't hear himself.** A tile's mic is muted while it plays, and every tile ignores
  the wake word while any tile is speaking (the flag rides in the hellos).
- **The AI button** (GPIO 7) is push-to-talk: no wake word, it always wins, and it interrupts
  Dust mid-sentence.
- **Calibrate the mics once:** `cal mic` on any tile, then speak steadily from about 2 m in
  front of the wall for 4 s. Every tile measures at once and stores its offset from the
  wall's median, so bids from different boards compare fairly.
- The wake word is Espressif's "Hi ESP" (`wn9_hiesp`). A custom "Hey Dust" needs Espressif's
  wake word training service; set `CONFIG_DUST_WAKE_WORD` to the new model's name.

Console: `voice` (this tile's voice state), `wake 20` (pretend it heard the wake word at
20 dB), `say 0-5` (play a recorded line), `health` (every tile it can hear).

`CHECK` on hardware: the English MultiNet6 model's support on the ESP32-S31 (esp-sr ships S31
libraries; the board's own example uses the Chinese model), the mic gain (30 dB) and speaker
volume, and how much CPU the wake word takes from drawing (see the fps in `health`).

## How the wall stays in sync

Each screen says hello 10 times a second. The lowest-numbered screen that is on the wall
leads. It runs the wall's physics (the fluid fit and its slosh, the bend field, chunks
draining, shapes and the AI's effects) and broadcasts the result 20 times a second.
Followers render from that. If the leader is lifted or switched off, the next screen takes
over within about half a second.

Each screen owns a fixed ninth of Dust's dots, spread evenly over his body, so a lifted or
missing screen always drains the same amount without biting a hole in him. The bend field is
solved on every screen from the same screen set, so all nine agree without sending it.

### Agreement and errors

- **Epochs.** A screen that takes over leads under a new, higher epoch, stamped on every
  sync. Followers ignore pictures from an older epoch, so a leader that froze or wandered off
  can't come back and drag the wall to a stale picture; a leader that hears a newer epoch
  steps down, waits 300 ms, and takes over properly if it's still the rightful one. If the
  newest leader vanishes, followers fall back to whoever is still sending.
- **Commands arrive exactly once.** Every command is numbered by its sender. The leader
  acknowledges each one, the sender repeats it every 80 ms until it does (up to 10 times), and
  everyone drops repeats. Talk levels, sent 30 times a second, aren't numbered: the next one
  replaces a lost one. Tested at 30 % loss each way: no command applied twice.
- **Duplicate slots.** If two tiles think they're the same screen, the one with the lower uid
  keeps it; the other shows a red bar, stands aside as a ball, and reports it.
- **Firmware mismatches.** Every protocol version starts its packets the same way, so a tile
  can tell when it hears one it can't read. The older one shows an amber bar ("update me").
- **Health.** Every hello carries fps, free memory, battery, temperature, why the tile last
  restarted, and error flags (mic, speaker, magnetometers, motion sensor, overheating, low
  battery, lost commands, missing models or clips). `health` on the console lists every tile;
  the Jetson logs changes, and Dust can tell you himself ("screen 6 is running hot").
- **Leaders are chosen with care.** A tile that is overheating or nearly flat is passed over
  for leading.
- **Watchdogs.** The render loop, the radio task and the voice task are watched: a task stuck
  for 5 s restarts its tile, the wall carries on, and the tile rejoins and reports that it
  crashed (for 10 minutes).
- **Off the wall and running low:** a lifted tile below 10 % battery says so, once.

## Testing on a desktop

```
cd host && make test                       # neighbour sensing, agreement, commands, codec, framing, who answers
cd jetson && python3 -m unittest discover -s tests
```

## Performance notes

The renderer works at 240×240 and doubles to 480×480 on output. When Dust is settled, each
screen only animates the dots near its slice. While something is moving (the startup pour,
a chunk, a re-fit, a shape or an effect) it animates all 12,500. On a desktop a frame takes about 4 ms; on the ESP32-S31 at
320 MHz, expect it to be many times slower, so measure first (the log prints fps every 60
frames). If it's slow, lower the dot counts with `tools/bake_dust.py --head 4000 --torso 2600`.

The wall clock is a float in seconds, so after many hours of uptime the fastest flicker
loses smoothness. Power-cycling the whole wall resets it.
