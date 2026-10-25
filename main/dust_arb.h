// Who answers — portable C (tested on the desktop in host/net_test.c, in a simulated room).
//
// When someone says the wake word, every tile that heard it bids how close the voice sounds
// (its level above that tile's own noise floor). The tiles on the wall bid as one group, with
// the average of their levels: they sit centimetres apart, so averaging takes out each mic's
// jitter instead of rewarding whichever was noisiest. Each lifted tile bids as a group of its
// own. After a short window every tile works out the same answer from the bids it heard: the
// closest group answers, through its loudest tile. The winner then says "taking" so a tile
// which missed a better bid finds out and stands down. The group answering keeps the
// conversation for a while: another needs a clearly closer bid to take over.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARB_WINDOW_MS     200    // collect bids this long after hearing the wake word
#define ARB_JOIN_MS       400    // bids heard this long before our own wake still count (same words, faster tile)
#define ARB_TAKE_MS       1500   // a "taking" from a better bid this soon after winning: stand down
#define ARB_SILENT_MS     250    // lost, but nobody said "taking" this long after: decide again, plainly
#define ARB_SESSION_MS    8000   // the group answering keeps the conversation this long after it last spoke
#define ARB_STEAL_DB      6.0f   // a group must beat the conversation's owner by this much to take over
#define ARB_HELD_DB       3.0f   // bonus for a tile in someone's hand: it's next to their face
#define ARB_BUTTON_DB     40.0f  // bonus for a tile whose AI button was pressed: it was touched
#define ARB_CLAIMS        24
#define ARB_WALL          0u     // group id of the wall; a lifted tile's group is its own uid

typedef struct { uint32_t uid, group; float score; int64_t rx_ms; } arb_claim_t;

typedef enum { ARB_EV_NONE = 0, ARB_EV_WON, ARB_EV_LOST, ARB_EV_YIELD } arb_event_t;

typedef struct {
    uint32_t uid;
    bool     deciding;        // a bid of ours is open
    int64_t  open_ms;
    float    my_score;
    uint32_t my_group;
    arb_claim_t claims[ARB_CLAIMS]; int next;
    // lost a round: if nobody takes it (they each heard different bids and each picked another
    // group), fall back to the loudest single bid heard, so the round never goes unanswered
    bool     awaiting; int64_t await_until; bool taken;
    int64_t  quiet_until;     // someone is speaking (or just was): wake words are echoes
    // the conversation
    bool     owner; int64_t won_ms, session_until;
    float    session_score;   // the winning group's bid
    uint32_t session_group;
    uint32_t peer_owner, peer_group; float peer_owner_score; int64_t peer_owner_until;
} arb_t;

void arb_init(arb_t *a, uint32_t uid);
static inline uint32_t arb_group(uint32_t uid, bool docked) { return docked ? ARB_WALL : uid; }
// This tile heard the wake word with this score (dB above its floor, plus any bonus). Returns
// true if it bids: broadcast a claim with the score.
bool arb_wake(arb_t *a, int64_t now_ms, float score, bool docked);
// A claim from another tile: its own bid, and with taking=true (it has decided it answers) its
// group's bid too. Returns ARB_EV_YIELD if this tile was answering and that one outranks it.
arb_event_t arb_claim(arb_t *a, int64_t now_ms, uint32_t uid, float score, bool docked, bool taking, float group_score);
// Speech is playing somewhere (here or on another tile) until this time.
void arb_quiet(arb_t *a, int64_t until_ms);
// Another tile says (in its hello) it owns the conversation.
void arb_peer_session(arb_t *a, int64_t now_ms, uint32_t uid, bool docked, float score, int64_t until_ms);
// Call often. Returns ARB_EV_WON once if this tile answers (broadcast a "taking" claim with
// session_score), ARB_EV_LOST once if another does.
arb_event_t arb_poll(arb_t *a, int64_t now_ms);
// This tile is answering: keep the conversation going / give it up.
void arb_touch(arb_t *a, int64_t now_ms);
void arb_end(arb_t *a);
bool arb_owner(const arb_t *a, int64_t now_ms);

#ifdef __cplusplus
}
#endif
