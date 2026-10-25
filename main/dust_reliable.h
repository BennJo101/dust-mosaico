// Commands that arrive exactly once — portable C (tested on the desktop in host/net_test.c).
//
// ESP-NOW broadcasts aren't acknowledged, so a command can simply vanish. Each sender numbers
// its commands; the leader acknowledges every one it hears, and the sender repeats a command
// until it is acknowledged or it gives up. Receivers drop repeats they've already applied.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REL_SENDERS   8       // senders remembered at once (tiles, the bridge)
#define REL_FORGET_MS 30000   // a sender silent this long is forgotten (it may have restarted)
#define REL_PENDING   8       // commands one sender can have waiting for an ack
#define REL_RESEND_MS 80
#define REL_TRIES     10      // about 0.8 s, then give up
#define REL_PKT_MAX   64

/* ---------- receiving: drop repeats ---------- */
typedef struct { uint32_t sender; uint16_t top; uint32_t bits; int64_t used_ms; } rel_window_t;
typedef struct { rel_window_t w[REL_SENDERS]; } rel_dedupe_t;

void rel_dedupe_init(rel_dedupe_t *d);
// True the first time (sender, seq) arrives, false for a repeat (or one too old to tell).
// Remembers the last 32 numbers per sender.
bool rel_first(rel_dedupe_t *d, uint32_t sender, uint16_t seq, int64_t now_ms);

/* ---------- sending: repeat until acknowledged ---------- */
typedef struct { uint8_t pkt[REL_PKT_MAX]; uint8_t len, tries; uint16_t seq; int64_t next_ms; bool used; } rel_item_t;
typedef struct { rel_item_t q[REL_PENDING]; uint16_t next_seq; uint32_t lost, resent; } rel_out_t;

void rel_out_init(rel_out_t *o, uint16_t first_seq);   // start from a random number each boot
uint16_t rel_next_seq(rel_out_t *o);
// Keep a sent packet for repeating. If the queue is full the oldest is dropped (and counted lost).
void rel_track(rel_out_t *o, const void *pkt, int len, uint16_t seq, int64_t now_ms);
void rel_acked(rel_out_t *o, uint16_t seq);
// Repeat whatever is due. Returns how many commands were given up on in this call.
int rel_poll(rel_out_t *o, int64_t now_ms, void (*send)(const void *pkt, int len, void *ctx), void *ctx);
int rel_waiting(const rel_out_t *o);

#ifdef __cplusplus
}
#endif
