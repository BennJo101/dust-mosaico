#include "dust_consensus.h"
#include <string.h>

cons_view_t cons_view(const cons_tile_t *tiles, int n, int64_t now_ms, int max_ears)
{
    cons_view_t v = { .leader = -1 };
    int owner[9];   // index of the tile that holds each slot
    for (int s = 0; s < 9; s++) owner[s] = -1;
    for (int i = 0; i < n; i++) {
        const cons_tile_t *t = &tiles[i];
        bool alive = i == 0 || now_ms - t->seen_ms < CONS_ALIVE_MS;
        if (!alive || t->slot >= 9 || t->lifted || t->setup || !t->uid) continue;
        // two tiles on one slot: the lower uid keeps it, the other stands aside and says so
        int o = owner[t->slot];
        if (o < 0 || t->uid < tiles[o].uid) owner[t->slot] = i;
    }
    for (int i = 1; i < n && tiles[0].slot < 9 && !tiles[0].lifted && !tiles[0].setup; i++) {
        const cons_tile_t *t = &tiles[i];
        if (now_ms - t->seen_ms < CONS_ALIVE_MS && t->slot == tiles[0].slot && !t->lifted && !t->setup && t->uid < tiles[0].uid) v.self_dup = true;
    }
    for (int s = 0; s < 9; s++) if (owner[s] >= 0) v.present |= 1u << s;

    // lowest slot leads, passing over tiles too hot or too flat to be trusted with it
    for (int s = 0; s < 9 && v.leader < 0; s++)
        if (owner[s] >= 0 && !(tiles[owner[s]].health.err & DUST_ERR_NO_LEAD)) v.leader = s;
    for (int s = 0; s < 9 && v.leader < 0; s++) if (owner[s] >= 0) v.leader = s;

    // ears: tiles that can listen, spread out (centre and corners first), sparing the leader
    // when there are enough others
    static const int ORDER[9] = { 4, 0, 2, 6, 8, 1, 3, 5, 7 };
    int able = 0;
    for (int s = 0; s < 9; s++) if (owner[s] >= 0 && tiles[owner[s]].health.can_listen && s != v.leader) able++;
    bool spare_leader = able >= 3;
    int ears = 0;
    for (int k = 0; k < 9 && ears < max_ears; k++) {
        int s = ORDER[k];
        if (owner[s] < 0 || !tiles[owner[s]].health.can_listen) continue;
        if (s == v.leader && spare_leader) continue;
        v.ears |= 1u << s; ears++;
    }
    return v;
}

void cons_follow_init(cons_follow_t *f) { memset(f, 0, sizeof *f); f->slot = 255; f->last_ms = -100000; f->hold_until = -100000; }

void cons_saw_epoch(cons_follow_t *f, uint32_t epoch) { if (epoch > f->seen) f->seen = epoch; }

// (e1, s1) outranks (e2, s2): newer epoch, or the same epoch from a lower slot
static bool outranks(uint32_t e1, uint8_t s1, uint32_t e2, uint8_t s2) { return e1 > e2 || (e1 == e2 && s1 < s2); }

cons_sync_t cons_on_sync(cons_follow_t *f, uint32_t epoch, uint8_t slot, bool am_leader, uint8_t my_slot, int64_t now_ms)
{
    cons_saw_epoch(f, epoch);
    if (am_leader) {
        if (!outranks(epoch, slot, f->epoch, my_slot)) return CONS_SYNC_IGNORE;
        f->epoch = epoch; f->slot = slot; f->last_ms = now_ms; f->hold_until = now_ms + CONS_HOLD_MS;
        return CONS_SYNC_STEP_DOWN;
    }
    bool same = epoch == f->epoch && slot == f->slot;
    bool stale = now_ms - f->last_ms > CONS_SYNC_LOST_MS;
    // an old leader that hasn't noticed it was replaced is ignored, unless nothing better is heard
    if (!same && !stale && !outranks(epoch, slot, f->epoch, f->slot)) return CONS_SYNC_IGNORE;
    f->epoch = epoch; f->slot = slot; f->last_ms = now_ms;
    return CONS_SYNC_TAKE;
}

uint32_t cons_take_lead(cons_follow_t *f, uint8_t my_slot, int64_t now_ms)
{
    f->seen += 1;   // newer than anything this tile has heard of
    f->epoch = f->seen; f->slot = my_slot; f->last_ms = now_ms;
    return f->epoch;
}

bool cons_may_lead(const cons_follow_t *f, int64_t now_ms) { return now_ms >= f->hold_until; }
