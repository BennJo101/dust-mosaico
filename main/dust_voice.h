// Dust's ears and voice on one tile (esp-sr on the ES8311 mic and speaker).
//
// Every tile can listen and speak on its own. On the wall, the tiles the wall picks as ears
// listen for the wake word; off the wall, a tile always listens for itself. When the wake word
// is heard, the tiles that heard it bid for who answers (dust_arb.h): the closest one wins.
//
// The winner then answers in the best way it can reach:
//   Jetson online   streams what you say to it (Whisper, the LLM, ElevenLabs) and plays the reply
//   Jetson offline  understands a fixed set of commands on the chip (MultiNet) and replies with
//                   lines recorded in Dust's voice (the "clips" partition, tools/make_clips.py)
// A tile in the wall shows it on the whole wall (listening, thinking, speaking, and the dust
// gathers toward it); a lifted tile shows it on its own ball.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "dust_render.h"

// Recorded lines, in the order tools/make_clips.py writes them.
typedef enum {
    CLIP_YES = 0,     // "Yes?"                       wake word heard, Jetson offline: say a command
    CLIP_OK,          // "As you wish."                command done
    CLIP_UNKNOWN,     // "I didn't catch that."
    CLIP_OFFLINE,     // "My thoughts are far away."   offline, and no commands on the chip either
    CLIP_NO_REPLY,    // "I lost my train of thought." the Jetson never answered
    CLIP_LOW_BATT,    // "I'm fading. Put me back on the wall."
    CLIP_COUNT
} dust_clip_t;

// Start listening and speaking. slot: this tile's place in the wall (0..8, or -1 if unplaced).
esp_err_t dust_voice_start(int slot);
// From the main loop, every frame: lifted off the wall? in someone's hand? render fps?
void dust_voice_set_context(bool lifted, bool held, float fps);
// The AI button: talk without the wake word (and interrupt Dust if he's talking).
void dust_voice_push_to_talk(void);
// While this tile answers off the wall: its own mood and voice level for the ball. False otherwise.
bool dust_voice_local(dust_state_t *state, float *talk);
void dust_voice_play_clip(dust_clip_t c);
// DUST_ERR_MIC, DUST_ERR_SPK, DUST_ERR_NO_CLIPS, DUST_ERR_NO_MODEL.
uint16_t dust_voice_errors(void);
// The mic works and there's CPU to spare for the wake word.
bool dust_voice_can_listen(void);
// Console: "voice" prints the state; "wake 20" pretends the wake word was heard at 20 dB.
void dust_voice_print_status(void);
void dust_voice_fake_wake(float score_db);
