#include "dust_arb.h"
#include <string.h>

void arb_init(arb_t *a, uint32_t uid) { memset(a, 0, sizeof *a); a->uid = uid; a->quiet_until = -100000; }

// A group's bid: whoever owns the conversation gets a head start.
static float worth(const arb_t *a, int64_t now, uint32_t group, float mean)
{
    if (a->owner && now < a->session_until && group == a->session_group) return mean + ARB_STEAL_DB;
    if (a->peer_owner && now < a->peer_owner_until && group == a->peer_group) return mean + ARB_STEAL_DB;
    return mean;
}

bool arb_wake(arb_t *a, int64_t now_ms, float score, bool docked)
{
    if (now_ms < a->quiet_until || a->deciding) return false;
    a->deciding = true; a->open_ms = now_ms; a->my_score = score; a->my_group = arb_group(a->uid, docked);
    a->awaiting = false; a->taken = false;
    return true;
}

arb_event_t arb_claim(arb_t *a, int64_t now_ms, uint32_t uid, float score, bool docked, bool taking, float group_score)
{
    if (uid == a->uid) return ARB_EV_NONE;
    uint32_t group = arb_group(uid, docked);
    if (!taking) {
        a->claims[a->next] = (arb_claim_t){ uid, group, score, now_ms };
        a->next = (a->next + 1) % ARB_CLAIMS;
        return ARB_EV_NONE;
    }
    a->taken = true;
    // Someone decided it answers. If we think we won the same round, one of us missed bids the
    // other heard: between groups the better group keeps it, inside one the louder tile does.
    if (a->owner && now_ms - a->won_ms < ARB_TAKE_MS) {
        float mine = group == a->session_group ? a->my_score : a->session_score;
        float theirs = group == a->session_group ? score : group_score;
        if (theirs < mine || (theirs == mine && uid > a->uid)) return ARB_EV_NONE;   // they'll stand down
        a->owner = false;
        a->peer_owner = uid; a->peer_group = group; a->peer_owner_score = group_score; a->peer_owner_until = now_ms + ARB_SESSION_MS;
        return ARB_EV_YIELD;
    }
    if (a->deciding) return ARB_EV_NONE;   // our own bid decides; a head start only counts from rounds we saw it win
    a->owner = false;
    a->peer_owner = uid; a->peer_group = group; a->peer_owner_score = group_score; a->peer_owner_until = now_ms + ARB_SESSION_MS;
    return ARB_EV_NONE;
}

void arb_quiet(arb_t *a, int64_t until_ms) { if (until_ms > a->quiet_until) a->quiet_until = until_ms; }

void arb_peer_session(arb_t *a, int64_t now_ms, uint32_t uid, bool docked, float score, int64_t until_ms)
{
    (void)now_ms;
    if (uid == a->uid) return;
    a->peer_owner = uid; a->peer_group = arb_group(uid, docked); a->peer_owner_score = score; a->peer_owner_until = until_ms;
}

// Every bid in this round, the latest from each tile (claims are sent three times). Ours first.
static int round_bids(const arb_t *a, int64_t now_ms, arb_claim_t *bids)
{
    int nb = 0;
    bids[nb++] = (arb_claim_t){ a->uid, a->my_group, a->my_score, now_ms };
    for (int i = 0; i < ARB_CLAIMS; i++) {
        const arb_claim_t *c = &a->claims[i];
        if (!c->uid || c->rx_ms < a->open_ms - ARB_JOIN_MS || c->rx_ms > a->open_ms + ARB_WINDOW_MS) continue;
        int k = 0; while (k < nb && bids[k].uid != c->uid) k++;
        if (k == nb) bids[nb++] = *c; else if (k > 0 && c->rx_ms >= bids[k].rx_ms) bids[k] = *c;
    }
    return nb;
}

static arb_event_t win(arb_t *a, int64_t now_ms, float group_score, uint32_t group)
{
    a->owner = true; a->won_ms = now_ms; a->session_until = now_ms + ARB_SESSION_MS;
    a->session_score = group_score; a->session_group = group;
    a->peer_owner = 0; a->awaiting = false;
    return ARB_EV_WON;
}

arb_event_t arb_poll(arb_t *a, int64_t now_ms)
{
    arb_claim_t bids[ARB_CLAIMS + 1];
    if (a->awaiting && now_ms >= a->await_until) {
        a->awaiting = false;
        if (a->taken) return ARB_EV_NONE;
        // nobody took it: the loudest single bid heard answers (worth the same to every tile that heard it)
        int nb = round_bids(a, now_ms, bids), best = 0;
        for (int i = 1; i < nb; i++) {
            float wi = worth(a, now_ms, bids[i].group, bids[i].score), wb = worth(a, now_ms, bids[best].group, bids[best].score);
            if (wi > wb || (wi == wb && bids[i].uid < bids[best].uid)) best = i;
        }
        if (best != 0) return ARB_EV_NONE;
        float sum = 0; int n = 0;
        for (int i = 0; i < nb; i++) if (bids[i].group == a->my_group) { sum += bids[i].score; n++; }
        return win(a, now_ms, sum / n, a->my_group);
    }
    if (!a->deciding || now_ms - a->open_ms < ARB_WINDOW_MS) return ARB_EV_NONE;
    a->deciding = false;
    int nb = round_bids(a, now_ms, bids);
    // groups: the wall's tiles average, a lifted tile stands alone
    struct { uint32_t group, best_uid; float sum, best; int n; } g[ARB_CLAIMS + 1]; int ng = 0;
    for (int i = 0; i < nb; i++) {
        int k = 0; while (k < ng && g[k].group != bids[i].group) k++;
        if (k == ng) { g[ng].group = bids[i].group; g[ng].sum = 0; g[ng].n = 0; g[ng].best_uid = bids[i].uid; g[ng].best = bids[i].score; ng++; }
        g[k].sum += bids[i].score; g[k].n++;
        if (bids[i].score > g[k].best || (bids[i].score == g[k].best && bids[i].uid < g[k].best_uid)) { g[k].best = bids[i].score; g[k].best_uid = bids[i].uid; }
    }
    int w = 0;
    for (int k = 1; k < ng; k++) {
        float wk = worth(a, now_ms, g[k].group, g[k].sum / g[k].n), ww = worth(a, now_ms, g[w].group, g[w].sum / g[w].n);
        if (wk > ww || (wk == ww && g[k].best_uid < g[w].best_uid)) w = k;   // ties to the lower uid: every tile picks the same
    }
    if (g[w].best_uid != a->uid) {
        a->owner = false;
        a->awaiting = true; a->await_until = now_ms + ARB_SILENT_MS;
        return ARB_EV_LOST;
    }
    return win(a, now_ms, g[w].sum / g[w].n, g[w].group);
}

void arb_touch(arb_t *a, int64_t now_ms) { if (a->owner) a->session_until = now_ms + ARB_SESSION_MS; }
void arb_end(arb_t *a) { a->owner = false; }
bool arb_owner(const arb_t *a, int64_t now_ms) { return a->owner && now_ms < a->session_until; }
