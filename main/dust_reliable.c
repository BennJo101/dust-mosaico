#include "dust_reliable.h"
#include <string.h>

void rel_dedupe_init(rel_dedupe_t *d) { memset(d, 0, sizeof *d); }

bool rel_first(rel_dedupe_t *d, uint32_t sender, uint16_t seq, int64_t now_ms)
{
    rel_window_t *w = NULL, *spare = NULL;
    for (int i = 0; i < REL_SENDERS; i++) {
        rel_window_t *c = &d->w[i];
        if (c->sender == sender && now_ms - c->used_ms < REL_FORGET_MS) { w = c; break; }
        if (!spare || c->sender == 0 || c->used_ms < spare->used_ms) spare = c;   // empty, else least recently used
    }
    if (!w) {   // a sender we don't know (or forgot): this is its first
        *spare = (rel_window_t){ sender, seq, 1, now_ms };
        return true;
    }
    w->used_ms = now_ms;
    int16_t ahead = (int16_t)(seq - w->top);   // wraps cleanly at 65535
    if (ahead > 0) {
        w->bits = ahead >= 32 ? 1 : (w->bits << ahead) | 1;
        w->top = seq;
        return true;
    }
    int back = -ahead;
    if (back >= 32) return false;
    uint32_t bit = 1u << back;
    if (w->bits & bit) return false;
    w->bits |= bit;
    return true;
}

void rel_out_init(rel_out_t *o, uint16_t first_seq) { memset(o, 0, sizeof *o); o->next_seq = first_seq ? first_seq : 1; }

uint16_t rel_next_seq(rel_out_t *o) { uint16_t s = o->next_seq++; if (!o->next_seq) o->next_seq = 1; return s; }

void rel_track(rel_out_t *o, const void *pkt, int len, uint16_t seq, int64_t now_ms)
{
    if (len > REL_PKT_MAX) return;
    rel_item_t *slot = NULL;
    for (int i = 0; i < REL_PENDING; i++) if (!o->q[i].used) { slot = &o->q[i]; break; }
    if (!slot) {   // full: the one closest to giving up goes
        slot = &o->q[0];
        for (int i = 1; i < REL_PENDING; i++) if (o->q[i].tries > slot->tries) slot = &o->q[i];
        o->lost++;
    }
    memcpy(slot->pkt, pkt, (size_t)len);
    slot->len = (uint8_t)len; slot->seq = seq; slot->tries = 1; slot->next_ms = now_ms + REL_RESEND_MS; slot->used = true;
}

void rel_acked(rel_out_t *o, uint16_t seq)
{
    for (int i = 0; i < REL_PENDING; i++) if (o->q[i].used && o->q[i].seq == seq) o->q[i].used = false;
}

int rel_poll(rel_out_t *o, int64_t now_ms, void (*send)(const void *pkt, int len, void *ctx), void *ctx)
{
    int gave_up = 0;
    for (int i = 0; i < REL_PENDING; i++) {
        rel_item_t *it = &o->q[i];
        if (!it->used || now_ms < it->next_ms) continue;
        if (it->tries >= REL_TRIES) { it->used = false; o->lost++; gave_up++; continue; }
        send(it->pkt, it->len, ctx);
        it->tries++; it->next_ms = now_ms + REL_RESEND_MS; o->resent++;
    }
    return gave_up;
}

int rel_waiting(const rel_out_t *o) { int n = 0; for (int i = 0; i < REL_PENDING; i++) n += o->q[i].used; return n; }
