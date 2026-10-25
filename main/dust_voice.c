// Dust's ears and voice (see dust_voice.h). Built on Espressif's voice_wake_cmd example for the
// ESP-Mosaico: the same mic front end (AFE, mic only), WakeNet9 and MultiNet, here in English.
//
// Three tasks:
//   feed    mic → front end (zeros while this tile is talking, so Dust never hears himself)
//   detect  front end → wake word, levels, the noise floor, streaming to the Jetson, offline commands
//   play    replies from the Jetson and recorded clips → speaker, and the voice level for the glow
// and a control task for everything that arrives over the air: bids, replies, the Jetson's word.
#include "dust_voice.h"
#include "dust_arb.h"
#include "dust_adpcm.h"
#include "dust_consensus.h"
#include "mosaic_net.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#ifndef CONFIG_DUST_WAKE_WORD
#define CONFIG_DUST_WAKE_WORD "hiesp"
#endif
#ifndef CONFIG_DUST_SPEAKER_VOLUME
#define CONFIG_DUST_SPEAKER_VOLUME 70
#endif

static const char *TAG = "voice";

#define RING_N          (AUDIO_RATE * 2)     // the last 2 s the mic heard (after the front end)
#define PLAY_N          (AUDIO_RATE * 4)     // reply buffer
#define LEVEL_N         48                   // recent frame levels, about 1.5 s: how loud the wake word was
#define PREBUFFER       (AUDIO_RATE / 6)     // start a reply once ~160 ms of it is in
#define END_SILENCE_MS  800                  // this much quiet after speech: you've finished
#define NOTHING_SAID_MS 4000                 // won, but nobody spoke: give up
#define MAX_UTTER_MS    12000
#define REPLY_WAIT_MS   15000                // nothing back from the Jetson this long after you stopped
#define FOLLOW_MS       6000                 // after a reply, listen this long without the wake word
#define QUIET_TAIL_MS   400                  // after any tile stops talking, wake words are still echoes
#define CLAIM_REPEAT_MS 30
#define TALK_SEND_MS    33

typedef enum { V_OFF, V_IDLE, V_BIDDING, V_LISTEN, V_COMMAND, V_THINKING, V_SPEAKING, V_FOLLOW } vmode_t;
static const char *MODE_NAME[] = { "off", "idle", "bidding", "listening", "command", "thinking", "speaking", "follow-up" };

typedef struct { uint8_t len; uint8_t data[sizeof(pkt_audio_t)]; } rx_item_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rxq;
static vmode_t s_mode = V_OFF;
static int64_t s_mode_ms;
static arb_t   s_arb;
static uint32_t s_uid;
static int     s_slot = -1;
static bool    s_lifted, s_held, s_listen_ok = true;
static float   s_fps;
static uint16_t s_err;

// front end
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static int     s_mn_chunk;
static esp_codec_dev_handle_t s_mic, s_spk;
static volatile bool s_mute_mic;
static bool    s_wn_on = true;

// what the mic heard
static int16_t *s_ring;
static int64_t s_ring_count;                 // samples written since boot
static float   s_levels[LEVEL_N]; static int s_level_i;
static float   s_floor = -60;                // dBFS when nobody is talking
static float   s_mic_off;                    // this mic against the wall's median (cal mic)
static int64_t s_last_speech_ms;
static bool    s_speech_now;

// a turn: bidding, then listening (streaming or offline commands)
static float   s_bid;
static int     s_claims_left; static int64_t s_claim_next; static bool s_claim_taking;
static uint16_t s_utt, s_up_seq;
static int64_t s_up_pos;                     // next ring sample to send / feed to MultiNet
static adpcm_state_t s_enc;
static bool    s_heard_speech;

// playing
static int16_t *s_play;
static volatile uint32_t s_play_r, s_play_w; // ring positions (play task reads, control task writes)
static uint16_t s_play_utt, s_play_seq;
static adpcm_state_t s_dec;
static volatile bool s_reply_end, s_play_started, s_out_active;
static volatile int s_clip_req = -1;
static volatile float s_talk;
static int64_t s_out_end_ms = -100000;
static dust_state_t s_mood = DUST_IDLE;

// recorded lines: "DCLP", count, rate, then (offset, samples) per clip, ADPCM from a zero state
typedef struct __attribute__((packed)) { uint32_t magic; uint16_t count, rate; } clip_head_t;
typedef struct __attribute__((packed)) { uint32_t offset, samples; } clip_entry_t;
static const uint8_t *s_clips;
static int     s_nclips;

// mic calibration
static volatile bool s_cal_req;
static int64_t s_cal_until, s_cal_done_ms;
static double  s_cal_sum; static int s_cal_n;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---------- small helpers (call with s_lock held) ---------- */
static bool docked(void) { return !s_lifted && s_slot >= 0; }

static void set_mood(dust_state_t st)
{
    if (st == s_mood) return;
    s_mood = st;
    if (docked()) dust_cmd_state(st);   // on the wall: the whole wall shows it
}

static bool listening_allowed(void)
{
    if (s_err & (DUST_ERR_MIC | DUST_ERR_NO_MODEL)) return false;
    if (s_lifted || s_slot < 0) return true;             // on its own: always listens for itself
    return s_listen_ok && mosaic_net_is_ear();
}

static void wakenet(bool on)
{
    if (!s_afe || on == s_wn_on) return;
    if (on) s_afe->enable_wakenet(s_afe_data); else s_afe->disable_wakenet(s_afe_data);
    s_wn_on = on;
}

static void set_mode(vmode_t m, int64_t t)
{
    if (m != s_mode) ESP_LOGI(TAG, "%s → %s", MODE_NAME[s_mode], MODE_NAME[m]);
    s_mode = m; s_mode_ms = t;
}

static void ring_read(int64_t pos, int16_t *out, int n)
{
    for (int i = 0; i < n; i++) out[i] = s_ring[(pos + i) % RING_N];
}

static float wake_score(void)
{
    float peak = -100;
    for (int i = 0; i < LEVEL_N; i++) if (s_levels[i] > peak) peak = s_levels[i];
    return peak - s_floor - s_mic_off + (s_held ? ARB_HELD_DB : 0);
}

static void send_claim(bool taking)
{
    pkt_claim_t c = { .uid = s_uid, .score_x10 = (int16_t)fmaxf(-3000, fminf(3000, s_bid * 10)),
                      .group_x10 = (int16_t)fmaxf(-3000, fminf(3000, s_arb.session_score * 10)),
                      .cflags = (uint8_t)((taking ? CLAIM_TAKING : 0) | (docked() ? CLAIM_DOCKED : 0)) };
    mosaic_net_head(&c.h, PKT_CLAIM);
    mosaic_net_broadcast(&c, sizeof c);
}

static void end_turn(int64_t t, bool follow_up)
{
    if (follow_up && arb_owner(&s_arb, t)) { set_mode(V_FOLLOW, t); set_mood(DUST_LISTENING); }
    else { set_mode(V_IDLE, t); set_mood(DUST_IDLE); }
    s_claims_left = 0;
}

static void play_clip_locked(dust_clip_t c) { if ((int)c < s_nclips) s_clip_req = c; }

/* ---------- the wake word ---------- */
static void on_wake(int64_t t, float score, bool forced)
{
    if (!forced && !listening_allowed()) return;
    if (s_mode != V_IDLE && s_mode != V_FOLLOW) return;   // already in a turn
    if (!arb_wake(&s_arb, t, score, docked())) return;    // someone's talking: it's an echo
    s_bid = score;
    s_up_pos = s_ring_count;                              // what you say next starts here
    s_claims_left = 3; s_claim_next = t; s_claim_taking = false;
    set_mode(V_BIDDING, t);
    ESP_LOGI(TAG, "wake word, bidding %.1f dB", score);
}

// This tile won: answer as well as it can.
static void start_answer(int64_t t)
{
    s_claims_left = 3; s_claim_next = t; s_claim_taking = true;   // tell the others it's ours
    if (!s_lifted && s_slot >= 0)                                 // the dust leans toward whoever spoke
        dust_cmd_fx(DUST_FX_GATHER, ((s_slot % 3) + .5f) / 3, ((s_slot / 3) + .5f) / 3, 0, 0, .6f, .35f, 3);
    set_mood(DUST_LISTENING);
    uint8_t caps = 0;
    if (mosaic_net_brain(&caps) && (caps & BRAIN_STT)) {
        s_utt++; s_up_seq = 0; s_enc = (adpcm_state_t){ 0, 0 }; s_heard_speech = false;
        set_mode(V_LISTEN, t);
    } else if (s_mn) {
        s_mn->clean(s_mn_data);
        play_clip_locked(CLIP_YES);
        set_mode(V_COMMAND, t);
    } else {
        play_clip_locked(CLIP_OFFLINE);
        end_turn(t, false);
    }
}

/* ---------- streaming what you say to the Jetson ---------- */
static void stream(int64_t t)
{
    while (s_ring_count - s_up_pos >= AUDIO_FRAME) {
        int16_t pcm[AUDIO_FRAME];
        if (s_ring_count - s_up_pos > RING_N) s_up_pos = s_ring_count - RING_N;   // fell behind: skip ahead
        ring_read(s_up_pos, pcm, AUDIO_FRAME);
        s_up_pos += AUDIO_FRAME;
        pkt_audio_t p = { .uid = s_uid, .utt = s_utt, .seq = s_up_seq, .aflags = s_up_seq == 0 ? AUDIO_START : 0,
                          .nbytes = AUDIO_FRAME / 2, .score_x10 = (int16_t)(s_arb.session_score * 10), .pred = s_enc.pred, .index = s_enc.index };
        mosaic_net_head(&p.h, PKT_AUDIO);
        adpcm_encode(&s_enc, pcm, AUDIO_FRAME, p.data);
        if (mosaic_net_send_brain(&p, sizeof p) == ESP_ERR_NOT_FOUND) {   // the bridge vanished mid-sentence
            ESP_LOGW(TAG, "lost the Jetson while streaming");
            play_clip_locked(CLIP_OFFLINE); end_turn(t, false); return;
        }
        s_up_seq++;   // (a frame the radio couldn't queue is just a 20 ms gap)
    }
    if (s_speech_now && t - s_mode_ms > 150) s_heard_speech = true;   // sooner, it's the wake word trailing off
    bool done = (s_heard_speech && t - s_last_speech_ms > END_SILENCE_MS) || t - s_mode_ms > MAX_UTTER_MS;
    bool nothing = !s_heard_speech && t - s_mode_ms > NOTHING_SAID_MS;
    if (!done && !nothing) return;
    pkt_audio_t end = { .uid = s_uid, .utt = s_utt, .seq = s_up_seq, .aflags = AUDIO_END, .nbytes = 0 };
    mosaic_net_head(&end.h, PKT_AUDIO);
    for (int k = 0; k < 2; k++) mosaic_net_send_brain(&end, offsetof(pkt_audio_t, data));
    if (nothing) { end_turn(t, false); return; }
    set_mode(V_THINKING, t); set_mood(DUST_THINKING);
}

/* ---------- offline: a fixed set of commands on the chip ---------- */
typedef struct { const char *words; void (*run)(void); } voice_cmd_t;
static void c_ring(void) { dust_cmd_form(DUST_FORM_RING, NULL); }
static void c_orb(void) { dust_cmd_form(DUST_FORM_ORB, NULL); }
static void c_wave(void) { dust_cmd_form(DUST_FORM_WAVE, NULL); }
static void c_spiral(void) { dust_cmd_form(DUST_FORM_SPIRAL, NULL); }
static void c_cloud(void) { dust_cmd_form(DUST_FORM_CLOUD, NULL); }
static void c_body(void) { dust_cmd_form(DUST_FORM_BODY, NULL); }
static void c_scatter(void) { dust_cmd_scatter(); }
static void c_calm(void) { dust_cmd_calm(); }
static void c_swirl(void) { dust_cmd_fx(DUST_FX_SWIRL, .5f, .5f, 0, 0, .8f, .3f, 4); }
static void c_hello(void) { dust_cmd_form(DUST_FORM_TEXT, "HELLO"); }
static void c_here(void)
{
    float x = .5f, y = .5f;
    if (!s_lifted && s_slot >= 0) { x = ((s_slot % 3) + .5f) / 3; y = ((s_slot / 3) + .5f) / 3; }
    dust_cmd_fx(DUST_FX_GATHER, x, y, 0, 0, .9f, .35f, 5);
}
static void c_sleep(void) { dust_cmd_state(DUST_IDLE); dust_cmd_calm(); }
// MultiNet6 English takes the words as they are (upper case, no digits or punctuation)
static const voice_cmd_t COMMANDS[] = {
    { "MAKE A RING", c_ring }, { "MAKE AN ORB", c_orb }, { "MAKE A WAVE", c_wave }, { "MAKE A SPIRAL", c_spiral },
    { "MAKE A CLOUD", c_cloud }, { "BE YOURSELF", c_body }, { "SCATTER", c_scatter }, { "CALM DOWN", c_calm },
    { "SWIRL", c_swirl }, { "COME HERE", c_here }, { "GO TO SLEEP", c_sleep }, { "SAY HELLO", c_hello },
};
#define N_COMMANDS (int)(sizeof COMMANDS / sizeof COMMANDS[0])

static void command(int64_t t)
{
    if (s_out_active || s_clip_req >= 0) { s_up_pos = s_ring_count; return; }   // "Yes?" still playing
    int16_t buf[1024];
    while (s_ring_count - s_up_pos >= s_mn_chunk && s_mn_chunk <= 1024) {
        if (s_ring_count - s_up_pos > RING_N) s_up_pos = s_ring_count - RING_N;
        ring_read(s_up_pos, buf, s_mn_chunk);
        s_up_pos += s_mn_chunk;
        esp_mn_state_t st = s_mn->detect(s_mn_data, buf);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            int id = r && r->num > 0 ? r->command_id[0] : 0;
            ESP_LOGI(TAG, "command %d: %s", id, r && r->num > 0 ? r->string : "?");
            if (id >= 1 && id <= N_COMMANDS) { COMMANDS[id - 1].run(); play_clip_locked(CLIP_OK); }
            else play_clip_locked(CLIP_UNKNOWN);
            end_turn(t, false);
            return;
        }
        if (st == ESP_MN_STATE_TIMEOUT) { play_clip_locked(CLIP_UNKNOWN); end_turn(t, false); return; }
    }
}

/* ---------- tasks ---------- */
static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data), nch = s_afe->get_feed_channel_num(s_afe_data);
    size_t bytes = (size_t)chunk * sizeof(int16_t) * (size_t)nch;
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "feed buffer"); vTaskDelete(NULL); return; }
    for (;;) {
        if (esp_codec_dev_read(s_mic, buf, (int)bytes) != ESP_CODEC_DEV_OK) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (s_mute_mic) memset(buf, 0, bytes);   // Dust is talking: the mic would only hear him
        s_afe->feed(s_afe_data, buf);
    }
}

static void detect_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        esp_task_wdt_reset();
        if (!res || res->ret_value != ESP_OK) continue;
        int64_t t = now_ms();
        int n = res->data_size / (int)sizeof(int16_t);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < n; i++) s_ring[(s_ring_count + i) % RING_N] = res->data[i];
        s_ring_count += n;
        float vol = res->data_volume;
        s_speech_now = res->vad_state == VAD_SPEECH && !s_mute_mic;
        if (!s_mute_mic) {
            s_levels[s_level_i++ % LEVEL_N] = vol;
            // the floor drops quickly to a quieter room and rises slowly, so speech doesn't lift it
            if (res->vad_state == VAD_SILENCE) s_floor += (vol - s_floor) * (vol < s_floor ? 0.05f : 0.004f);
        }
        if (s_speech_now) {
            s_last_speech_ms = t;
            if (t < s_cal_until) { s_cal_sum += vol - s_floor; s_cal_n++; }
        }
        if (res->wakeup_state == WAKENET_DETECTED) { s_wn_on = false; on_wake(t, wake_score(), false); }   // the AFE pauses WakeNet after a detection
        switch (s_mode) {
        case V_LISTEN:  stream(t); break;
        case V_COMMAND: command(t); break;
        case V_FOLLOW: {
            uint8_t caps = 0;
            if (s_speech_now && mosaic_net_brain(&caps) && (caps & BRAIN_STT)) {   // a follow-up, no wake word needed
                s_utt++; s_up_seq = 0; s_enc = (adpcm_state_t){ 0, 0 }; s_heard_speech = true;
                s_up_pos = s_ring_count - AUDIO_RATE * 3 / 10;   // keep the start of the first word
                arb_touch(&s_arb, t);
                set_mode(V_LISTEN, t);
            } else if (t - s_mode_ms > FOLLOW_MS) end_turn(t, false);
            break; }
        default: break;
        }
        xSemaphoreGive(s_lock);
    }
}

static void play_task(void *arg)
{
    (void)arg;
    int16_t buf[AUDIO_FRAME];
    for (;;) {
        int clip = s_clip_req;
        if (clip >= 0) {   // a recorded line, start to finish
            s_clip_req = -1;
            const clip_entry_t *e = (const clip_entry_t *)(s_clips + sizeof(clip_head_t)) + clip;
            const uint8_t *src = s_clips + e->offset;
            adpcm_state_t st = { 0, 0 };
            s_out_active = true; s_mute_mic = true;
            for (uint32_t done = 0; done < e->samples && s_clip_req < 0; done += AUDIO_FRAME) {
                int n = e->samples - done < AUDIO_FRAME ? (int)(e->samples - done) & ~1 : AUDIO_FRAME;
                adpcm_decode(&st, src + done / 2, n, buf);
                if (n < AUDIO_FRAME) memset(buf + n, 0, (AUDIO_FRAME - n) * sizeof(int16_t));
                double acc = 0; for (int i = 0; i < AUDIO_FRAME; i++) acc += (double)buf[i] * buf[i];
                float db = 10 * log10f((float)(acc / AUDIO_FRAME) / (32768.f * 32768.f) + 1e-10f);
                s_talk = fminf(1, fmaxf(0, (db + 50) / 40));
                esp_codec_dev_write(s_spk, buf, sizeof buf);
            }
            s_out_active = false; s_out_end_ms = now_ms(); s_talk = 0;
            continue;
        }
        uint32_t avail = s_play_w - s_play_r;
        bool reply = s_mode == V_SPEAKING;
        if (reply && !s_play_started && (avail >= PREBUFFER || s_reply_end)) s_play_started = true;
        if (reply && s_play_started && avail > 0) {
            int n = avail < AUDIO_FRAME ? (int)avail : AUDIO_FRAME;
            for (int i = 0; i < n; i++) buf[i] = s_play[(s_play_r + i) % PLAY_N];
            if (n < AUDIO_FRAME) memset(buf + n, 0, (AUDIO_FRAME - n) * sizeof(int16_t));
            s_play_r += n;
            double acc = 0; for (int i = 0; i < AUDIO_FRAME; i++) acc += (double)buf[i] * buf[i];
            float db = 10 * log10f((float)(acc / AUDIO_FRAME) / (32768.f * 32768.f) + 1e-10f);
            s_talk = fminf(1, fmaxf(0, (db + 50) / 40));
            s_out_active = true; s_mute_mic = true;
            esp_codec_dev_write(s_spk, buf, sizeof buf);   // blocks about 20 ms: this paces the task
            continue;
        }
        if (s_out_active) { s_out_active = false; s_out_end_ms = now_ms(); }
        s_talk *= 0.7f;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Over the air (control task, s_lock held).
static void on_packet(const uint8_t *data, int len, int64_t t)
{
    const pkt_head_t *h = (const pkt_head_t *)data;
    switch (h->type) {
    case PKT_HELLO: {
        const pkt_hello_t *p = (const pkt_hello_t *)data;
        if (h->flags & HEAD_SPEAKING) arb_quiet(&s_arb, t + QUIET_TAIL_MS);
        if (h->flags & HEAD_SESSION) arb_peer_session(&s_arb, t, p->uid, !(h->flags & HEAD_LIFTED) && h->slot < 9, p->session_x10 / 10.f, t + p->session_left * 100);
        break; }
    case PKT_CLAIM: {
        const pkt_claim_t *c = (const pkt_claim_t *)data;
        if (arb_claim(&s_arb, t, c->uid, c->score_x10 / 10.f, c->cflags & CLAIM_DOCKED, c->cflags & CLAIM_TAKING, c->group_x10 / 10.f) == ARB_EV_YIELD) {
            ESP_LOGI(TAG, "%08lx is closer: standing down", (unsigned long)c->uid);
            end_turn(t, false);
        }
        break; }
    case PKT_AUDIO: {
        const pkt_audio_t *a = (const pkt_audio_t *)data;
        if (!(a->aflags & AUDIO_DOWN) || a->uid != s_uid || s_mode != V_SPEAKING || a->utt != s_play_utt) break;
        if (len < (int)offsetof(pkt_audio_t, data) + a->nbytes) break;
        if (a->seq < s_play_seq) break;   // late
        int gap = a->seq - s_play_seq; if (gap > 5) gap = 5;
        int n = a->nbytes * 2;
        if (s_play_w - s_play_r + (uint32_t)(gap * AUDIO_FRAME + n) > PLAY_N) break;   // full: drop rather than smear
        for (int i = 0; i < gap * AUDIO_FRAME; i++) s_play[s_play_w++ % PLAY_N] = 0;   // lost frames: a moment of silence
        int16_t pcm[AUDIO_FRAME];
        s_dec = (adpcm_state_t){ a->pred, a->index };
        adpcm_decode(&s_dec, a->data, n, pcm);
        for (int i = 0; i < n; i++) s_play[(s_play_w + i) % PLAY_N] = pcm[i];
        s_play_w += n;
        s_play_seq = a->seq + 1;
        break; }
    case PKT_VOICE: {
        const pkt_voice_t *v = (const pkt_voice_t *)data;
        if (v->uid != s_uid || v->utt != s_utt) break;
        switch (v->op) {
        case VOICE_YIELD:    // two of us streamed the same words; the Jetson picked the other
            if (s_mode == V_LISTEN || s_mode == V_THINKING) { arb_end(&s_arb); end_turn(t, false); }
            break;
        case VOICE_THINKING: if (s_mode == V_LISTEN || s_mode == V_THINKING) set_mood(DUST_THINKING); break;
        case VOICE_REPLY_START:
            if (s_mode != V_LISTEN && s_mode != V_THINKING) break;   // too late: this tile has moved on
            s_play_r = s_play_w = 0; s_play_utt = v->utt; s_play_seq = 0; s_reply_end = false; s_play_started = false;
            wakenet(false);
            set_mode(V_SPEAKING, t); set_mood(DUST_SPEAKING); arb_touch(&s_arb, t);
            break;
        case VOICE_REPLY_END: s_reply_end = true; break;
        case VOICE_FAIL:
            if (v->arg < CLIP_COUNT) play_clip_locked((dust_clip_t)v->arg);
            end_turn(t, false);
            break;
        }
        break; }
    }
}

static void control_task(void *arg)
{
    (void)arg;
    int64_t last_talk = 0;
    struct { uint8_t mac[6]; rx_item_t it; } msg;
    for (;;) {
        bool got = xQueueReceive(s_rxq, &msg, pdMS_TO_TICKS(10)) == pdTRUE;
        int64_t t = now_ms();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (got) on_packet(msg.it.data, msg.it.len, t);
        // claims go out three times, 30 ms apart
        if (s_claims_left > 0 && t >= s_claim_next) { send_claim(s_claim_taking); s_claims_left--; s_claim_next = t + CLAIM_REPEAT_MS; }
        arb_event_t ev = arb_poll(&s_arb, t);
        if (ev == ARB_EV_WON) { ESP_LOGI(TAG, "this tile answers (%.1f dB)", s_bid); start_answer(t); }
        else if (ev == ARB_EV_LOST && s_mode == V_BIDDING) { ESP_LOGI(TAG, "another tile is closer"); set_mode(V_IDLE, t); }
        // a reply that has finished playing
        if (s_mode == V_SPEAKING && s_reply_end && s_play_started && s_play_w == s_play_r && !s_out_active) { arb_touch(&s_arb, t); end_turn(t, true); }
        if (s_mode == V_SPEAKING && !s_reply_end && t - s_mode_ms > 60000) end_turn(t, false);   // a reply that never ended
        if (s_mode == V_THINKING && t - s_mode_ms > REPLY_WAIT_MS) { play_clip_locked(CLIP_NO_REPLY); end_turn(t, false); }
        // own speech is an echo for everyone, this tile included
        if (s_out_active) arb_quiet(&s_arb, t + QUIET_TAIL_MS);
        if (!s_out_active && s_mute_mic && t - s_out_end_ms > 250) s_mute_mic = false;
        // WakeNet only runs where it's wanted, and never while Dust talks
        bool want = listening_allowed() && !s_mute_mic && (s_mode == V_IDLE || s_mode == V_FOLLOW);
        wakenet(want);
        // the glow follows the voice: on the wall through the wall, off it on this ball
        if (s_out_active && !s_lifted && s_slot >= 0 && t - last_talk >= TALK_SEND_MS) { dust_cmd_talk((int)(s_talk * 255)); last_talk = t; }
        // mic calibration: every tile measures at once, then compares with the others
        if (s_cal_req) { s_cal_req = false; s_cal_until = t + 4000; s_cal_sum = 0; s_cal_n = 0; s_cal_done_ms = 0; printf("mic calibration: speak steadily from 2 m in front of the wall for 4 s\n"); }
        if (s_cal_until && t > s_cal_until && !s_cal_done_ms) {
            if (s_cal_n < 20) { printf("mic calibration: heard no speech\n"); s_cal_until = 0; }
            else { mosaic_net_set_mic_cal((float)(s_cal_sum / s_cal_n)); s_cal_done_ms = t; }
        }
        if (s_cal_done_ms && t - s_cal_done_ms > 1500) {   // others' results have arrived in their hellos
            float mine = (float)(s_cal_sum / s_cal_n), med = mosaic_net_mic_cal_median();
            s_mic_off = isnan(med) ? 0 : fmaxf(-10, fminf(10, mine - med));
            nvs_handle_t nh;
            if (nvs_open("dust", NVS_READWRITE, &nh) == ESP_OK) { nvs_set_i16(nh, "mic_off", (int16_t)(s_mic_off * 10)); nvs_commit(nh); nvs_close(nh); }
            printf("mic calibration: %.1f dB above the floor, wall median %.1f, offset %.1f dB (saved)\n", mine, isnan(med) ? mine : med, s_mic_off);
            s_cal_done_ms = 0; s_cal_until = 0;
        }
        mosaic_net_set_voice(s_out_active, arb_owner(&s_arb, t), s_arb.session_score, (int)(s_arb.session_until - t));
        xSemaphoreGive(s_lock);
    }
}

/* ---------- plumbing ---------- */
static void on_rx(const uint8_t mac[6], const void *pkt, int len)   // Wi-Fi task: copy and go
{
    struct { uint8_t mac[6]; rx_item_t it; } msg;
    if (len > (int)sizeof msg.it.data) return;
    memcpy(msg.mac, mac, 6); msg.it.len = (uint8_t)len; memcpy(msg.it.data, pkt, (size_t)len);
    xQueueSend(s_rxq, &msg, 0);
}
static void on_cal_mic(void) { s_cal_req = true; }

static void load_clips(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "clips");
    esp_partition_mmap_handle_t map;
    const void *data = NULL;
    if (!p || esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &data, &map) != ESP_OK) { s_err |= DUST_ERR_NO_CLIPS; return; }
    const clip_head_t *h = data;
    if (h->magic != 0x504C4344u /* "DCLP" */ || h->rate != AUDIO_RATE || h->count == 0) {
        ESP_LOGW(TAG, "no recorded lines flashed (tools/make_clips.py)");
        esp_partition_munmap(map); s_err |= DUST_ERR_NO_CLIPS; return;
    }
    s_clips = data; s_nclips = h->count < CLIP_COUNT ? h->count : CLIP_COUNT;
    ESP_LOGI(TAG, "%d recorded lines", s_nclips);
}

static esp_err_t start_front_end(void)
{
    s_models = esp_srmodel_init("model");
    char *wn = s_models ? esp_srmodel_filter(s_models, ESP_WN_PREFIX, CONFIG_DUST_WAKE_WORD) : NULL;
    if (!wn) { ESP_LOGE(TAG, "wake word model \"%s\" not in the model partition", CONFIG_DUST_WAKE_WORD); return ESP_ERR_NOT_FOUND; }
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) return ESP_ERR_NO_MEM;
    cfg->aec_init = false;          // one mic and no reference: the mic is muted while Dust talks instead
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_3;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = wn;
    cfg->wakenet_mode = DET_MODE_95;
    cfg->agc_init = true;
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_afe_data) return ESP_FAIL;

    char *mn = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mn && (s_mn = esp_mn_handle_from_name(mn)) != NULL) {
        s_mn_data = s_mn->create(mn, 6000);
        s_mn_chunk = s_mn->get_samp_chunksize(s_mn_data);
        esp_mn_commands_clear();
        for (int i = 0; i < N_COMMANDS; i++) esp_mn_commands_add(i + 1, (char *)COMMANDS[i].words);
        esp_mn_commands_update();
        ESP_LOGI(TAG, "offline commands: %s, %d phrases", mn, N_COMMANDS);
    } else ESP_LOGW(TAG, "no English MultiNet model: offline commands off");
    ESP_LOGI(TAG, "wake word: %s", wn);
    return ESP_OK;
}

esp_err_t dust_voice_start(int slot)
{
    s_slot = slot; s_uid = mosaic_net_uid();
    arb_init(&s_arb, s_uid);
    s_lock = xSemaphoreCreateMutex();
    s_rxq = xQueueCreate(32, sizeof(struct { uint8_t mac[6]; rx_item_t it; }));
    s_ring = heap_caps_calloc(RING_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_play = heap_caps_calloc(PLAY_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_rxq || !s_ring || !s_play) return ESP_ERR_NO_MEM;
    for (int i = 0; i < LEVEL_N; i++) s_levels[i] = -100;
    nvs_handle_t nh; int16_t off = 0;
    if (nvs_open("dust", NVS_READONLY, &nh) == ESP_OK) { nvs_get_i16(nh, "mic_off", &off); nvs_close(nh); }
    s_mic_off = off / 10.f;

    esp_codec_dev_sample_info_t fs = { .sample_rate = AUDIO_RATE, .bits_per_sample = 16, .channel = 1 };
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk || esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) { s_err |= DUST_ERR_SPK; s_spk = NULL; ESP_LOGE(TAG, "speaker not available"); }
    else esp_codec_dev_set_out_vol(s_spk, CONFIG_DUST_SPEAKER_VOLUME);
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic || esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) { s_err |= DUST_ERR_MIC; s_mic = NULL; ESP_LOGE(TAG, "microphone not available"); }
    else esp_codec_dev_set_in_gain(s_mic, 30.0f);
    load_clips();
    if (s_mic && start_front_end() != ESP_OK) s_err |= DUST_ERR_NO_MODEL;

    mosaic_net_set_voice_rx(on_rx);
    mosaic_net_set_cal_mic_cb(on_cal_mic);
    xTaskCreatePinnedToCore(control_task, "voice_ctl", 6144, NULL, 5, NULL, 0);
    if (s_spk) xTaskCreatePinnedToCore(play_task, "voice_play", 4096, NULL, 6, NULL, 0);
    if (s_afe_data) {
        xTaskCreatePinnedToCore(feed_task, "voice_feed", 6144, NULL, 5, NULL, 0);
        xTaskCreatePinnedToCore(detect_task, "voice_detect", 8192, NULL, 5, NULL, 1);
        s_mode = V_IDLE;
    }
    return ESP_OK;
}

void dust_voice_set_context(bool lifted, bool held, float fps)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_lifted = lifted; s_held = held; s_fps = fps;
    // listening costs CPU the renderer may need: step out below 10 fps, back in above 14
    if (fps > 0 && fps < 10) s_listen_ok = false;
    else if (fps > 14) s_listen_ok = true;
    xSemaphoreGive(s_lock);
}

void dust_voice_push_to_talk(void)
{
    if (!s_lock || s_mode == V_OFF) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t = now_ms();
    if (s_mode == V_SPEAKING || s_out_active) {   // interrupt him
        s_play_r = s_play_w; s_reply_end = true; s_clip_req = -1;
        end_turn(t, false);
    }
    s_arb.quiet_until = 0;   // the button is never an echo
    on_wake(t, wake_score() + ARB_BUTTON_DB, true);
    xSemaphoreGive(s_lock);
}

void dust_voice_fake_wake(float score_db)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    on_wake(now_ms(), score_db, true);
    xSemaphoreGive(s_lock);
}

bool dust_voice_local(dust_state_t *state, float *talk)
{
    if (!s_lifted || (s_mode == V_IDLE && !s_out_active) || s_mode == V_OFF) return false;
    *state = s_mood; *talk = s_talk;
    return true;
}

void dust_voice_play_clip(dust_clip_t c) { if ((int)c < s_nclips) s_clip_req = c; }
uint16_t dust_voice_errors(void) { return s_err; }
bool dust_voice_can_listen(void) { return !(s_err & (DUST_ERR_MIC | DUST_ERR_NO_MODEL)) && s_listen_ok; }

void dust_voice_print_status(void)
{
    if (!s_lock) { printf("voice not started\n"); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t = now_ms();
    uint8_t caps = 0; bool brain = mosaic_net_brain(&caps);
    printf("voice %s, %s, floor %.0f dBFS, mic offset %+.1f dB, wake word %s\n", MODE_NAME[s_mode],
           listening_allowed() ? "listening" : "not listening", s_floor, s_mic_off, s_wn_on ? "on" : "off");
    printf("  Jetson %s%s%s%s%s, offline commands %s, %d recorded lines\n", brain ? "online" : "offline",
           caps & BRAIN_STT ? " stt" : "", caps & BRAIN_LLM ? " llm" : "", caps & BRAIN_TTS_CLOUD ? " elevenlabs" : "",
           caps & BRAIN_TTS_LOCAL ? " piper" : "", s_mn ? "on" : "off", s_nclips);
    if (arb_owner(&s_arb, t)) printf("  owns the conversation for %.1f s (won at %.1f dB)\n", (s_arb.session_until - t) / 1000.f, s_arb.session_score);
    else if (s_arb.peer_owner && t < s_arb.peer_owner_until) printf("  %08lx owns the conversation\n", (unsigned long)s_arb.peer_owner);
    xSemaphoreGive(s_lock);
}
