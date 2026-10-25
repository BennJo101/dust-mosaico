// Wall sync over ESP-NOW. Every screen says hello; the lowest-numbered screen on the wall
// leads. The leader runs the wall's physics (the fluid fit and its slosh, chunks draining,
// shapes, the AI's effects) and broadcasts the result 20 times a second, so all nine
// screens draw the same moment.
//
// Agreement (dust_consensus.h): leaders are numbered by epoch, so a stale one can't drag the
// wall back; two tiles set to the same slot are noticed and one stands aside; hellos carry
// each tile's health. Commands are numbered and acknowledged (dust_reliable.h). Voice: tiles
// bid for who answers (dust_arb.h) and stream audio to and from the Jetson through a bridge
// ESP32 on the same channel (bridge/), which forwards packets both ways over USB.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "dust_render.h"
#include "dust_consensus.h"

#define DUST_MAGIC 0x54535544u  /* "DUST" */
#define DUST_PROTO 6
#define DUST_SLOT_UNKNOWN 255

typedef enum { PKT_HELLO = 1, PKT_SYNC = 2, PKT_CMD = 3, PKT_LAYOUT = 4, PKT_ACK = 5, PKT_CLAIM = 6,
               PKT_AUDIO = 7, PKT_BEACON = 8, PKT_VOICE = 9 } dust_pkt_type_t;

// Header flags.
enum { HEAD_LIFTED = 1, HEAD_SETUP = 2, HEAD_SPEAKING = 4, HEAD_SESSION = 8 };

// Every packet starts like this, in every protocol version: a tile hearing a version it can't
// read can still tell it's there (and which of them is out of date).
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  proto, type, slot, flags;
} pkt_head_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, amount, radius, release;  // amount 0..255, radius uv*255, release 0..1.25 → 0..255
    int16_t  x, y;                       // uv * 16384
    int8_t   dx, dy;                     // direction * 127
    float    phase;
} pkt_fx_t;

typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t wall_ms;
    uint32_t epoch;                      // the leader's epoch (dust_consensus.h)
    uint16_t present, lifted;
    uint8_t  state, talk, assemble, shake, scatter;
    uint8_t  take[9], pres[9];
    uint16_t frame_cx, frame_cy, frame_sx, frame_sy;   // uv * 16384
    uint8_t  form_a, form_b, form_amt, form_swap;
    char     text_a[DUST_TEXT_MAX], text_b[DUST_TEXT_MAX];
    uint16_t warp_from, warp_to;         // bend field: blending between the fields for two screen sets
    int16_t  warp_k;                     // progress * 10000 (overshoots a little: the slosh)
    pkt_fx_t fx[DUST_MAX_FX];
} pkt_sync_t;

// Every tile, every 100 ms: who it is, which of its sides have a neighbour, how it's doing.
typedef struct __attribute__((packed)) {
    pkt_head_t h;              // h.slot = DUST_SLOT_UNKNOWN until the tile knows where it is
    uint32_t uid;              // from the tile's MAC
    uint8_t  sides;            // bit 0 top, 1 right, 2 bottom, 3 left (wall frame), bit 4 on the wall plate
    uint8_t  rot;              // quarter turns the tile is mounted at
    uint32_t epoch;            // epoch of the leader it follows
    uint16_t fps_x10, heap_kb;
    uint8_t  battery;          // %, 255 unknown
    int8_t   temp_c;           // -128 unknown
    uint16_t err;              // DUST_ERR_*
    uint8_t  reset, can_listen;
    int16_t  session_x10;      // HEAD_SESSION: the bid that won it the conversation, dB * 10
    uint8_t  session_left;     // HEAD_SESSION: how long it keeps it, tenths of a second
    int16_t  mic_cal_x10;      // last mic calibration: speech level above the floor, dB * 10 (0 = none)
} pkt_hello_t;

// Sent by the tile with the lowest uid when nine tiles form a complete 3x3: each tile's slot.
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t uid[9];
    uint8_t  slot[9];
} pkt_layout_t;

// Commands anyone on the channel can send (the Jetson through the bridge, the USB console, a tap).
typedef enum { CMD_STATE = 1, CMD_TALK, CMD_FORM, CMD_FX, CMD_SCATTER, CMD_CALM, CMD_CAL_MIC } dust_cmd_op_t;
enum { CMD_F_ACK = 1 };        // numbered: the leader acknowledges it and repeats are dropped

typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t sender;                     // uid of whoever sent it (the bridge uses its own)
    uint16_t seq;                        // per sender, counts up
    uint8_t  cflags;                     // CMD_F_*
    uint8_t  op;
    uint8_t  a;                          // state / talk level / form id / fx type
    char     text[DUST_TEXT_MAX];        // form text
    int16_t  x, y;                       // fx centre, uv * 16384
    int8_t   dx, dy;                     // fx direction * 127
    uint8_t  strength, radius, duration; // 0..255, uv*255, tenths of a second
} pkt_cmd_t;

// The leader heard command (sender, seq).
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t sender;
    uint16_t seq;
    uint32_t epoch;
} pkt_ack_t;

// A bid to answer the wake word (sent three times; ESP-NOW broadcasts aren't acknowledged).
enum { CLAIM_TAKING = 1, CLAIM_DOCKED = 2 };
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t uid;
    int16_t  score_x10;                  // dB above the tile's noise floor (plus bonuses) * 10
    int16_t  group_x10;                  // CLAIM_TAKING: the winning group's bid (the wall's average), dB * 10
    uint8_t  cflags;                     // CLAIM_TAKING: it won and is answering. CLAIM_DOCKED: it bids with the wall
} pkt_claim_t;

// 20 ms of 16 kHz speech, IMA ADPCM. Up: a tile's mic to the Jetson. Down: Dust's voice to a tile.
#define AUDIO_RATE    16000
#define AUDIO_FRAME   320                // samples per packet
enum { AUDIO_START = 1, AUDIO_END = 2, AUDIO_DOWN = 4 };
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t uid;                        // the tile it comes from (up) or is for (down)
    uint16_t utt;                        // which utterance / reply
    uint16_t seq;                        // frame number within it
    uint8_t  aflags;                     // AUDIO_*
    uint8_t  nbytes;                     // ADPCM bytes in data (AUDIO_FRAME / 2, less in the last frame)
    int16_t  score_x10;                  // up, on START: the winning bid, so the Jetson can settle doubles
    int16_t  pred;                       // coder state at the start of this frame
    uint8_t  index;
    uint8_t  data[AUDIO_FRAME / 2];
} pkt_audio_t;

// From the Jetson (through the bridge) twice a second: it's there, and what it can do.
enum { BRAIN_STT = 1, BRAIN_LLM = 2, BRAIN_TTS_CLOUD = 4, BRAIN_TTS_LOCAL = 8 };
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t uid;                        // the bridge's
    uint8_t  caps;                       // BRAIN_*
    uint8_t  busy;                       // replies in progress
} pkt_beacon_t;

// From the Jetson to the tile it's talking with.
typedef enum { VOICE_YIELD = 1, VOICE_THINKING, VOICE_REPLY_START, VOICE_REPLY_END, VOICE_FAIL } dust_voice_op_t;
typedef struct __attribute__((packed)) {
    pkt_head_t h;
    uint32_t uid;                        // the tile it's for
    uint16_t utt;
    uint8_t  op;                         // dust_voice_op_t
    uint8_t  arg;                        // VOICE_FAIL: a clip to play instead (dust_voice.h)
} pkt_voice_t;

// What one screen needs each frame.
typedef struct {
    bool     have_sync, am_leader, self_dup;
    uint16_t present, lifted, ears;
    uint32_t epoch;
    dust_state_t state;
    dust_view_t view;        // shared fields filled in (t, talk, frame, pres, take, forms, fx, ...)
    uint16_t warp_from, warp_to; float warp_k;   // build view.warp with dust_warp_blend()
} wall_snapshot_t;

esp_err_t mosaic_net_start(uint8_t my_slot, uint8_t channel, float gap_px);
void mosaic_net_set_lifted(bool lifted);
void mosaic_net_set_setup(bool in_setup);
void mosaic_net_set_sides(uint8_t wall_sides, uint8_t rot);   // from the magnetometers
void mosaic_net_set_health(const dust_health_t *h);          // sent in every hello
// Slot this tile was assigned by an automatic layout (-1 if none yet). The app saves it and
// restarts into that slot.
int  mosaic_net_assigned_slot(void);
uint32_t mosaic_net_uid(void);
void mosaic_net_snapshot(wall_snapshot_t *out);
// Errors the network noticed: DUST_ERR_DUP_SLOT, DUST_ERR_FW, DUST_ERR_CMD_LOST.
uint16_t mosaic_net_errors(void);
// This tile runs older firmware than another one it can hear: it should say "update me".
bool mosaic_net_outdated(void);
// Print every tile this one can hear, with its health (console "health").
void mosaic_net_print_tiles(void);

/* ---------- voice plumbing (dust_voice.c) ---------- */
// Claims, audio, voice control, and hellos from tiles speaking or owning the conversation, are
// handed to this (from the Wi-Fi task: copy and return).
typedef void (*mosaic_voice_rx_t)(const uint8_t src_mac[6], const void *pkt, int len);
void mosaic_net_set_voice_rx(mosaic_voice_rx_t cb);
// Speaking / owning the conversation, for the hello flags.
void mosaic_net_set_voice(bool speaking, bool session, float session_score, int session_left_ms);
void mosaic_net_set_mic_cal(float level_db);
// Median of the other tiles' mic calibrations (NAN if none heard).
float mosaic_net_mic_cal_median(void);
// The Jetson's bridge, if a beacon was heard in the last 1.5 s.
bool mosaic_net_brain(uint8_t *caps);
esp_err_t mosaic_net_send_brain(const void *pkt, int len);   // unicast to the bridge
void mosaic_net_broadcast(const void *pkt, int len);
void mosaic_net_head(pkt_head_t *h, uint8_t type);          // fill a header for this tile
bool mosaic_net_is_ear(void);                               // the wall wants this tile listening

// Commands: sent to the whole wall and applied here too.
void dust_cmd_state(dust_state_t s);
void dust_cmd_talk(int level_0_255);
void dust_cmd_form(dust_form_t form, const char *text);
void dust_cmd_fx(dust_fx_type_t type, float x, float y, float dx, float dy, float strength, float radius, float seconds);
void dust_cmd_scatter(void);
void dust_cmd_calm(void);
void dust_cmd_cal_mic(void);
// Called for CMD_CAL_MIC (every tile measures its mic at the same time). It runs with the
// network lock held, so it must only set a flag.
void mosaic_net_set_cal_mic_cb(void (*cb)(void));
