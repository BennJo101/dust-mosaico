// Dust on an ESP-Mosaico wall.
//
// Every screen runs this same firmware. Each knows its place in the 3x3 wall (set once on
// first boot), renders its slice of Dust from the shared wall state, and turns into a ball
// of dust when it is lifted off the wall.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "board_port.h"
#include "dust_render.h"
#include "mosaic_net.h"
#include "dust_warp.h"
#include "dust_topo.h"
#include "dust_voice.h"
#include "dust_consensus.h"

static const char *TAG = "dust";
static uint16_t *s_fb[2];
static volatile bool s_force_lift = false, s_force_dock = false;

/* ---------- slot storage ---------- */
static int load_slot(void)
{
#if CONFIG_DUST_SLOT > 0
    return CONFIG_DUST_SLOT - 1;
#else
    nvs_handle_t h; uint8_t v = 0xFF;
    if (nvs_open("dust", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "slot", &v); nvs_close(h); }
    return v < 9 ? v : -1;
#endif
}
static void save_slot(int slot)
{
    nvs_handle_t h;
    if (nvs_open("dust", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "slot", (uint8_t)slot); nvs_commit(h); nvs_close(h); }
}

/* ---------- setup screen: big digit, tap for next, hold to save ---------- */
static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++)
        if (xx >= 0 && yy >= 0 && xx < DUST_PANEL_PX && yy < DUST_PANEL_PX) fb[yy * DUST_PANEL_PX + xx] = c;
}
static void draw_digit(uint16_t *fb, int d, int cx, int cy, int s, uint16_t c)
{
    static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int w = s, h = s * 2, t = s / 5, x = cx - w / 2, y = cy - h / 2;
    uint8_t m = SEG[d];
    if (m & 0x01) fill_rect(fb, x, y, w, t, c);
    if (m & 0x02) fill_rect(fb, x + w - t, y, t, h / 2, c);
    if (m & 0x04) fill_rect(fb, x + w - t, y + h / 2, t, h / 2, c);
    if (m & 0x08) fill_rect(fb, x, y + h - t, w, t, c);
    if (m & 0x10) fill_rect(fb, x, y + h / 2, t, h / 2, c);
    if (m & 0x20) fill_rect(fb, x, y, t, h / 2, c);
    if (m & 0x40) fill_rect(fb, x, y + h / 2 - t / 2, w, t, c);
}
static uint16_t rgb565(int r, int g, int b, bool swap)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
}
static int run_setup(void)
{
    const bool sw = board_port_swap_bytes();
    int choice = 0; int64_t press_start = -1; bool was = false;
    mosaic_net_set_setup(true);
    for (;;) {
        uint16_t *fb = s_fb[0];
        uint16_t bg = rgb565(3, 6, 11, sw);
        for (int i = 0; i < DUST_PANEL_PX * DUST_PANEL_PX; i++) fb[i] = bg;
        // a 3x3 map with this screen's cell lit
        for (int i = 0; i < 9; i++) {
            int x = 150 + (i % 3) * 62, y = 40 + (i / 3) * 62;
            fill_rect(fb, x, y, 54, 54, i == choice ? rgb565(95, 227, 255, sw) : rgb565(26, 36, 46, sw));
        }
        draw_digit(fb, choice + 1, 240, 330, 70, rgb565(211, 236, 244, sw));
        board_port_flush(fb);

        int x, y; bool now = board_port_touch(&x, &y);
        int64_t t = esp_timer_get_time() / 1000;
        if (now && !was) press_start = t;
        if (now && press_start >= 0 && t - press_start > 1500) {   // hold: save
            save_slot(choice);
            fill_rect(fb, 0, 440, DUST_PANEL_PX, 40, rgb565(255, 171, 74, sw)); board_port_flush(fb);
            vTaskDelay(pdMS_TO_TICKS(600));
            mosaic_net_set_setup(false);
            return choice;
        }
        if (!now && was && press_start >= 0 && t - press_start < 600) choice = (choice + 1) % 9;   // tap: next
        if (!now) press_start = -1;
        was = now;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ---------- lifting and neighbours ----------
   1. Edge magnets: each tile's two magnetometers read the magnets of the tiles snapped onto
      its edges, and the wall plate's holding magnets behind it (dust_topo.c). A tile that
      feels no neighbours and no plate has been lifted; one that feels either again is back.
      Neighbours notice too, the instant a tile leaves.
   2. Motion sensor: a sharp tilt or move means lifted; resting again for 1.5 s means docked.
   3. Hold the screen 1.2 s: works with no sensors at all. */
typedef struct {
    bool lifted; float dock[3]; bool have_dock; int64_t moved_ms, still_since; float prev[3];
    // neighbour sensing
    uint8_t mask, cand; int64_t cand_ms; bool ever_joined; int rot;
    // in someone's hand: a lifted tile that moved in the last few seconds
    float hand_prev[3]; int64_t hand_moved_ms;
} lift_t;

#define TOPO_DEBOUNCE_MS  120
#define TOPO_MAX_RESIDUAL 150.f   // µT: worse than this, something else magnetic is nearby; hold the last answer

static topo_cal_t s_cal;
static void load_cal(void)
{
    nvs_handle_t h; size_t n = sizeof s_cal;
    if (nvs_open("dust", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_blob(h, "topo", &s_cal, &n) != ESP_OK || n != sizeof s_cal) memset(&s_cal, 0, sizeof s_cal); nvs_close(h); }
}
static void save_cal(void)
{
    nvs_handle_t h;
    if (nvs_open("dust", NVS_READWRITE, &h) == ESP_OK) { nvs_set_blob(h, "topo", &s_cal, sizeof s_cal); nvs_commit(h); nvs_close(h); }
}
// Average 32 readings (about a third of a second).
static bool read_avg(float out[2][3])
{
    float acc[2][3] = { 0 }, v[2][3]; int ok = 0;
    for (int i = 0; i < 32; i++) {
        if (board_port_mag3(v)) { for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) acc[s][a] += v[s][a]; ok++; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ok) return false;
    for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) out[s][a] = acc[s][a] / ok;
    return true;
}
// "cal alone" with no neighbours, then "cal top" (one neighbour snapped on top), and so on.
static void calibrate(const char *what)
{
    float r[2][3];
    if (!read_avg(r)) { printf("magnetometers not readable yet (see board_port_mag3)\n"); return; }
    static const char *SIDES[SIDE_COUNT] = { "top", "right", "bottom", "left", "back" };
    if (!strcmp(what, "alone")) { memcpy(s_cal.alone, r, sizeof r); s_cal.have_alone = true; }
    else {
        int k = -1; for (int i = 0; i < SIDE_COUNT; i++) if (!strcmp(what, SIDES[i])) k = i;
        if (k < 0 || !s_cal.have_alone) { printf("use: cal alone (off the plate, no neighbours), then cal top|right|bottom|left|back\n"); return; }
        for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) s_cal.side[s][k][a] = r[s][a] - s_cal.alone[s][a];
        s_cal.have_side[k] = true;
        printf("%s: adds %.0f / %.0f uT\n", what, sqrtf(s_cal.side[0][k][0]*s_cal.side[0][k][0]+s_cal.side[0][k][1]*s_cal.side[0][k][1]+s_cal.side[0][k][2]*s_cal.side[0][k][2]),
               sqrtf(s_cal.side[1][k][0]*s_cal.side[1][k][0]+s_cal.side[1][k][1]*s_cal.side[1][k][1]+s_cal.side[1][k][2]*s_cal.side[1][k][2]));
    }
    save_cal();
    printf("calibration saved (alone:%d top:%d right:%d bottom:%d left:%d back:%d)\n", s_cal.have_alone, s_cal.have_side[0], s_cal.have_side[1], s_cal.have_side[2], s_cal.have_side[3], s_cal.have_side[4]);
}

// Quarter turns from gravity when the tile hangs on a wall (screen vertical).
static int rotation_from_gravity(const float g[3], int prev)
{
    if (fabsf(g[2]) > 0.6f) return prev;             // lying flat: keep what we had
    float ang = atan2f(-g[0], -g[1]);                // gravity straight down the tile = 0
    int q = (int)lroundf(ang / (3.14159265f / 2)); q = ((q % 4) + 4) % 4;
    return q;
}

// Returns true if the magnets decided the lifted state this frame.
static bool lift_by_neighbours(lift_t *L, int64_t t)
{
    float v[2][3], g[3], res;
    if (!s_cal.have_alone || !board_port_mag3(v)) return false;
    if (board_port_accel(g)) { int r = rotation_from_gravity(g, L->rot); if (r != L->rot) { L->rot = r; board_port_set_rotation(r); } }
    uint8_t m = topo_classify(&s_cal, v, &res);
    if (res > TOPO_MAX_RESIDUAL) m = L->mask;
    if (m != L->cand) { L->cand = m; L->cand_ms = t; }
    else if (m != L->mask && t - L->cand_ms > TOPO_DEBOUNCE_MS) {
        uint8_t was = L->mask; L->mask = m;
        ESP_LOGI(TAG, "neighbours %c%c%c%c%s", m & 1 ? 'T' : '-', m & 2 ? 'R' : '-', m & 4 ? 'B' : '-', m & 8 ? 'L' : '-', m & 16 ? " · on the plate" : "");
        if (m) { L->ever_joined = true; if (L->lifted) ESP_LOGI(TAG, "snapped back on"); L->lifted = false; }
        else if (was && L->ever_joined) { L->lifted = true; ESP_LOGI(TAG, "lifted off the cluster"); }
    }
    mosaic_net_set_sides(topo_rotate(L->mask, L->rot), (uint8_t)L->rot);
    return true;
}

static void lift_update(lift_t *L, int64_t t)
{
    float g[3];
    if (s_force_lift) { s_force_lift = false; L->lifted = true; }
    if (s_force_dock) { s_force_dock = false; L->lifted = false; }
    if (lift_by_neighbours(L, t)) return;
    if (!board_port_accel(g)) return;
    float dx = g[0] - L->prev[0], dy = g[1] - L->prev[1], dz = g[2] - L->prev[2];
    float jitter = sqrtf(dx * dx + dy * dy + dz * dz);
    memcpy(L->prev, g, sizeof g);
    float mag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    bool still = fabsf(mag - 1) < 0.06f && jitter < 0.02f;
    if (!L->lifted) {
        if (!L->have_dock) { memcpy(L->dock, g, sizeof g); L->have_dock = true; }
        float ex = g[0] - L->dock[0], ey = g[1] - L->dock[1], ez = g[2] - L->dock[2];
        if (sqrtf(ex * ex + ey * ey + ez * ez) > 0.25f) {
            if (L->moved_ms < 0) L->moved_ms = t;
            if (t - L->moved_ms > 60) { L->lifted = true; L->still_since = -1; ESP_LOGI(TAG, "lifted"); }
        } else {
            L->moved_ms = -1;
            if (still) for (int i = 0; i < 3; i++) L->dock[i] += (g[i] - L->dock[i]) * 0.02f;   // follow slow drift
        }
    } else {
        // back on the wall: resting, and facing the way it faced when docked
        float dot = (g[0] * L->dock[0] + g[1] * L->dock[1] + g[2] * L->dock[2]) / (mag + 1e-6f);
        if (still && dot > cosf(12 * 3.14159f / 180)) {
            if (L->still_since < 0) L->still_since = t;
            if (t - L->still_since > 1500) { L->lifted = false; L->moved_ms = -1; ESP_LOGI(TAG, "docked"); }
        } else L->still_since = -1;
    }
}

/* ---------- first boot: snap the tiles together ----------
   Shows which of this tile's sides feel a neighbour. When all nine form a complete 3x3, the
   lowest-numbered tile assigns positions from the magnets and every tile restarts into its
   slot. Holding the screen for 2 s opens the manual picker instead. */
static int join_screen(lift_t *lift)
{
    const bool sw = board_port_swap_bytes();
    int64_t press = -1;
    for (;;) {
        int64_t t = esp_timer_get_time() / 1000;
        lift_update(lift, t);
        int a = mosaic_net_assigned_slot();
        if (a >= 0) { save_slot(a); esp_restart(); }
        uint16_t *fb = s_fb[0];
        uint16_t bg = rgb565(3, 6, 11, sw), dim = rgb565(26, 36, 46, sw), on = rgb565(95, 227, 255, sw), me = rgb565(255, 171, 74, sw);
        for (int i = 0; i < DUST_PANEL_PX * DUST_PANEL_PX; i++) fb[i] = bg;
        fill_rect(fb, 180, 180, 120, 120, me);                                   // this tile
        const int pos[4][4] = { {180, 40, 120, 120}, {320, 180, 120, 120}, {180, 320, 120, 120}, {40, 180, 120, 120} };
        uint8_t m = topo_rotate(lift->mask, lift->rot);
        if (m & (1 << SIDE_BACK)) fill_rect(fb, 0, 0, DUST_PANEL_PX, 12, on);   // top bar: on the wall plate
        for (int k = 0; k < 4; k++) fill_rect(fb, pos[k][0], pos[k][1], pos[k][2], pos[k][3], m >> k & 1 ? on : dim);
        if (!s_cal.have_alone) fill_rect(fb, 0, 460, DUST_PANEL_PX, 20, me);     // amber bar: calibrate first (cal alone ...)
        board_port_flush(fb);
        int x, y; bool tch = board_port_touch(&x, &y);
        if (tch && press < 0) press = t;
        if (!tch) press = -1;
        if (press >= 0 && t - press > 2000) { int s = run_setup(); esp_restart(); return s; }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ---------- console: everything an AI (or you) can tell Dust to do ----------
   state idle|listening|thinking|speaking     talk 0-255
   form body|orb|ring|wave|spiral|cloud        form text HELLO
   flow DX DY   swirl X Y   gather X Y   pulse X Y   (X, Y in 0..1 across the wall, y down)
   scatter   calm   lift   dock   slot 1-9   cal alone|top|right|bottom|left|back
   cal mic   health (every tile, its health and errors)   voice   wake DB (pretend)   say N (a clip) */
static void console_task(void *arg)
{
    (void)arg;
    char line[80], word[16], txt[DUST_TEXT_MAX + 2];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        line[strcspn(line, "\r\n")] = 0;
        int n; float a, b;
        if (!strcmp(line, "state idle")) dust_cmd_state(DUST_IDLE);
        else if (!strcmp(line, "state listening")) dust_cmd_state(DUST_LISTENING);
        else if (!strcmp(line, "state thinking")) dust_cmd_state(DUST_THINKING);
        else if (!strcmp(line, "state speaking")) dust_cmd_state(DUST_SPEAKING);
        else if (sscanf(line, "talk %d", &n) == 1) dust_cmd_talk(n);
        else if (sscanf(line, "form text %11[^\n]", txt) == 1) dust_cmd_form(DUST_FORM_TEXT, txt);
        else if (sscanf(line, "form %15s", word) == 1 && dust_form_from_name(word) >= 0) dust_cmd_form((dust_form_t)dust_form_from_name(word), NULL);
        else if (sscanf(line, "flow %f %f", &a, &b) == 2) dust_cmd_fx(DUST_FX_FLOW, .5f, .5f, a, b, .8f, .3f, 4);
        else if (sscanf(line, "swirl %f %f", &a, &b) == 2) dust_cmd_fx(DUST_FX_SWIRL, a, b, 0, 0, .8f, .28f, 4);
        else if (sscanf(line, "gather %f %f", &a, &b) == 2) dust_cmd_fx(DUST_FX_GATHER, a, b, 0, 0, .8f, .3f, 4);
        else if (sscanf(line, "pulse %f %f", &a, &b) == 2) dust_cmd_fx(DUST_FX_PULSE, a, b, 0, 0, 1, .3f, 2.5f);
        else if (!strcmp(line, "scatter")) dust_cmd_scatter();
        else if (!strcmp(line, "calm")) dust_cmd_calm();
        else if (!strcmp(line, "lift")) s_force_lift = true;
        else if (!strcmp(line, "dock")) s_force_dock = true;
        else if (sscanf(line, "slot %d", &n) == 1 && n >= 1 && n <= 9) { save_slot(n - 1); esp_restart(); }
        else if (!strcmp(line, "cal mic")) dust_cmd_cal_mic();
        else if (sscanf(line, "cal %15s", word) == 1) calibrate(word);
        else if (!strcmp(line, "health")) mosaic_net_print_tiles();
        else if (!strcmp(line, "voice")) dust_voice_print_status();
        else if (sscanf(line, "wake %f", &a) == 1) dust_voice_fake_wake(a);
        else if (sscanf(line, "say %d", &n) == 1 && n >= 0 && n < CLIP_COUNT) dust_voice_play_clip((dust_clip_t)n);
        else printf("commands: state idle|listening|thinking|speaking, talk 0-255, form body|orb|ring|wave|spiral|cloud,\n"
                    "  form text WORD, flow DX DY, swirl X Y, gather X Y, pulse X Y, scatter, calm, lift, dock, slot 1-9,\n"
                    "  cal alone|top|right|bottom|left|back (magnet calibration, once per tile),\n"
                    "  cal mic (every tile at once: speak steadily from 2 m), health, voice, wake DB (pretend), say 0-%d\n", CLIP_COUNT - 1);
    }
}

/* ---------- health: what this tile tells the others (and the Jetson) about itself ---------- */
static bool crashed_last_time(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: case ESP_RST_BROWNOUT: return true;
    default: return false;
    }
}

static void report_health(float fps, bool lifted, int64_t t_ms)
{
    static bool warned_batt;
    dust_health_t h = { .fps_x10 = (uint16_t)fminf(65535, fps * 10), .heap_kb = (uint16_t)(esp_get_free_internal_heap_size() / 1024),
                        .battery = 255, .temp_c = -128, .reset = (uint8_t)esp_reset_reason(), .can_listen = dust_voice_can_listen() };
    int pct; float temp;
    if (board_port_battery(&pct, &temp)) {
        h.battery = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
        h.temp_c = (int8_t)fmaxf(-127, fminf(127, temp));
        if (temp > 60) h.err |= DUST_ERR_HOT;
        if (pct < 10) h.err |= DUST_ERR_LOW_BATT;
        // off the wall and nearly flat: say so, once
        if (lifted && pct < 10 && !warned_batt) { dust_voice_play_clip(CLIP_LOW_BATT); warned_batt = true; }
        if (pct > 20) warned_batt = false;
    }
    h.err |= dust_voice_errors();
    if (!board_port_has_mag()) h.err |= DUST_ERR_MAG;
    if (!board_port_has_imu()) h.err |= DUST_ERR_IMU;
    if (crashed_last_time() && t_ms < 10 * 60 * 1000) h.err |= DUST_ERR_CRASHED;
    mosaic_net_set_health(&h);
}

// A lifted tile that moved in the last 3 s is in someone's hand: right by their face, so its
// bid to answer gets a little bonus.
static bool in_hand(lift_t *L, int64_t t)
{
    float g[3];
    if (!L->lifted || !board_port_accel(g)) return false;
    float dx = g[0] - L->hand_prev[0], dy = g[1] - L->hand_prev[1], dz = g[2] - L->hand_prev[2];
    memcpy(L->hand_prev, g, sizeof g);
    if (sqrtf(dx * dx + dy * dy + dz * dz) > 0.03f) L->hand_moved_ms = t;
    return t - L->hand_moved_ms < 3000;
}

/* ---------- main loop ---------- */
static inline float approach(float cur, float target, float k) { return cur + (target - cur) * k; }

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    ESP_ERROR_CHECK(board_port_init());
    for (int i = 0; i < 2; i++) s_fb[i] = board_port_alloc(DUST_PANEL_PX * DUST_PANEL_PX * 2);
    if (!s_fb[0] || !s_fb[1] || !dust_render_init(board_port_alloc)) { ESP_LOGE(TAG, "out of memory"); return; }

    int slot = load_slot();
    load_cal();
    ESP_ERROR_CHECK(mosaic_net_start(slot < 0 ? DUST_SLOT_UNKNOWN : (uint8_t)slot, CONFIG_DUST_CHANNEL, CONFIG_DUST_GAP_PX));
    xTaskCreate(console_task, "dust_cli", 3072, NULL, 3, NULL);
    lift_t lift = { .moved_ms = -1, .still_since = -1, .hand_moved_ms = -100000 };
    if (slot < 0) slot = join_screen(&lift);   // snap tiles together: positions come from the magnets
    if (dust_voice_start(slot) != ESP_OK) ESP_LOGE(TAG, "voice failed to start");
    report_health(0, false, 0);

    const bool swap = board_port_swap_bytes();
    dust_view_t v = { .slot = slot, .home_slot = slot, .gap_px = CONFIG_DUST_GAP_PX };
    float listen = 0, think = 0, ball_in = 0, ball_out = 0;
    static float warp[DUST_WARP_G * DUST_WARP_G * 2];
    bool showing_ball = false;
    int64_t last = esp_timer_get_time(), press_start = -1, fps_t = last; int frames = 0, cur = 0;
    bool was_touch = false, was_button = false;
    float fps = 0;
    esp_task_wdt_add(NULL);   // a render loop stuck for 5 s restarts the tile; the wall carries on without it

    for (;;) {
        esp_task_wdt_reset();
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last) / 1e6f; if (dt > 0.1f) dt = 0.1f; last = now_us;
        int64_t t_ms = now_us / 1000;

        // touch: tap cycles Dust's state (handy for demos), hold 1.2 s lifts / docks by hand
        int tx, ty; bool touch = board_port_touch(&tx, &ty);
        if (touch && !was_touch) press_start = t_ms;
        if (touch && press_start >= 0 && t_ms - press_start > 1200) { if (lift.lifted) s_force_dock = true; else s_force_lift = true; press_start = -1; }
        if (!touch && was_touch && press_start >= 0 && t_ms - press_start < 400) {
            wall_snapshot_t w; mosaic_net_snapshot(&w);
            dust_cmd_state((dust_state_t)((w.state + 1) % 4));
        }
        was_touch = touch;
        // the AI button: talk to Dust without the wake word (and interrupt him)
        bool button = board_port_ai_button();
        if (button && !was_button) dust_voice_push_to_talk();
        was_button = button;

        bool was_lifted = lift.lifted;
        lift_update(&lift, t_ms);
        if (lift.lifted != was_lifted) mosaic_net_set_lifted(lift.lifted);
        dust_voice_set_context(lift.lifted, in_hand(&lift, t_ms), fps);
        // the cluster was rearranged into a complete 3x3 with this tile somewhere new
        int assigned = mosaic_net_assigned_slot();
        if (assigned >= 0 && assigned != slot) { ESP_LOGI(TAG, "moved to screen %d", assigned + 1); save_slot(assigned); esp_restart(); }

        wall_snapshot_t w; mosaic_net_snapshot(&w);
        const dust_view_t *src = &w.view;
        float k = 1 - expf(-dt * 15), ks = 1 - expf(-dt * 3);
        // a lifted tile that is answering has its own mood and voice; otherwise it shows the wall's
        dust_state_t mood = w.state; float own_talk = 0;
        bool own = dust_voice_local(&mood, &own_talk);
        listen = approach(listen, mood == DUST_LISTENING ? 1 : 0, ks);
        think = approach(think, mood == DUST_THINKING ? 1 : 0, ks);
        // continuous values are eased between the leader's 20 Hz updates; discrete ones are copied
        bool first = v.frame_sx <= 0;
        v.t = src->t; v.listen = listen; v.think = think;
        v.talk = approach(v.talk, own ? own_talk : src->talk, k);
        v.assemble = w.have_sync ? src->assemble : 0;
        v.shake = src->shake; v.scatter = approach(v.scatter, src->scatter, k);
        for (int i = 0; i < 9; i++) { v.take[i] = approach(v.take[i], src->take[i], k); v.pres[i] = first ? src->pres[i] : approach(v.pres[i], src->pres[i], k); }
        if (first) { v.frame_cx = src->frame_cx; v.frame_cy = src->frame_cy; v.frame_sx = src->frame_sx; v.frame_sy = src->frame_sy; }
        v.frame_cx = approach(v.frame_cx, src->frame_cx, k); v.frame_cy = approach(v.frame_cy, src->frame_cy, k);
        v.frame_sx = approach(v.frame_sx, src->frame_sx, k); v.frame_sy = approach(v.frame_sy, src->frame_sy, k);
        v.form_a = src->form_a; v.form_b = src->form_b;
        v.form_amt = approach(v.form_amt, src->form_amt, k); v.form_swap = approach(v.form_swap, src->form_swap, k);
        for (int f = 0; f < DUST_MAX_FX; f++) {
            dust_fx_t nf = src->fx[f];
            if (nf.type == v.fx[f].type && nf.type != DUST_FX_NONE) {   // same effect: ease its numbers
                nf.amount = approach(v.fx[f].amount, nf.amount, k); nf.phase = approach(v.fx[f].phase, nf.phase, k);
                nf.release = approach(v.fx[f].release, nf.release, k);
            }
            v.fx[f] = nf;
        }
        // bend field: the leader says which two screen sets to blend between and how far
        if (w.warp_to == 0x1FF && w.warp_from == 0x1FF) v.warp = NULL;
        else { dust_warp_blend(w.warp_from, w.warp_to, w.warp_k, CONFIG_DUST_GAP_PX, warp); v.warp = warp; }

        // lifted: become the ball. Put back: the ball pours out, then the wall slice returns.
        // A tile that lost a slot clash stands aside the same way until it's given its own slot.
        bool apart = lift.lifted || w.self_dup;
        if (apart) {
            if (!showing_ball) { showing_ball = true; ball_in = -0.25f; ball_out = 0; }
            ball_in = fminf(1.2f, ball_in + dt / 1.9f);
        } else if (showing_ball) {
            ball_out = fminf(1, ball_out + dt * 1.6f);
            if (ball_out >= 1) showing_ball = false;
        }
        v.slot = showing_ball ? -1 : slot; v.ball_in = ball_in; v.ball_out = apart ? 0 : ball_out;

        if (src->frame_sx <= 0 && !showing_ball) {   // no wall state yet: stay dark
            memset(s_fb[cur], 0, DUST_PANEL_PX * DUST_PANEL_PX * 2);
        } else {
            dust_render(&v, s_fb[cur], swap);
        }
        // problems you can see: red across the top, another tile has this slot ("slot N" to fix);
        // amber across the bottom, this tile's firmware is older than its neighbours'
        if (w.self_dup) fill_rect(s_fb[cur], 0, 0, DUST_PANEL_PX, 10, rgb565(255, 60, 60, swap));
        if (mosaic_net_outdated()) fill_rect(s_fb[cur], 0, DUST_PANEL_PX - 10, DUST_PANEL_PX, 10, rgb565(255, 171, 74, swap));
        board_port_flush(s_fb[cur]);
        cur ^= 1;

        if (++frames == 60) {
            fps = 60e6f / (now_us - fps_t);
            ESP_LOGI(TAG, "screen %d %s (epoch %lu)%s · %.1f fps · %d dots", slot + 1, w.am_leader ? "leads" : "follows", (unsigned long)w.epoch,
                     lift.lifted ? " · lifted" : "", fps, dust_render_last_count());
            report_health(fps, lift.lifted, t_ms);
            frames = 0; fps_t = now_us;
        }
        vTaskDelay(1);
    }
}
