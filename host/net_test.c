// Desktop tests of the wall's agreement, reliable commands, the voice codec, and who answers
// the wake word, in a simulated room over a lossy radio:  make net_test && ./net_test
#include "mosaic_net.h"
#include "dust_consensus.h"
#include "dust_reliable.h"
#include "dust_arb.h"
#include "dust_adpcm.h"
#include "link_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static double urand(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (s_rng >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(6.283185307 * v); }

/* ---------- packet sizes (jetson/dust/proto.py must agree) ---------- */
static void test_sizes(void)
{
    printf("packet sizes: head %zu hello %zu sync %zu cmd %zu ack %zu claim %zu audio %zu beacon %zu voice %zu\n",
           sizeof(pkt_head_t), sizeof(pkt_hello_t), sizeof(pkt_sync_t), sizeof(pkt_cmd_t), sizeof(pkt_ack_t),
           sizeof(pkt_claim_t), sizeof(pkt_audio_t), sizeof(pkt_beacon_t), sizeof(pkt_voice_t));
    CHECK(sizeof(pkt_head_t) == 8 && sizeof(pkt_hello_t) == 33 && sizeof(pkt_cmd_t) == 36 && sizeof(pkt_ack_t) == 18 &&
          sizeof(pkt_claim_t) == 17 && sizeof(pkt_audio_t) == 183 && sizeof(pkt_beacon_t) == 14 && sizeof(pkt_voice_t) == 16,
          "a packet changed size: update jetson/dust/proto.py to match");
    CHECK(sizeof(pkt_sync_t) <= 250 && sizeof(pkt_audio_t) <= 250, "a packet is over ESP-NOW's 250 bytes");
}

/* ---------- ADPCM ---------- */
static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void test_adpcm(void)
{
    enum { N = 16000 };
    static int16_t pcm[N], out[N]; static uint8_t enc[N / 2];
    // a voice-like test signal: a few harmonics with a slow syllable envelope (the same formula is in jetson/tests)
    for (int i = 0; i < N; i++) {
        double t = i / 16000.0, env = 0.5 + 0.5 * sin(6.283185307 * 3 * t);
        double x = sin(6.283185307 * 180 * t) + 0.5 * sin(6.283185307 * 360 * t) + 0.25 * sin(6.283185307 * 1250 * t);
        pcm[i] = (int16_t)lrint(9000 * env * x / 1.75);
    }
    adpcm_state_t e = { 0, 0 }, d = { 0, 0 };
    adpcm_encode(&e, pcm, N, enc);
    adpcm_decode(&d, enc, N, out);
    double sig = 0, err = 0;
    for (int i = 0; i < N; i++) { sig += (double)pcm[i] * pcm[i]; err += (double)(pcm[i] - out[i]) * (pcm[i] - out[i]); }
    double snr = 10 * log10(sig / (err + 1));
    uint32_t c = crc32(enc, N / 2);
    printf("adpcm: %.1f dB SNR on a voice-like signal, encoded crc %08x\n", snr, c);
    CHECK(snr > 20, "ADPCM SNR %.1f dB", snr);
    CHECK(e.pred == d.pred && e.index == d.index, "encoder and decoder disagree on the state");
    CHECK(c == 0x7c0d394du, "encoded bytes changed (crc %08x): jetson/tests/test_proto.py checks the same value", c);
    // a packet decoded on its own from its saved state matches decoding the whole stream
    adpcm_state_t s0 = { 0, 0 }, mid; int16_t a[640], b[320]; uint8_t ea[320];
    adpcm_encode(&s0, pcm, 320, ea); mid = s0; adpcm_encode(&s0, pcm + 320, 320, ea + 160);
    adpcm_state_t dd = { 0, 0 }; adpcm_decode(&dd, ea, 640, a);
    adpcm_decode(&mid, ea + 160, 320, b);
    CHECK(!memcmp(a + 320, b, sizeof b), "a frame decoded from its own state differs");
}

/* ---------- frames on the USB link between the bridge and the Jetson ---------- */
static void test_link(void)
{
    lf_reader_t rd = { 0 }; lf_msg_t got; static uint8_t enc[LF_MAX_ENC];
    int good = 0, bad = 0;
    for (int trial = 0; trial < 3000; trial++) {
        lf_msg_t m = { .kind = (uint8_t)(1 + trial % 4), .rssi = (int8_t)(-30 - trial % 60), .len = (uint8_t)(trial % (LF_MAX_DATA + 1)) };
        for (int k = 0; k < 6; k++) m.mac[k] = (uint8_t)(urand() * 256);
        for (int k = 0; k < m.len; k++) m.data[k] = trial % 3 == 0 ? 0 : trial % 3 == 1 ? 0xFF : (uint8_t)(urand() * 256);   // zeros, long runs, noise
        int n = lf_encode(&m, enc);
        for (int k = 0; k < n - 1; k++) if (!enc[k]) { bad++; break; }   // only the last byte may be 0
        if (trial % 10 == 0) { lf_feed(&rd, 0x41, &got); lf_feed(&rd, 0x00, &got); }   // garbage between frames
        int r = 0;
        for (int k = 0; k < n; k++) r = lf_feed(&rd, enc[k], &got);
        if (r == 1 && got.kind == m.kind && got.rssi == m.rssi && got.len == m.len && !memcmp(got.mac, m.mac, 6) && !memcmp(got.data, m.data, m.len)) good++;
        else bad++;
        if (trial % 7 == 0) {   // the same frame with one byte flipped must be rejected
            enc[n / 2] ^= 0x10; if (!enc[n / 2]) enc[n / 2] = 1;
            for (int k = 0; k < n; k++) r = lf_feed(&rd, enc[k], &got);
            if (r == 1) bad++;
        }
    }
    lf_msg_t m = { .kind = LF_TX, .mac = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, .rssi = 0, .len = 5, .data = { 1, 0, 2, 0, 0 } };
    int n = lf_encode(&m, enc);
    uint32_t c = crc32(enc, n);
    printf("link frames: %d of 3000 round trips intact, %d problems, sample frame crc %08x\n", good, bad, c);
    CHECK(good == 3000 && bad == 0, "link framing");
    CHECK(c == 0x876aab18u, "sample frame changed (crc %08x): jetson/tests/test_proto.py checks the same value", c);
}

/* ---------- reliable commands over a lossy radio ---------- */
typedef struct { double loss; int sent_to_leader; } link_t;
static int s_delivered[2000];
static rel_dedupe_t s_leader_seen;
static link_t s_link;
static uint16_t s_acks[64]; static int s_nacks;
static void radio_send(const void *pkt, int len, void *ctx)
{
    (void)len; (void)ctx;
    const pkt_cmd_t *c = pkt;
    if (urand() < s_link.loss) return;                   // lost on the way to the leader
    if (rel_first(&s_leader_seen, c->sender, c->seq, 0)) s_delivered[c->a | (c->text[0] << 8)]++;
    if (urand() >= s_link.loss && s_nacks < 64) s_acks[s_nacks++] = c->seq;   // the leader acks every copy
}
static void test_reliable(double loss)
{
    rel_out_t out; rel_out_init(&out, 65500);   // starts near the wrap, on purpose
    rel_dedupe_init(&s_leader_seen);
    memset(s_delivered, 0, sizeof s_delivered);
    s_link.loss = loss;
    int64_t t = 0; const int N = 1500;
    for (int i = 0; i < N; i++) {
        pkt_cmd_t c = { .sender = 0xBEEF, .cflags = CMD_F_ACK, .a = (uint8_t)(i & 0xFF), .text = { (char)(i >> 8) } };
        c.seq = rel_next_seq(&out);
        s_nacks = 0;
        radio_send(&c, sizeof c, NULL);
        rel_track(&out, &c, sizeof c, c.seq, t);
        for (int k = 0; k < s_nacks; k++) rel_acked(&out, s_acks[k]);
        for (int step = 0; step < 20; step++) {          // 20 ms between commands, polled every ms
            t++; s_nacks = 0;
            rel_poll(&out, t, radio_send, NULL);
            for (int k = 0; k < s_nacks; k++) rel_acked(&out, s_acks[k]);
        }
    }
    for (int step = 0; step < 2000; step++) { t++; s_nacks = 0; rel_poll(&out, t, radio_send, NULL); for (int k = 0; k < s_nacks; k++) rel_acked(&out, s_acks[k]); }
    int once = 0, twice = 0, never = 0;
    for (int i = 0; i < N; i++) { if (s_delivered[i] == 1) once++; else if (s_delivered[i] > 1) twice++; else never++; }
    printf("commands, %2.0f%% loss each way: %d of %d applied exactly once, %d twice, %d never (gave up on %u), %u repeats sent\n",
           loss * 100, once, N, twice, never, (unsigned)out.lost, (unsigned)out.resent);
    CHECK(twice == 0, "a command was applied twice");
    if (loss <= 0.1) CHECK(never == 0, "a command was lost at %.0f%% loss", loss * 100);
    if (loss <= 0.3) CHECK(never <= N / 200, "too many commands lost at %.0f%% loss", loss * 100);
}

/* ---------- agreement ---------- */
static cons_tile_t tile(uint32_t uid, int slot, int64_t seen) { cons_tile_t t = { .uid = uid, .slot = (uint8_t)slot, .seen_ms = seen }; t.health.can_listen = 1; return t; }
static void test_view(void)
{
    cons_tile_t t[CONS_MAX_TILES]; memset(t, 0, sizeof t);
    for (int i = 0; i < 9; i++) t[i] = tile(100 + i, i, 1000);
    cons_view_t v = cons_view(t, CONS_MAX_TILES, 1000, 9);
    CHECK(v.present == 0x1FF && v.leader == 0 && !v.self_dup, "full wall: present %03x leader %d", v.present, v.leader);
    CHECK(v.ears == 0x1FE, "full wall: the leader is spared listening (ears %03x)", v.ears);
    v = cons_view(t, CONS_MAX_TILES, 1000, 3);
    CHECK(v.ears == ((1 << 4) | (1 << 2) | (1 << 6)), "three ears: centre and two corners, not the leader (ears %03x)", v.ears);

    t[5].slot = 3;   // two tiles think they're screen 4
    v = cons_view(t, CONS_MAX_TILES, 1000, 9);
    CHECK(v.present == (0x1FF & ~(1 << 5)), "slot clash: one keeps the slot (present %03x)", v.present);
    cons_tile_t me[CONS_MAX_TILES]; memcpy(me, t, sizeof t);
    cons_tile_t tmp = me[0]; me[0] = me[5]; me[5] = tmp;   // seen from the tile with the higher uid
    CHECK(cons_view(me, CONS_MAX_TILES, 1000, 9).self_dup, "the higher uid stands aside");
    tmp = me[0]; me[0] = me[3]; me[3] = tmp;                // and from the lower one
    CHECK(!cons_view(me, CONS_MAX_TILES, 1000, 9).self_dup, "the lower uid keeps its slot");
    t[5].slot = 5;

    t[0].health.err = DUST_ERR_HOT;
    v = cons_view(t, CONS_MAX_TILES, 1000, 9);
    CHECK(v.leader == 1, "a hot tile is passed over for leading (leader %d)", v.leader);
    t[0].health.err = 0;

    t[1].lifted = true; t[2].seen_ms = 0;                   // one lifted, one silent
    v = cons_view(t, CONS_MAX_TILES, 1000, 9);
    CHECK(v.present == (0x1FF & ~6), "lifted and silent tiles leave the wall (present %03x)", v.present);
    printf("agreement: wall membership, slot clashes, leader choice, ears%s\n", s_fail ? "" : " ok");
}

static void test_epochs(void)
{
    // A (slot 0) leads at epoch 1. It freezes; B (slot 1) takes over at epoch 2. A wakes up still
    // thinking it leads.
    cons_follow_t fa, fb, ff; cons_follow_init(&fa); cons_follow_init(&fb); cons_follow_init(&ff);
    uint32_t ea = cons_take_lead(&fa, 0, 0);
    CHECK(cons_on_sync(&fb, ea, 0, false, 1, 10) == CONS_SYNC_TAKE, "B follows A");
    CHECK(cons_on_sync(&ff, ea, 0, false, 4, 10) == CONS_SYNC_TAKE, "F follows A");
    uint32_t eb = cons_take_lead(&fb, 1, 600);   // A silent for 600 ms
    CHECK(eb > ea, "B's epoch is newer");
    CHECK(cons_on_sync(&ff, eb, 1, false, 4, 600) == CONS_SYNC_TAKE, "F follows B");
    CHECK(cons_on_sync(&ff, ea, 0, false, 4, 650) == CONS_SYNC_IGNORE, "F ignores A's stale picture");
    CHECK(cons_on_sync(&fb, ea, 0, true, 1, 650) == CONS_SYNC_IGNORE, "B ignores A's stale picture");
    CHECK(cons_on_sync(&fa, eb, 1, true, 0, 650) == CONS_SYNC_STEP_DOWN, "A hears B's newer epoch and steps down");
    CHECK(!cons_may_lead(&fa, 700) && cons_may_lead(&fa, 650 + CONS_HOLD_MS), "A holds off before leading again");
    uint32_t ea2 = cons_take_lead(&fa, 0, 1000);   // A is still the lowest slot, so it takes over properly
    CHECK(ea2 > eb, "A comes back with a newer epoch than B's");
    CHECK(cons_on_sync(&fb, ea2, 0, true, 1, 1000) == CONS_SYNC_STEP_DOWN, "B steps down for A");
    CHECK(cons_on_sync(&ff, ea2, 0, false, 4, 1000) == CONS_SYNC_TAKE, "F follows A again");

    // fallback: the newest leader vanishes and only an older one is left: follow it, steadily
    cons_follow_t g; cons_follow_init(&g);
    cons_on_sync(&g, 5, 2, false, 6, 0);
    int taken = 0;
    for (int64_t t = 50; t < 3000; t += 50) taken += cons_on_sync(&g, 4, 3, false, 6, t) == CONS_SYNC_TAKE;
    CHECK(taken >= 3000 / 50 - 10, "after the newer leader is gone, the older one is followed every time (%d)", taken);
    // equal epochs (two leaders after a partition heals): the lower slot wins everywhere
    cons_follow_t h; cons_follow_init(&h);
    cons_on_sync(&h, 7, 4, false, 8, 0);
    CHECK(cons_on_sync(&h, 7, 2, false, 8, 10) == CONS_SYNC_TAKE && cons_on_sync(&h, 7, 4, false, 8, 20) == CONS_SYNC_IGNORE, "same epoch: lower slot");
    printf("epochs: stale leaders fenced off, step-down and takeover, fallback%s\n", s_fail ? "" : " ok");
}

/* ---------- who answers: a room, a wall, some lifted tiles, a lossy radio ---------- */
#define MAXT 12
typedef struct {
    arb_t arb; bool docked, held, ear; double x, y, z;
    double snr;                    // what this tile would measure for this utterance
    int64_t wake_at;               // when its wake word fires (-1: missed it)
    bool won, answering;
    int claims_left; int64_t next_claim; bool taking;
} simtile_t;

typedef struct { int64_t at; int to; uint32_t uid; float score, group_score; bool docked, taking; } msg_t;
static msg_t s_q[4096]; static int s_nq;

static void bcast(simtile_t *T, int n, int from, int64_t t, float score, bool taking, double loss)
{
    for (int j = 0; j < n; j++) if (j != from && urand() >= loss && s_nq < 4096)
        s_q[s_nq++] = (msg_t){ t + 2 + (int64_t)(urand() * 6), j, T[from].arb.uid, score, T[from].arb.session_score, T[from].docked, taking };
}

typedef struct { int trials, heard, one_fw, one_jetson, two_groups_fw, clear, clear_right, close, close_right; } stats_t;

static void room_trial(double loss, stats_t *st)
{
    simtile_t T[MAXT]; memset(T, 0, sizeof T);
    int nlift = (int)(urand() * 3);   // 0..2 tiles off the wall
    int n = 9;
    for (int i = 0; i < 9; i++) {
        T[i].docked = true;
        T[i].x = ((i % 3) - 1) * 0.0508; T[i].y = 1.5 - ((i / 3) - 1) * 0.0508; T[i].z = 0;
    }
    for (int k = 0; k < nlift; k++) {   // lifted tiles leave the wall (their slot goes dark)
        int s = 1 + (int)(urand() * 8); if (!T[s].docked) continue;
        T[s].docked = false;
        T[s].x = -2 + 4 * urand(); T[s].y = 0.7 + 0.8 * urand(); T[s].z = 0.3 + 3.5 * urand();
    }
    // the speaker: anywhere in the room, or holding one of the lifted tiles
    double sx = -2 + 4 * urand(), sy = 1.6, sz = 0.6 + 3.4 * urand();
    for (int i = 0; i < n; i++) if (!T[i].docked && urand() < 0.5) { T[i].held = true; sx = T[i].x + 0.25; sy = T[i].y + 0.15; sz = T[i].z + 0.1; break; }
    int leader = 0; while (leader < 9 && !T[leader].docked) leader++;
    for (int i = 0; i < n; i++) {
        T[i].ear = !T[i].docked || i != leader;
        arb_init(&T[i].arb, 0x1000 + (uint32_t)(urand() * 1e6) * 16 + i);
        double d = sqrt((T[i].x - sx) * (T[i].x - sx) + (T[i].y - sy) * (T[i].y - sy) + (T[i].z - sz) * (T[i].z - sz));
        if (d < 0.1) d = 0.1;
        T[i].snr = 40 - 20 * log10(d);                    // 40 dB above the floor at 1 m
        double measured = T[i].snr + gauss() * 1.5 + gauss() * 0.7;   // frame-to-frame jitter, mic calibration left over
        T[i].wake_at = (T[i].ear && T[i].snr > 12 && urand() < 0.95) ? 30 + (int64_t)(urand() * 120) : -1;
        T[i].arb.my_score = (float)(measured + (T[i].held ? ARB_HELD_DB : 0));
    }
    s_nq = 0;
    for (int64_t t = 0; t < 2500; t++) {
        for (int i = 0; i < n; i++) {
            simtile_t *a = &T[i];
            if (a->wake_at == t && arb_wake(&a->arb, t, a->arb.my_score, a->docked)) { a->claims_left = 3; a->next_claim = t; a->taking = false; }
            if (a->claims_left > 0 && t >= a->next_claim) { bcast(T, n, i, t, a->arb.my_score, a->taking, loss); a->claims_left--; a->next_claim = t + 30; }
        }
        for (int q = 0; q < s_nq; q++) if (s_q[q].at == t) {
            simtile_t *a = &T[s_q[q].to];
            if (arb_claim(&a->arb, t, s_q[q].uid, s_q[q].score, s_q[q].docked, s_q[q].taking, s_q[q].group_score) == ARB_EV_YIELD) a->answering = false;
        }
        for (int i = 0; i < n; i++) {
            arb_event_t ev = arb_poll(&T[i].arb, t);
            if (ev == ARB_EV_WON) { T[i].won = T[i].answering = true; T[i].claims_left = 3; T[i].next_claim = t; T[i].taking = true; }
        }
    }
    int heard = 0; for (int i = 0; i < n; i++) heard += T[i].wake_at >= 0;
    st->trials++;
    if (!heard) return;
    st->heard++;
    // firmware alone
    int ans = 0, groups_wall = 0, groups_lifted = 0;
    for (int i = 0; i < n; i++) if (T[i].answering) { ans++; if (T[i].docked) groups_wall = 1; else groups_lifted++; }
    if (ans == 1) st->one_fw++;
    if (groups_wall + groups_lifted > 1) st->two_groups_fw++;
    // the Jetson sees every tile that won and started streaming, and keeps the best bid
    int pick = -1;
    for (int i = 0; i < n; i++) if (T[i].won && T[i].answering && (pick < 0 || T[i].arb.session_score > T[pick].arb.session_score)) pick = i;
    if (pick < 0) for (int i = 0; i < n; i++) if (T[i].won && (pick < 0 || T[i].arb.session_score > T[pick].arb.session_score)) pick = i;
    if (pick >= 0) st->one_jetson++;
    // Did the closest group answer (the wall counts as one group, each lifted tile as another)?
    // Only meaningful when one group really is closer; within the wall, tiles 5 cm apart are a tie.
    double g[MAXT]; int gid[MAXT];   // group id: 0 the wall, 1 + i a lifted tile
    for (int i = 0; i < n; i++) { gid[i] = T[i].docked ? 0 : 1 + i; g[i] = T[i].wake_at >= 0 ? T[i].snr + (T[i].held ? ARB_HELD_DB : 0) : -1e9; }
    int best = 0; for (int i = 1; i < n; i++) if (g[i] > g[best]) best = i;
    double second = -1e9; for (int i = 0; i < n; i++) if (gid[i] != gid[best] && g[i] > second) second = g[i];
    bool right = pick >= 0 && gid[pick] == gid[best];
    if (second < -1e8) return;   // only one group heard it
    if (g[best] - second >= 3) { st->clear++; st->clear_right += right; }
    else { st->close++; st->close_right += right; }
}

static void test_room(void)
{
    static const double LOSS[3] = { 0.0, 0.1, 0.3 };
    for (int l = 0; l < 3; l++) {
        stats_t st = { 0 };
        for (int k = 0; k < 20000; k++) room_trial(LOSS[l], &st);
        double h = st.heard;
        printf("who answers, %2.0f%% loss: %d utterances; exactly one answers %.2f%% (tiles alone), %.2f%% (with the Jetson); "
               "wall and a lifted tile both answering %.2f%% (tiles alone)\n"
               "    clearly closest group (3 dB+) answers %.2f%% of %d; near-ties (<3 dB) go to the closer %.1f%% of %d\n",
               LOSS[l] * 100, st.heard, 100 * st.one_fw / h, 100 * st.one_jetson / h, 100 * st.two_groups_fw / h,
               100.0 * st.clear_right / st.clear, st.clear, 100.0 * st.close_right / st.close, st.close);
        if (LOSS[l] == 0) CHECK(st.one_fw == st.heard, "with no loss, exactly one tile must answer every time");
        CHECK(st.one_jetson == st.heard, "with the Jetson settling doubles, exactly one answers every time");
        CHECK(st.clear_right >= 0.99 * st.clear, "the clearly closest group should answer");
    }
}

static void test_session(void)
{
    // A owns the conversation. B, slightly louder, doesn't take it over; much louder, it does.
    arb_t a, b; arb_init(&a, 10); arb_init(&b, 20);
    arb_wake(&a, 0, 20, false); arb_claim(&a, 5, 20, 15, false, false, 0);
    CHECK(arb_poll(&a, ARB_WINDOW_MS) == ARB_EV_WON, "A wins its first bid");
    arb_peer_session(&b, 300, 10, false, 20, 300 + ARB_SESSION_MS);
    arb_wake(&a, 1000, 20, false); arb_wake(&b, 1000, 23, false);
    arb_claim(&a, 1005, 20, 23, false, false, 0); arb_claim(&b, 1005, 10, 20, false, false, 0);
    CHECK(arb_poll(&a, 1000 + ARB_WINDOW_MS) == ARB_EV_WON && arb_poll(&b, 1000 + ARB_WINDOW_MS) == ARB_EV_LOST, "3 dB louder doesn't steal the conversation");
    arb_wake(&a, 3000, 20, false); arb_wake(&b, 3000, 28, false);
    arb_claim(&a, 3005, 20, 28, false, false, 0); arb_claim(&b, 3005, 10, 20, false, false, 0);
    CHECK(arb_poll(&a, 3000 + ARB_WINDOW_MS) == ARB_EV_LOST && arb_poll(&b, 3000 + ARB_WINDOW_MS) == ARB_EV_WON, "8 dB louder takes it over");
    // echoes: while anyone is talking, the wake word is ignored
    arb_t c; arb_init(&c, 30); arb_quiet(&c, 5000);
    CHECK(!arb_wake(&c, 4900, 30, true) && arb_wake(&c, 5001, 30, true), "wake words during speech are echoes");
    // both think they won (each missed the other's bid): the better bid keeps it
    arb_t d, e; arb_init(&d, 40); arb_init(&e, 50);
    arb_wake(&d, 0, 15, false); arb_wake(&e, 0, 18, false);
    arb_poll(&d, ARB_WINDOW_MS); arb_poll(&e, ARB_WINDOW_MS);
    CHECK(arb_claim(&d, 210, 50, 18, false, true, 18) == ARB_EV_YIELD && arb_claim(&e, 210, 40, 15, false, true, 15) == ARB_EV_NONE, "a double between groups is settled by the groups' bids");
    // the same inside the wall: the louder tile keeps it
    arb_t f, g2; arb_init(&f, 60); arb_init(&g2, 70);
    arb_wake(&f, 0, 15, true); arb_wake(&g2, 0, 18, true);
    arb_poll(&f, ARB_WINDOW_MS); arb_poll(&g2, ARB_WINDOW_MS);
    CHECK(arb_claim(&f, 210, 70, 18, true, true, 18) == ARB_EV_YIELD && arb_claim(&g2, 210, 60, 15, true, true, 15) == ARB_EV_NONE, "a double inside the wall goes to the louder tile");
    // the wall bids its average: one noisy wall tile doesn't outbid a lifted tile that's closer
    arb_t w1; arb_init(&w1, 80);
    arb_wake(&w1, 0, 26, true);
    arb_claim(&w1, 5, 81, 18, true, false, 0); arb_claim(&w1, 5, 82, 19, true, false, 0); arb_claim(&w1, 5, 90, 23, false, false, 0);
    CHECK(arb_poll(&w1, ARB_WINDOW_MS) == ARB_EV_LOST, "the wall's average (21 dB) loses to a lifted tile at 23 dB");
    printf("conversation: stays with its owner, can be taken over, echoes ignored, doubles settled%s\n", s_fail ? "" : " ok");
}

int main(void)
{
    test_sizes();
    test_adpcm();
    test_link();
    test_reliable(0.0); test_reliable(0.1); test_reliable(0.3);
    test_view();
    test_epochs();
    test_session();
    test_room();
    printf(s_fail ? "%d checks FAILED\n" : "all checks passed\n", s_fail);
    return s_fail != 0;
}
