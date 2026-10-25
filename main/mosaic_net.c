#include "mosaic_net.h"
#include "dust_layout.h"
#include "dust_topo.h"
#include "dust_reliable.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

static const char *TAG = "mosaic";
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define HELLO_MS     100
#define SYNC_MS      50
#define BRAIN_MS     1500    // no beacon from the Jetson for this long: it's gone
#define FW_WARN_MS   10000   // how long a heard firmware mismatch stays reported
#define UV_Q         16384.f
#ifndef CONFIG_DUST_WALL_EARS
#define CONFIG_DUST_WALL_EARS 9
#endif

typedef struct { float x, v, t; } spring_t;
typedef struct { dust_fx_t fx; float t0, dur, env, strength, vel; } fx_slot_t;

static SemaphoreHandle_t s_lock;
static uint8_t  s_slot;
static float    s_gap;
static bool     s_lifted, s_setup;

// every tile this one can hear (entry 0 is this tile), and the agreement drawn from them
static cons_tile_t s_tiles[CONS_MAX_TILES];
static uint8_t  s_tile_sides[CONS_MAX_TILES];
static int16_t  s_tile_mic[CONS_MAX_TILES];
static cons_view_t   s_view;
static cons_follow_t s_follow;

// clock: wall time = local ms + offset
static int64_t  s_offset_ms;

static wall_snapshot_t s_snap;

// neighbour sensing / automatic layout
static uint32_t s_uid;
static uint8_t  s_sides, s_rot;
static int      s_assigned = -1;
static int64_t  s_last_layout_ms = -100000;

// commands: numbered, acknowledged by the leader, repeats dropped
static rel_out_t    s_out;
static rel_dedupe_t s_seen;
static int64_t  s_cmd_lost_ms = -100000;
static void   (*s_cal_mic_cb)(void);

// firmware versions heard that this tile can't talk to
static int64_t  s_fw_older_ms = -100000, s_fw_newer_ms = -100000;

// voice
static mosaic_voice_rx_t s_voice_rx;
static bool     s_speaking, s_session; static float s_session_score; static int s_session_left_ms;
static int16_t  s_mic_cal_x10;
static uint8_t  s_brain_mac[6], s_brain_caps; static int64_t s_brain_ms = -100000; static bool s_brain_new;

/* ---------- leader-side physics ---------- */
static dust_state_t s_state = DUST_IDLE;
static float    s_ext_talk; static int64_t s_ext_talk_ms = -100000;
static uint16_t s_prev_gone = 0xFFFF;
static spring_t s_frame[4], s_pres[9];
static bool     s_springs_init;
static float    s_form_amt_target;
static fx_slot_t s_fx[DUST_MAX_FX];
static float    s_scatter_left;
static spring_t s_warp_k;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static float wall_seconds(int64_t t) { return (t + s_offset_ms) / 1000.f; }
static void step_spring(spring_t *s, float dt, float k, float c) { s->v += (s->t - s->x) * k * dt; s->v *= expf(-c * dt); s->x += s->v * dt; }
static bool alive(int i, int64_t t) { return i == 0 || (s_tiles[i].uid && t - s_tiles[i].seen_ms < CONS_ALIVE_MS); }

void mosaic_net_head(pkt_head_t *h, uint8_t type)
{
    *h = (pkt_head_t){ DUST_MAGIC, DUST_PROTO, type, s_slot,
                       (uint8_t)((s_lifted ? HEAD_LIFTED : 0) | (s_setup ? HEAD_SETUP : 0) | (s_speaking ? HEAD_SPEAKING : 0) | (s_session ? HEAD_SESSION : 0)) };
}

/* ---------- commands (applied by whoever leads) ---------- */
static void apply_cmd(const pkt_cmd_t *c, int64_t t)
{
    dust_view_t *v = &s_snap.view;
    float now = wall_seconds(t);
    switch (c->op) {
    case CMD_STATE: if (c->a <= DUST_SPEAKING) s_state = (dust_state_t)c->a; break;
    case CMD_TALK:  s_ext_talk = c->a / 255.f; s_ext_talk_ms = t; break;
    case CMD_FORM: {
        if (c->a >= DUST_FORM_COUNT) break;
        if (c->a == DUST_FORM_BODY) { s_form_amt_target = 0; break; }
        dust_form_spec_t nf = { .id = c->a };
        memcpy(nf.text, c->text, DUST_TEXT_MAX); nf.text[DUST_TEXT_MAX] = 0;
        if (v->form_amt < 0.05f) { v->form_a = nf; v->form_b = nf; v->form_swap = 1.2f; }
        else { if (v->form_swap > 0.75f) v->form_a = v->form_b; v->form_b = nf; v->form_swap = 0; }
        s_form_amt_target = 1;
        break; }
    case CMD_FX: {
        if (c->a < DUST_FX_FLOW || c->a > DUST_FX_PULSE) break;
        // effects stack: a new one never cuts an old one off. Older ones of the same kind start
        // easing out, and only an almost-faded one gives up its slot.
        for (int k = 0; k < DUST_MAX_FX; k++)
            if (s_fx[k].fx.type == c->a && c->a != DUST_FX_PULSE && s_fx[k].dur > now - s_fx[k].t0) s_fx[k].dur = now - s_fx[k].t0;
        int slot = -1;
        for (int k = 0; k < DUST_MAX_FX; k++) if (s_fx[k].fx.type == DUST_FX_NONE) { slot = k; break; }
        if (slot < 0) { slot = 0; for (int k = 1; k < DUST_MAX_FX; k++) if (s_fx[k].fx.release > s_fx[slot].fx.release) slot = k; }   // closest to home gives up its slot
        fx_slot_t *f = &s_fx[slot];
        memset(f, 0, sizeof *f);
        f->fx.type = c->a; f->fx.x = c->x / UV_Q; f->fx.y = c->y / UV_Q; f->fx.dx = c->dx / 127.f; f->fx.dy = c->dy / 127.f;
        f->fx.radius = c->radius / 255.f; f->strength = c->strength / 255.f;
        f->t0 = now; f->dur = c->duration / 10.f;
        break; }
    case CMD_SCATTER: s_scatter_left = 1.6f; v->shake = fmaxf(v->shake, 0.5f); break;
    case CMD_CALM:   // everything eases out, nothing snaps
        for (int k = 0; k < DUST_MAX_FX; k++) if (s_fx[k].dur > now - s_fx[k].t0) s_fx[k].dur = now - s_fx[k].t0;
        break;
    case CMD_CAL_MIC: if (s_cal_mic_cb) s_cal_mic_cb(); break;
    }
}

static void lead_step(int64_t t, float dt, uint16_t present)
{
    dust_view_t *v = &s_snap.view;
    uint16_t lifted = 0;
    for (int i = 0; i < CONS_MAX_TILES; i++)
        if (alive(i, t) && s_tiles[i].slot < 9 && (i == 0 ? s_lifted : s_tiles[i].lifted)) lifted |= 1u << s_tiles[i].slot;
    uint16_t gone = (uint16_t)(0x1FF & ~present);
    dust_frame_t f = dust_fit(present, s_gap);

    if (!s_springs_init || !s_snap.have_sync) {   // first leader on a fresh wall: start the pour
        s_springs_init = true; s_snap.have_sync = true; v->assemble = 0;
        float init[4] = { f.cx, f.cy, f.sx, f.sy };
        for (int k = 0; k < 4; k++) s_frame[k] = (spring_t){ init[k], 0, init[k] };
        for (int i = 0; i < 9; i++) s_pres[i] = (spring_t){ (present >> i & 1) ? 1.f : 0.f, 0, 0 };
    }
    if (gone != s_prev_gone && s_prev_gone != 0xFFFF) {
        v->shake = 1;
        // bend from wherever the field is now toward the new screen set
        if (s_warp_k.x > 0.5f) s_snap.warp_from = s_snap.warp_to;
        s_snap.warp_to = (uint16_t)(0x1FF & ~gone);
        s_warp_k = (spring_t){ 0, 0, 1 };
    }
    if (s_prev_gone == 0xFFFF) { s_snap.warp_from = s_snap.warp_to = present; s_warp_k = (spring_t){ 1, 0, 1 }; }
    s_prev_gone = gone;
    step_spring(&s_warp_k, dt, 16, 5.5f);
    s_snap.warp_k = s_warp_k.x;

    // the fluid: springs give the dust a slosh when the wall changes shape
    if (f.valid) { s_frame[0].t = f.cx; s_frame[1].t = f.cy; s_frame[2].t = f.sx; s_frame[3].t = f.sy; }
    for (int k = 0; k < 4; k++) step_spring(&s_frame[k], dt, 16, 5.5f);
    for (int i = 0; i < 9; i++) { s_pres[i].t = (present >> i & 1) ? 1.f : 0.f; step_spring(&s_pres[i], dt, 22, 6.5f); }
    v->frame_cx = s_frame[0].x; v->frame_cy = s_frame[1].x; v->frame_sx = fmaxf(.05f, s_frame[2].x); v->frame_sy = fmaxf(.05f, s_frame[3].x);
    for (int i = 0; i < 9; i++) v->pres[i] = fminf(1, fmaxf(0, s_pres[i].x));

    float up = 1 - expf(-dt * 1.4f), down = 1 - expf(-dt * 1.8f);
    for (int i = 0; i < 9; i++) { float tg = (gone >> i & 1) ? 1.f : 0.f; v->take[i] += (tg - v->take[i]) * (tg > v->take[i] ? up : down); }
    v->shake *= expf(-dt * 2.4f);
    if (v->assemble < 1.2f) v->assemble = fminf(1.2f, v->assemble + dt / 3.2f);

    // shapes
    v->form_amt += (s_form_amt_target - v->form_amt) * (1 - expf(-dt * 1.6f));
    if (s_form_amt_target == 0 && v->form_amt < 0.002f) v->form_amt = 0;
    v->form_swap = fminf(1.2f, v->form_swap + dt / 1.4f);

    // the AI's effects: smooth attack, hold, release
    float now = wall_seconds(t);
    for (int k = 0; k < DUST_MAX_FX; k++) {
        fx_slot_t *s = &s_fx[k];
        if (s->fx.type == DUST_FX_NONE) { v->fx[k].type = DUST_FX_NONE; v->fx[k].amount = 0; continue; }
        float age = now - s->t0; bool held = age < s->dur;
        if (held) s->env += (1 - s->env) * (1 - expf(-dt * 2.2f));
        // a swirl has momentum: held, it spins up; released, it coasts down instead of stopping dead
        if (s->fx.type == DUST_FX_SWIRL) { s->vel = held ? 3.2f * s->env : s->vel * expf(-dt * 1.1f); s->fx.phase += dt * s->vel; }
        if (s->fx.type == DUST_FX_PULSE) s->fx.phase = age;
        // release: the drift home fades in gently (no hard start), then carries every dot back
        if (!held && s->fx.type != DUST_FX_PULSE) {
            float ramp = fminf(1, (age - s->dur) / 1.6f);
            s->fx.release = fminf(1.25f, s->fx.release + dt / 3 * ramp * ramp);
        }
        s->fx.amount = s->fx.type == DUST_FX_PULSE ? s->strength : s->strength * s->env;
        bool done = s->fx.type == DUST_FX_PULSE ? age > s->dur + 1.5f : s->fx.release >= 1.25f;
        if (done) s->fx.type = DUST_FX_NONE;
        v->fx[k] = s->fx;
    }
    if (s_scatter_left > 0) s_scatter_left -= dt;
    v->scatter += ((s_scatter_left > 0 ? 1.f : 0.f) - v->scatter) * (1 - expf(-dt * (s_scatter_left > 0 ? 2.2f : 1.5f)));

    // voice
    float wt = wall_seconds(t);
    if (t - s_ext_talk_ms < 250) v->talk = s_ext_talk;
    else if (s_state == DUST_SPEAKING) {
        float e = (0.55f + 0.45f * sinf(wt * 10.3f)) * (0.55f + 0.45f * sinf(wt * 3.7f + 1.2f)) * (sinf(wt * 1.4f) > -0.55f ? 1 : 0.1f);
        v->talk += (fmaxf(0, e) - v->talk) * (1 - expf(-dt * 14));
    } else v->talk *= expf(-dt * 8);
    s_snap.present = present; s_snap.lifted = lifted; s_snap.state = s_state;

    // broadcast
    pkt_sync_t p; memset(&p, 0, sizeof p);
    mosaic_net_head(&p.h, PKT_SYNC);
    p.wall_ms = (uint32_t)(t + s_offset_ms);
    p.epoch = s_follow.epoch;
    p.present = present; p.lifted = lifted; p.state = (uint8_t)s_state;
    p.talk = (uint8_t)(fminf(1, v->talk) * 255); p.assemble = (uint8_t)(v->assemble / 1.2f * 255);
    p.shake = (uint8_t)(fminf(1, v->shake) * 255); p.scatter = (uint8_t)(fminf(1, v->scatter) * 255);
    for (int i = 0; i < 9; i++) { p.take[i] = (uint8_t)(v->take[i] * 255); p.pres[i] = (uint8_t)(v->pres[i] * 255); }
    p.frame_cx = (uint16_t)(v->frame_cx * UV_Q); p.frame_cy = (uint16_t)(v->frame_cy * UV_Q);
    p.frame_sx = (uint16_t)(v->frame_sx * UV_Q); p.frame_sy = (uint16_t)(v->frame_sy * UV_Q);
    p.form_a = v->form_a.id; p.form_b = v->form_b.id;
    p.form_amt = (uint8_t)(fminf(1, v->form_amt) * 255); p.form_swap = (uint8_t)(v->form_swap / 1.2f * 255);
    memcpy(p.text_a, v->form_a.text, DUST_TEXT_MAX); memcpy(p.text_b, v->form_b.text, DUST_TEXT_MAX);
    p.warp_from = s_snap.warp_from; p.warp_to = s_snap.warp_to; p.warp_k = (int16_t)(fminf(3, fmaxf(-3, s_snap.warp_k)) * 10000);
    for (int k = 0; k < DUST_MAX_FX; k++) {
        const dust_fx_t *f = &v->fx[k];
        p.fx[k] = (pkt_fx_t){ f->type, (uint8_t)(fminf(1, f->amount) * 255), (uint8_t)(fminf(1, f->radius) * 255),
                              (uint8_t)(fminf(1.25f, f->release) / 1.25f * 255),
                              (int16_t)(f->x * UV_Q), (int16_t)(f->y * UV_Q), (int8_t)(f->dx * 127), (int8_t)(f->dy * 127), f->phase };
    }
    esp_now_send(BCAST, (const uint8_t *)&p, sizeof p);
}

/* ---------- radio ---------- */
static void send_hello(void)
{
    const dust_health_t *hh = &s_tiles[0].health;
    pkt_hello_t p = { .uid = s_uid, .sides = s_sides, .rot = s_rot, .epoch = s_follow.epoch,
                      .fps_x10 = hh->fps_x10, .heap_kb = hh->heap_kb, .battery = hh->battery, .temp_c = hh->temp_c,
                      .err = (uint16_t)(hh->err | mosaic_net_errors()), .reset = hh->reset, .can_listen = hh->can_listen,
                      .session_x10 = (int16_t)(s_session_score * 10), .session_left = (uint8_t)(s_session_left_ms < 0 ? 0 : s_session_left_ms > 25500 ? 255 : s_session_left_ms / 100),
                      .mic_cal_x10 = s_mic_cal_x10 };
    mosaic_net_head(&p.h, PKT_HELLO);
    esp_now_send(BCAST, (const uint8_t *)&p, sizeof p);
}

static void take_sync(const pkt_sync_t *p, int64_t t)
{
    static int64_t last_take = -100000;
    int64_t target = (int64_t)p->wall_ms - t;
    if (t - last_take > CONS_SYNC_LOST_MS || llabs(target - s_offset_ms) > 200) s_offset_ms = target;
    else s_offset_ms += (target - s_offset_ms) / 8;
    last_take = t;
    s_snap.have_sync = true;
    s_snap.present = p->present; s_snap.lifted = p->lifted; s_snap.state = (dust_state_t)p->state;
    dust_view_t *v = &s_snap.view;
    v->talk = p->talk / 255.f; v->assemble = p->assemble / 255.f * 1.2f; v->shake = p->shake / 255.f; v->scatter = p->scatter / 255.f;
    for (int i = 0; i < 9; i++) { v->take[i] = p->take[i] / 255.f; v->pres[i] = p->pres[i] / 255.f; }
    v->frame_cx = p->frame_cx / UV_Q; v->frame_cy = p->frame_cy / UV_Q; v->frame_sx = p->frame_sx / UV_Q; v->frame_sy = p->frame_sy / UV_Q;
    v->form_a.id = p->form_a; v->form_b.id = p->form_b;
    memcpy(v->form_a.text, p->text_a, DUST_TEXT_MAX); v->form_a.text[DUST_TEXT_MAX] = 0;
    memcpy(v->form_b.text, p->text_b, DUST_TEXT_MAX); v->form_b.text[DUST_TEXT_MAX] = 0;
    v->form_amt = p->form_amt / 255.f; v->form_swap = p->form_swap / 255.f * 1.2f;
    s_snap.warp_from = p->warp_from; s_snap.warp_to = p->warp_to; s_snap.warp_k = p->warp_k / 10000.f;
    for (int k = 0; k < DUST_MAX_FX; k++) {
        const pkt_fx_t *f = &p->fx[k];
        v->fx[k] = (dust_fx_t){ f->type, f->amount / 255.f, f->radius / 255.f, f->phase, f->x / UV_Q, f->y / UV_Q, f->dx / 127.f, f->dy / 127.f,
                                f->release / 255.f * 1.25f };
    }
}

static void note_hello(const pkt_hello_t *p, int64_t t)
{
    if (p->uid == s_uid) return;
    int at = -1, free_i = -1;
    for (int i = 1; i < CONS_MAX_TILES; i++) {
        if (s_tiles[i].uid == p->uid) { at = i; break; }
        if (free_i < 0 && (s_tiles[i].uid == 0 || t - s_tiles[i].seen_ms > 5000)) free_i = i;
    }
    if (at < 0) at = free_i;
    if (at < 0) return;   // more than 15 tiles in range
    cons_tile_t *c = &s_tiles[at];
    c->uid = p->uid; c->slot = p->h.slot; c->seen_ms = t;
    c->lifted = p->h.flags & HEAD_LIFTED; c->setup = p->h.flags & HEAD_SETUP;
    c->health = (dust_health_t){ p->fps_x10, p->heap_kb, p->battery, p->temp_c, p->err, p->reset, p->can_listen };
    s_tile_sides[at] = p->sides; s_tile_mic[at] = p->mic_cal_x10;
    cons_saw_epoch(&s_follow, p->epoch);
}

static void send_ack(const pkt_cmd_t *c)
{
    pkt_ack_t a = { .sender = c->sender, .seq = c->seq, .epoch = s_follow.epoch };
    mosaic_net_head(&a.h, PKT_ACK);
    esp_now_send(BCAST, (const uint8_t *)&a, sizeof a);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(pkt_head_t)) return;
    const pkt_head_t *h = (const pkt_head_t *)data;
    if (h->magic != DUST_MAGIC) return;
    int64_t t = now_ms();
    if (h->proto != DUST_PROTO) {   // a tile on other firmware: note which of us is out of date
        if (h->proto > DUST_PROTO) s_fw_newer_ms = t; else s_fw_older_ms = t;
        return;
    }
    bool to_voice = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (h->type) {
    case PKT_HELLO:
        if (len >= (int)sizeof(pkt_hello_t)) {
            note_hello((const pkt_hello_t *)data, t);
            to_voice = h->flags & (HEAD_SPEAKING | HEAD_SESSION);   // the voice only cares who's talking
        }
        break;
    case PKT_LAYOUT:
        if (len >= (int)sizeof(pkt_layout_t)) {
            const pkt_layout_t *p = (const pkt_layout_t *)data;
            for (int i = 0; i < 9; i++) if (p->uid[i] == s_uid && p->slot[i] < 9 && p->slot[i] != s_slot) s_assigned = p->slot[i];
        }
        break;
    case PKT_SYNC:
        if (len >= (int)sizeof(pkt_sync_t)) {   // (a tile never hears its own broadcasts)
            const pkt_sync_t *p = (const pkt_sync_t *)data;
            cons_sync_t r = cons_on_sync(&s_follow, p->epoch, h->slot, s_snap.am_leader, s_slot, t);
            if (r == CONS_SYNC_STEP_DOWN) { s_snap.am_leader = false; ESP_LOGW(TAG, "screen %d leads with a newer epoch: stepping down", h->slot + 1); }
            if (r != CONS_SYNC_IGNORE) take_sync(p, t);
        }
        break;
    case PKT_CMD:
        if (len >= (int)sizeof(pkt_cmd_t)) {
            const pkt_cmd_t *c = (const pkt_cmd_t *)data;
            bool numbered = c->cflags & CMD_F_ACK;
            if (numbered && s_snap.am_leader) send_ack(c);   // ack repeats too: the first ack may have been lost
            if (!numbered || rel_first(&s_seen, c->sender, c->seq, t)) apply_cmd(c, t);   // followers apply too, ready to take over
        }
        break;
    case PKT_ACK:
        if (len >= (int)sizeof(pkt_ack_t) && ((const pkt_ack_t *)data)->sender == s_uid) rel_acked(&s_out, ((const pkt_ack_t *)data)->seq);
        break;
    case PKT_BEACON:
        if (len >= (int)sizeof(pkt_beacon_t)) {
            if (memcmp(s_brain_mac, info->src_addr, 6)) { memcpy(s_brain_mac, info->src_addr, 6); s_brain_new = true; }
            s_brain_caps = ((const pkt_beacon_t *)data)->caps; s_brain_ms = t;
        }
        break;
    case PKT_CLAIM: case PKT_AUDIO: case PKT_VOICE:
        to_voice = true;
        break;
    }
    mosaic_voice_rx_t cb = s_voice_rx;
    xSemaphoreGive(s_lock);
    if (to_voice && cb) cb(info->src_addr, data, len);
}

// The tile with the lowest uid checks whether nine tiles now form a complete 3x3 by their
// neighbour sides. If so, each tile's slot follows from its sides; tiles whose slot is wrong or
// unknown are told, save it and restart. Runs at most every 2 s.
static void try_layout(int64_t t)
{
    if (t - s_last_layout_ms < 2000) return;
    uint32_t uids[10]; uint8_t masks[10], slots[10]; int n = 0;
    uids[n] = s_uid; masks[n] = s_sides & 0xF; slots[n] = s_slot; n++;
    for (int i = 1; i < CONS_MAX_TILES && n < 10; i++)
        if (alive(i, t)) { uids[n] = s_tiles[i].uid; masks[n] = s_tile_sides[i]; slots[n] = s_tiles[i].slot; n++; }
    for (int i = 1; i < n; i++) if (uids[i] < s_uid) return;   // not ours to do
    if (n != 9) return;
    int pos[9];
    if (!topo_layout_full(masks, pos)) return;
    bool change = false;
    for (int i = 0; i < 9; i++) if (slots[i] != pos[i]) change = true;
    if (!change) return;
    s_last_layout_ms = t;
    pkt_layout_t p = { 0 };
    mosaic_net_head(&p.h, PKT_LAYOUT);
    for (int i = 0; i < 9; i++) { p.uid[i] = uids[i]; p.slot[i] = (uint8_t)pos[i]; if (uids[i] == s_uid && pos[i] != s_slot) s_assigned = pos[i]; }
    for (int k = 0; k < 3; k++) esp_now_send(BCAST, (const uint8_t *)&p, sizeof p);   // a few times; ESP-NOW broadcasts aren't acknowledged
    ESP_LOGI(TAG, "complete 3x3 found: assigning positions");
}

static void resend(const void *pkt, int len, void *ctx) { (void)ctx; esp_now_send(BCAST, (const uint8_t *)pkt, len); }

static void net_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    int64_t last_hello = 0, last = now_ms();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SYNC_MS));
        esp_task_wdt_reset();
        int64_t t = now_ms(); float dt = (t - last) / 1000.f; last = t;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_tiles[0] = (cons_tile_t){ s_uid, s_slot, s_lifted, s_setup, s_tiles[0].health, t };
        if (t - last_hello >= HELLO_MS) { send_hello(); last_hello = t; }
        try_layout(t);
        if (s_brain_new) {   // the bridge is a unicast peer, for audio
            s_brain_new = false;
            esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
            memcpy(peer.peer_addr, s_brain_mac, 6);
            if (!esp_now_is_peer_exist(s_brain_mac)) esp_now_add_peer(&peer);
        }
        if (rel_poll(&s_out, t, resend, NULL)) { s_cmd_lost_ms = t; ESP_LOGW(TAG, "a command was never acknowledged"); }

        s_view = cons_view(s_tiles, CONS_MAX_TILES, t, CONFIG_DUST_WALL_EARS);
        uint16_t present = s_view.present;
        bool was_leader = s_snap.am_leader;
        s_snap.am_leader = !s_setup && s_slot < 9 && !s_view.self_dup && s_view.leader == s_slot && cons_may_lead(&s_follow, t);
        if (s_snap.am_leader) {
            if (!was_leader) {   // take over from the last sync without a jump, under a new epoch
                dust_view_t *v = &s_snap.view;
                float init[4] = { v->frame_cx, v->frame_cy, v->frame_sx, v->frame_sy };
                for (int k = 0; k < 4; k++) s_frame[k] = (spring_t){ init[k], 0, init[k] };
                for (int i = 0; i < 9; i++) s_pres[i] = (spring_t){ v->pres[i], 0, v->pres[i] };
                s_springs_init = s_snap.have_sync && v->frame_sx > 0;
                s_form_amt_target = v->form_amt > 0.5f ? 1 : 0;
                s_warp_k = (spring_t){ s_snap.warp_k, 0, 1 };
                s_prev_gone = (uint16_t)(0x1FF & ~s_snap.warp_to);
                uint32_t e = cons_take_lead(&s_follow, s_slot, t);
                ESP_LOGI(TAG, "screen %d leads (epoch %lu)", s_slot + 1, (unsigned long)e);
            }
            s_follow.last_ms = t;
            lead_step(t, dt, present);
        }
        s_snap.self_dup = s_view.self_dup; s_snap.ears = s_view.ears; s_snap.epoch = s_follow.epoch;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t mosaic_net_start(uint8_t my_slot, uint8_t channel, float gap_px)
{
    s_slot = my_slot; s_gap = gap_px;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_uid = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    s_lock = xSemaphoreCreateMutex();
    memset(s_tiles, 0, sizeof s_tiles);
    s_tiles[0] = (cons_tile_t){ .uid = s_uid, .slot = my_slot, .health = { .battery = 255, .temp_c = -128 } };
    cons_follow_init(&s_follow);
    rel_out_init(&s_out, (uint16_t)esp_random());   // a fresh start each boot, so old numbers aren't mistaken for repeats
    rel_dedupe_init(&s_seen);
    memset(&s_snap, 0, sizeof s_snap);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    esp_now_peer_info_t peer = { .channel = channel, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(peer.peer_addr, BCAST, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    xTaskCreate(net_task, "dust_net", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "screen %d on channel %d (uid %08lx)", my_slot + 1, channel, (unsigned long)s_uid);
    return ESP_OK;
}

void mosaic_net_set_lifted(bool lifted) { xSemaphoreTake(s_lock, portMAX_DELAY); s_lifted = lifted; xSemaphoreGive(s_lock); }
void mosaic_net_set_setup(bool in_setup) { xSemaphoreTake(s_lock, portMAX_DELAY); s_setup = in_setup; xSemaphoreGive(s_lock); }
void mosaic_net_set_sides(uint8_t sides, uint8_t rot) { xSemaphoreTake(s_lock, portMAX_DELAY); s_sides = sides & 0x1F; s_rot = rot & 3; xSemaphoreGive(s_lock); }
void mosaic_net_set_health(const dust_health_t *h) { xSemaphoreTake(s_lock, portMAX_DELAY); s_tiles[0].health = *h; xSemaphoreGive(s_lock); }
int  mosaic_net_assigned_slot(void) { return s_assigned; }
uint32_t mosaic_net_uid(void) { return s_uid; }

void mosaic_net_snapshot(wall_snapshot_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    out->view.t = wall_seconds(now_ms());
    xSemaphoreGive(s_lock);
}

uint16_t mosaic_net_errors(void)
{
    int64_t t = now_ms();
    uint16_t e = 0;
    if (s_view.self_dup) e |= DUST_ERR_DUP_SLOT;
    if (t - s_fw_older_ms < FW_WARN_MS || t - s_fw_newer_ms < FW_WARN_MS) e |= DUST_ERR_FW;
    if (t - s_cmd_lost_ms < 60000) e |= DUST_ERR_CMD_LOST;
    return e;
}
bool mosaic_net_outdated(void) { return now_ms() - s_fw_newer_ms < FW_WARN_MS; }

void mosaic_net_print_tiles(void)
{
    static const char *ERR[12] = { "dup-slot", "firmware", "mic", "speaker", "magnets", "imu", "cmd-lost", "hot", "low-batt", "crashed", "no-clips", "no-model" };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t = now_ms();
    printf("epoch %lu, leader %d, wall %03x, ears %03x, brain %s\n", (unsigned long)s_follow.epoch, s_view.leader + 1, s_view.present, s_view.ears,
           t - s_brain_ms < BRAIN_MS ? "online" : "offline");
    for (int i = 0; i < CONS_MAX_TILES; i++) {
        if (!alive(i, t)) continue;
        const cons_tile_t *c = &s_tiles[i];
        printf("%s %08lx  slot %-2d %-7s %4.1f fps  heap %3u KB  batt ", i == 0 ? "*" : " ", (unsigned long)c->uid, c->slot < 9 ? c->slot + 1 : 0,
               c->lifted ? "lifted" : c->setup ? "setup" : "docked", c->health.fps_x10 / 10.f, c->health.heap_kb);
        if (c->health.battery <= 100) printf("%3u%%", c->health.battery); else printf("  - ");
        if (c->health.temp_c != -128) printf("  %d C", c->health.temp_c);
        uint16_t err = c->health.err | (i == 0 ? mosaic_net_errors() : 0);
        for (int b = 0; b < 12; b++) if (err >> b & 1) printf("  !%s", ERR[b]);
        printf("\n");
    }
    xSemaphoreGive(s_lock);
}

/* ---------- voice plumbing ---------- */
void mosaic_net_set_voice_rx(mosaic_voice_rx_t cb) { xSemaphoreTake(s_lock, portMAX_DELAY); s_voice_rx = cb; xSemaphoreGive(s_lock); }
void mosaic_net_set_voice(bool speaking, bool session, float session_score, int session_left_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_speaking = speaking; s_session = session; s_session_score = session_score; s_session_left_ms = session_left_ms;
    xSemaphoreGive(s_lock);
}
void mosaic_net_set_mic_cal(float level_db) { s_mic_cal_x10 = (int16_t)fmaxf(-3000, fminf(3000, level_db * 10)); }

float mosaic_net_mic_cal_median(void)
{
    float v[CONS_MAX_TILES]; int n = 0;
    int64_t t = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 1; i < CONS_MAX_TILES; i++) if (alive(i, t) && s_tile_mic[i]) v[n++] = s_tile_mic[i] / 10.f;
    xSemaphoreGive(s_lock);
    if (!n) return NAN;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x; }
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

bool mosaic_net_brain(uint8_t *caps)
{
    bool on = now_ms() - s_brain_ms < BRAIN_MS;
    if (caps) *caps = on ? s_brain_caps : 0;
    return on;
}
esp_err_t mosaic_net_send_brain(const void *pkt, int len)
{
    if (!mosaic_net_brain(NULL)) return ESP_ERR_NOT_FOUND;
    uint8_t mac[6];
    xSemaphoreTake(s_lock, portMAX_DELAY); memcpy(mac, s_brain_mac, 6); xSemaphoreGive(s_lock);
    if (!esp_now_is_peer_exist(mac)) return ESP_ERR_NOT_FOUND;   // the net task adds it within 50 ms
    return esp_now_send(mac, (const uint8_t *)pkt, len);
}
void mosaic_net_broadcast(const void *pkt, int len) { esp_now_send(BCAST, (const uint8_t *)pkt, len); }
bool mosaic_net_is_ear(void) { bool e; xSemaphoreTake(s_lock, portMAX_DELAY); e = s_slot < 9 && (s_view.ears >> s_slot & 1); xSemaphoreGive(s_lock); return e; }
// cb runs with the network lock held: it must only set a flag.
void mosaic_net_set_cal_mic_cb(void (*cb)(void)) { s_cal_mic_cb = cb; }

/* ---------- command helpers ---------- */
static void send_cmd(pkt_cmd_t *c)
{
    int64_t t = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mosaic_net_head(&c->h, PKT_CMD);
    c->sender = s_uid;
    // talk levels stream 30 times a second: a lost one is replaced by the next, so they aren't numbered
    bool numbered = c->op != CMD_TALK;
    if (numbered) { c->cflags = CMD_F_ACK; c->seq = rel_next_seq(&s_out); }
    esp_now_send(BCAST, (const uint8_t *)c, sizeof *c);
    // repeat until the leader acknowledges it, unless this tile is the leader (or there's no wall)
    if (numbered && !s_snap.am_leader && s_view.leader >= 0) rel_track(&s_out, c, sizeof *c, c->seq, t);
    apply_cmd(c, t);
    xSemaphoreGive(s_lock);
}
static int16_t q_uv(float v) { v = fminf(1.9f, fmaxf(-1.9f, v)); return (int16_t)(v * UV_Q); }
static uint8_t q01(float v) { return (uint8_t)(fminf(1, fmaxf(0, v)) * 255); }

void dust_cmd_state(dust_state_t s) { pkt_cmd_t c = { .op = CMD_STATE, .a = (uint8_t)s }; send_cmd(&c); }
void dust_cmd_talk(int level) { pkt_cmd_t c = { .op = CMD_TALK, .a = (uint8_t)(level < 0 ? 0 : level > 255 ? 255 : level) }; send_cmd(&c); }
void dust_cmd_form(dust_form_t form, const char *text)
{
    pkt_cmd_t c = { .op = CMD_FORM, .a = (uint8_t)form };
    if (text) strncpy(c.text, text, DUST_TEXT_MAX);
    send_cmd(&c);
}
void dust_cmd_fx(dust_fx_type_t type, float x, float y, float dx, float dy, float strength, float radius, float seconds)
{
    float dl = sqrtf(dx * dx + dy * dy); if (dl > 1e-5f) { dx /= dl; dy /= dl; }
    pkt_cmd_t c = { .op = CMD_FX, .a = (uint8_t)type, .x = q_uv(x), .y = q_uv(y), .dx = (int8_t)(dx * 127), .dy = (int8_t)(dy * 127),
                    .strength = q01(strength), .radius = q01(radius), .duration = (uint8_t)fminf(255, fmaxf(1, seconds * 10)) };
    send_cmd(&c);
}
void dust_cmd_scatter(void) { pkt_cmd_t c = { .op = CMD_SCATTER }; send_cmd(&c); }
void dust_cmd_calm(void) { pkt_cmd_t c = { .op = CMD_CALM }; send_cmd(&c); }
void dust_cmd_cal_mic(void) { pkt_cmd_t c = { .op = CMD_CAL_MIC }; send_cmd(&c); }
