// Wall agreement — portable C (tested on the desktop in host/net_test.c).
//
// Who is part of the wall, who leads it, whose picture to follow and who listens for the wake
// word. Every tile runs these same rules over the same hellos, so they all reach the same
// answer without voting. Leaders are numbered by epoch: a tile that takes over raises it, and a
// leader that hears a newer epoch steps down, so a tile that froze or wandered off can't come
// back and drag the wall to an old picture.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONS_MAX_TILES   16
#define CONS_ALIVE_MS    600    // a tile not heard for this long has gone
#define CONS_SYNC_LOST_MS 400   // no picture from the followed leader for this long: follow anyone
#define CONS_HOLD_MS     300    // a leader that stepped down waits this long before leading again

// Errors a tile reports in its hello (bits of dust_health_t.err).
enum {
    DUST_ERR_DUP_SLOT  = 1 << 0,   // another tile has the same slot: this one stood aside
    DUST_ERR_FW        = 1 << 1,   // heard a tile running firmware this one can't talk to
    DUST_ERR_MIC       = 1 << 2,
    DUST_ERR_SPK       = 1 << 3,
    DUST_ERR_MAG       = 1 << 4,   // magnetometers missing: no neighbour sensing
    DUST_ERR_IMU       = 1 << 5,
    DUST_ERR_CMD_LOST  = 1 << 6,   // a command it sent was never acknowledged
    DUST_ERR_HOT       = 1 << 7,
    DUST_ERR_LOW_BATT  = 1 << 8,
    DUST_ERR_CRASHED   = 1 << 9,   // last restart was a watchdog, panic or brownout
    DUST_ERR_NO_CLIPS  = 1 << 10,  // no recorded voice lines flashed
    DUST_ERR_NO_MODEL  = 1 << 11,  // no speech models flashed: can't hear the wake word
};
// Errors that make a tile a poor leader (it still shows its slice).
#define DUST_ERR_NO_LEAD (DUST_ERR_HOT | DUST_ERR_LOW_BATT)

// Health a tile reports in every hello.
typedef struct {
    uint16_t fps_x10;     // render frames per second * 10
    uint16_t heap_kb;     // free internal heap
    uint8_t  battery;     // 0..100 %, 255 = unknown
    int8_t   temp_c;      // from the battery gauge, -128 = unknown
    uint16_t err;         // DUST_ERR_*
    uint8_t  reset;       // why it last restarted (esp_reset_reason_t)
    uint8_t  can_listen;  // 1: mic works and there is CPU to spare for the wake word
} dust_health_t;

// One tile as the others know it (from its hellos). Entry 0 is always this tile.
typedef struct {
    uint32_t uid;
    uint8_t  slot;        // 0..8, 255 = not placed yet
    bool     lifted, setup;
    dust_health_t health;
    int64_t  seen_ms;
} cons_tile_t;

typedef struct {
    uint16_t present;     // slots that make up the wall: alive, docked, placed, not a duplicate
    int      leader;      // slot that leads, -1 if no wall
    uint16_t ears;        // docked slots that listen for the wake word
    bool     self_dup;    // this tile lost a slot clash: it must not lead or draw a slice
} cons_view_t;

// Everything that follows from the tiles a tile can hear. max_ears: how many docked tiles listen
// at once (9 = all that can).
cons_view_t cons_view(const cons_tile_t *tiles, int n, int64_t now_ms, int max_ears);

// Following a leader's picture.
typedef struct {
    uint32_t seen;        // highest epoch this tile has heard of
    uint32_t epoch;       // epoch of the leader it follows (its own, while it leads)
    uint8_t  slot;        // slot of that leader
    int64_t  last_ms;     // last sync taken
    int64_t  hold_until;  // stepped down: don't lead before this
} cons_follow_t;

typedef enum { CONS_SYNC_IGNORE = 0, CONS_SYNC_TAKE, CONS_SYNC_STEP_DOWN } cons_sync_t;

void cons_follow_init(cons_follow_t *f);
// A sync arrived from (epoch, slot). Returns whether to take it; STEP_DOWN means this tile was
// leading and must stop (and take the sync).
cons_sync_t cons_on_sync(cons_follow_t *f, uint32_t epoch, uint8_t slot, bool am_leader, uint8_t my_slot, int64_t now_ms);
// Any epoch heard (hellos carry the epoch each tile follows).
void cons_saw_epoch(cons_follow_t *f, uint32_t epoch);
// This tile starts leading: the epoch to stamp on its syncs.
uint32_t cons_take_lead(cons_follow_t *f, uint8_t my_slot, int64_t now_ms);
// May this tile lead right now (not holding off after a step-down)?
bool cons_may_lead(const cons_follow_t *f, int64_t now_ms);

#ifdef __cplusplus
}
#endif
