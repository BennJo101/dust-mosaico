// Dust renderer: draws one panel of the 3x3 wall (or the ball on a lifted panel) into a
// 240x240 light buffer, adds a cheap bloom and writes a 480x480 RGB565 frame.
//
// Wall pipeline, per dot (mirrors the web simulator's shader):
//   3D body → project into wall space (uv: 0..1 across the whole wall) → blend toward the
//   AI's shape → the AI's effects (flow, swirl, gather, pulse) → the bend field squeezes
//   everything out of missing screens → the container catches any stragglers → this dot's ninth
//   drains into a lifted screen → this panel's pixels.
//
// Every panel runs this with the same dust_view_t and its own slot, so the slices line up.
#include "dust_render.h"
#include "dust_points.h"
#include "dust_warp.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

#define PI_F 3.14159265f
#define HEADY 0.66f
#define CAM_FOV_HALF_TAN 0.28674539f            // tan(16 deg): 32 deg vertical field of view
#define WALL_CAM_Y 0.12f
#define WALL_CAM_D (1.15f / (CAM_FOV_HALF_TAN * 0.94f))
#define BALL_CAM_D (0.86f / CAM_FOV_HALF_TAN)

#define GLOW_RES (DUST_RES / 4)                  // 60x60 bloom buffer
#define BALL_SHELL 2600
#define BALL_CORE  600
#define RING_PTS   240
#define FIELD_PTS  480

typedef struct { float x, y, z, nx, ny, nz; uint8_t kind, phase, fade, warm, delay, rnd; } fpoint_t;

static uint16_t *s_acc;          // DUST_RES*DUST_RES*3, additive light
static uint16_t *s_glow;         // GLOW_RES*GLOW_RES*3
static uint16_t *s_glow_tmp;
static fpoint_t *s_ball;         // generated once
static uint16_t *s_cull;         // dots whose settled position can land on this panel
static int       s_cull_n, s_cull_slot = -2;
static float     s_cull_key[13];
static int       s_last_count;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth01(float v) { v = clampf(v, 0, 1); return v * v * (3 - 2 * v); }
static inline float smoothstepf(float a, float b, float v) { return smooth01((v - a) / (b - a)); }
static inline float fractf(float v) { return v - floorf(v); }
static inline float mixf(float a, float b, float k) { return a + (b - a) * k; }

// 1024-entry sine table: the hot loops call sine many times per dot
static float s_sin_lut[1025];
static void init_lut(void) { for (int i = 0; i <= 1024; i++) s_sin_lut[i] = sinf(i * (2 * PI_F / 1024)); }
static inline float fsin(float x)
{
    float u = x * (1024.f / (2 * PI_F));
    float fl = floorf(u); int i = ((int)fl) & 1023; float f = u - fl;
    return s_sin_lut[i] + (s_sin_lut[i + 1] - s_sin_lut[i]) * f;
}
static inline float fcos(float x) { return fsin(x + PI_F * 0.5f); }

static uint32_t s_rng = 0x2545F491u;
static float frand(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return (s_rng & 0xFFFFFF) / 16777216.0f; }

// deterministic per-dot random numbers (same on every panel)
static inline uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static inline float hf(uint32_t i, uint32_t salt) { return (hash32(i * 0x9E3779B9u + salt * 0x85EBCA6Bu) & 0xFFFFFF) / 16777216.0f; }

/* ---------- shapes ---------- */
// 5x7 font, column bytes, bit 0 = top row
static const struct { char c; uint8_t col[5]; } FONT[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00}},{'!',{0x00,0x00,0x5F,0x00,0x00}},{',',{0x00,0x50,0x30,0x00,0x00}},
    {'-',{0x08,0x08,0x08,0x08,0x08}},{'.',{0x00,0x60,0x60,0x00,0x00}},{'?',{0x02,0x01,0x51,0x09,0x06}},
    {'0',{0x3E,0x51,0x49,0x45,0x3E}},{'1',{0x00,0x42,0x7F,0x40,0x00}},{'2',{0x42,0x61,0x51,0x49,0x46}},
    {'3',{0x21,0x41,0x45,0x4B,0x31}},{'4',{0x18,0x14,0x12,0x7F,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
    {'6',{0x3C,0x4A,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},{'8',{0x36,0x49,0x49,0x49,0x36}},
    {'9',{0x06,0x49,0x49,0x29,0x1E}},{'A',{0x7E,0x11,0x11,0x11,0x7E}},{'B',{0x7F,0x49,0x49,0x49,0x36}},
    {'C',{0x3E,0x41,0x41,0x41,0x22}},{'D',{0x7F,0x41,0x41,0x22,0x1C}},{'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'F',{0x7F,0x09,0x09,0x09,0x01}},{'G',{0x3E,0x41,0x49,0x49,0x7A}},{'H',{0x7F,0x08,0x08,0x08,0x7F}},
    {'I',{0x00,0x41,0x7F,0x41,0x00}},{'J',{0x20,0x40,0x41,0x3F,0x01}},{'K',{0x7F,0x08,0x14,0x22,0x41}},
    {'L',{0x7F,0x40,0x40,0x40,0x40}},{'M',{0x7F,0x02,0x0C,0x02,0x7F}},{'N',{0x7F,0x04,0x08,0x10,0x7F}},
    {'O',{0x3E,0x41,0x41,0x41,0x3E}},{'P',{0x7F,0x09,0x09,0x09,0x06}},{'Q',{0x3E,0x41,0x51,0x21,0x5E}},
    {'R',{0x7F,0x09,0x19,0x29,0x46}},{'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7F,0x01,0x01}},
    {'U',{0x3F,0x40,0x40,0x40,0x3F}},{'V',{0x1F,0x20,0x40,0x20,0x1F}},{'W',{0x3F,0x40,0x38,0x40,0x3F}},
    {'X',{0x63,0x14,0x08,0x14,0x63}},{'Y',{0x07,0x08,0x70,0x08,0x07}},{'Z',{0x61,0x51,0x49,0x45,0x43}},
};

typedef struct { char text[DUST_TEXT_MAX + 1]; uint8_t x[DUST_TEXT_MAX * 35], y[DUST_TEXT_MAX * 35]; int n, w; bool valid; } textcells_t;
static textcells_t s_text[2];

static const textcells_t *text_cells(int which, const char *txt)
{
    textcells_t *tc = &s_text[which];
    if (tc->valid && strncmp(tc->text, txt, DUST_TEXT_MAX) == 0) return tc;
    memset(tc, 0, sizeof *tc);
    strncpy(tc->text, txt, DUST_TEXT_MAX);
    int len = (int)strlen(tc->text);
    for (int k = 0; k < len; k++) {
        char ch = (char)toupper((unsigned char)tc->text[k]);
        for (size_t f = 0; f < sizeof FONT / sizeof FONT[0]; f++) if (FONT[f].c == ch) {
            for (int cx = 0; cx < 5; cx++) for (int cy = 0; cy < 7; cy++)
                if (FONT[f].col[cx] >> cy & 1) { tc->x[tc->n] = (uint8_t)(k * 6 + cx); tc->y[tc->n] = (uint8_t)cy; tc->n++; }
            break;
        }
    }
    tc->w = len > 0 ? len * 6 - 1 : 1;
    tc->valid = true;
    return tc;
}

// local target for dot i in a shape: x, y in about -1..1, y up
static void form_target(const dust_form_spec_t *f, int which, uint32_t i, float *ox, float *oy)
{
    float a = hf(i, 11), b = hf(i, 12), c = hf(i, 13);
    switch (f->id) {
    case DUST_FORM_ORB: { float r = b < 0.7f ? 0.62f * (0.9f + 0.1f * c) : 0.62f * sqrtf(c); float an = a * 2 * PI_F; *ox = cosf(an) * r; *oy = sinf(an) * r; return; }
    case DUST_FORM_RING: { float an = a * 2 * PI_F, r = 0.7f + (b - 0.5f) * 0.09f; *ox = cosf(an) * r; *oy = sinf(an) * r; return; }
    case DUST_FORM_WAVE: { float x = a * 1.9f - 0.95f; *ox = x; *oy = 0.26f * sinf(x * 5.4f) + (b - 0.5f) * 0.14f; return; }
    case DUST_FORM_SPIRAL: { float arm = b < 0.5f ? 0 : PI_F, r = powf(a, 0.7f) * 0.9f, an = r * 11 + arm;
                             *ox = cosf(an) * r + (c - 0.5f) * 0.05f; *oy = sinf(an) * r + (hf(i, 14) - 0.5f) * 0.05f; return; }
    case DUST_FORM_CLOUD: { // rejection-free: a lumpy ellipse
                             float an = a * 2 * PI_F, r = sqrtf(b) * (0.95f + 0.12f * sinf(an * 3 + 1.3f) + 0.08f * sinf(an * 5));
                             *ox = cosf(an) * r * 0.85f; *oy = sinf(an) * r * 0.5f; return; }
    case DUST_FORM_TEXT: {
        const textcells_t *tc = text_cells(which, f->text);
        if (tc->n == 0) { *ox = *oy = 0; return; }
        int k = (int)(a * tc->n); if (k >= tc->n) k = tc->n - 1;
        float half = tc->w / 2.f;
        *ox = (tc->x[k] + 0.5f + (b - 0.5f) * 0.9f - tc->w / 2.f) / half;
        *oy = -(tc->y[k] + 0.5f + (c - 0.5f) * 0.9f - 3.5f) / half;
        return; }
    default: *ox = *oy = 0; return;
    }
}

static void form_motion(int id, float *spin, float *wave)
{
    static const float M[DUST_FORM_COUNT][2] = { {0,0}, {0.25f,0}, {0.35f,0}, {0,1}, {0.5f,0}, {0.05f,0.35f}, {0,0.12f} };
    *spin = id < DUST_FORM_COUNT ? M[id][0] : 0; *wave = id < DUST_FORM_COUNT ? M[id][1] : 0;
}

// How big a shape is drawn inside Dust's current frame (same rule as the simulator).
static void form_scale(const dust_view_t *v, float *sx, float *sy)
{
    float fx = v->frame_sx, fy = v->frame_sy, m = fminf(fx, fy);
    switch (v->form_b.id) {
    case DUST_FORM_TEXT: { const textcells_t *tc = text_cells(1, v->form_b.text);
        float aspect = 7.f / (tc->w > 0 ? tc->w : 1), w = fx * 0.98f;
        if (w * aspect > fy * 0.9f) w = fy * 0.9f / aspect;
        *sx = *sy = w; return; }
    case DUST_FORM_WAVE: case DUST_FORM_CLOUD: *sx = fx * 0.95f; *sy = m * 0.95f; return;
    default: *sx = *sy = m; return;
    }
}

int dust_form_from_name(const char *name)
{
    static const char *N[DUST_FORM_COUNT] = { "body", "orb", "ring", "wave", "spiral", "cloud", "text" };
    for (int i = 0; i < DUST_FORM_COUNT; i++) {
        const char *a = name, *b = N[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

/* ---------- init ---------- */
static void build_ball(void);
bool dust_render_init(void *(*alloc_fn)(size_t))
{
    s_acc      = alloc_fn(DUST_RES * DUST_RES * 3 * sizeof(uint16_t));
    s_glow     = alloc_fn(GLOW_RES * GLOW_RES * 3 * sizeof(uint16_t));
    s_glow_tmp = alloc_fn(GLOW_RES * GLOW_RES * 3 * sizeof(uint16_t));
    s_ball     = alloc_fn((BALL_SHELL + BALL_CORE) * sizeof(fpoint_t));
    s_cull     = alloc_fn(DUST_POINT_COUNT * sizeof(uint16_t));
    if (!s_acc || !s_glow || !s_glow_tmp || !s_ball || !s_cull) return false;
    init_lut();
    build_ball();
    return true;
}

int dust_render_last_count(void) { return s_last_count; }

static void build_ball(void)
{
    int n = 0;
    for (int i = 0; i < BALL_SHELL; i++, n++) {
        float a, b, c, d;
        do { a = frand() * 2 - 1; b = frand() * 2 - 1; c = frand() * 2 - 1; d = a * a + b * b + c * c; } while (d > 1 || d < 1e-4f);
        d = sqrtf(d); a /= d; b /= d; c /= d;
        if (frand() < 0.35f) { b = roundf(b * 20) / 20; float r = sqrtf(fmaxf(0, 1 - b * b)), h = sqrtf(a * a + c * c); if (h > 1e-5f) { a = a / h * r; c = c / h * r; } }
        float r = 0.6f * (1 + (frand() - 0.5f) * 0.04f);
        s_ball[n] = (fpoint_t){ a * r, b * r + HEADY, c * r, a, b, c, 0, (uint8_t)(frand() * 255), 255,
                                (uint8_t)(fmaxf(0, c - 0.3f) * 0.9f * 255), (uint8_t)(frand() * 255), (uint8_t)(frand() * 255) };
    }
    for (int i = 0; i < BALL_CORE; i++, n++) {
        float a, b, c, d;
        do { a = frand() * 2 - 1; b = frand() * 2 - 1; c = frand() * 2 - 1; d = a * a + b * b + c * c; } while (d > 1);
        s_ball[n] = (fpoint_t){ a * 0.36f, b * 0.36f + HEADY, c * 0.36f + 0.05f, 0, 0, 1, 3, (uint8_t)(frand() * 255), 230, 255,
                                (uint8_t)(frand() * 255), (uint8_t)(frand() * 255) };
    }
}

/* ---------- camera ---------- */
typedef struct { float cy, cp, sy, sp, cam_y, cam_d; } cam_t;

// model -> normalized device coords (-1..1, y up). False when behind the camera.
static inline bool cam_ndc(const cam_t *c, float x, float y, float z, float *nx, float *ny, float *depth)
{
    float x1 = x * c->cy + z * c->sy, z1 = -x * c->sy + z * c->cy;    // yaw
    float y1 = y * c->cp - z1 * c->sp, z2 = y * c->sp + z1 * c->cp;   // pitch
    float zc = c->cam_d - z2;
    if (zc < 0.2f) return false;
    float k = 1.0f / (zc * CAM_FOV_HALF_TAN);
    *nx = x1 * k; *ny = (y1 - c->cam_y) * k; *depth = c->cam_d / zc;
    return true;
}

/* ---------- wall geometry (uv) ---------- */
typedef struct { float cell, pitch; } wallgeo_t;
static inline wallgeo_t wall_geo(float gap_px)
{
    float wall = 3 * DUST_PANEL_PX + 2 * gap_px;
    return (wallgeo_t){ DUST_PANEL_PX / wall, (DUST_PANEL_PX + gap_px) / wall };
}

// The container: a dot on a missing screen pours into the nearest screen that's there,
// settling a little way in.
static void contain(const dust_view_t *v, const wallgeo_t *g, float depth01, float *u, float *w)
{
    if (*u < 0 || *w < 0 || *u > 1 || *w > 1) return;
    int cx = (int)(*u / g->pitch), cy = (int)(*w / g->pitch);
    cx = cx > 2 ? 2 : cx; cy = cy > 2 ? 2 : cy;
    float pres = v->pres[cy * 3 + cx];
    if (pres > 0.999f) return;
    float best = 1e9f, tu = *u, tw = *w, m = g->cell * 0.04f, depth = g->cell * 0.42f * depth01;
    for (int j = 0; j < 9; j++) {
        if (v->pres[j] < 0.5f) continue;
        float lou = (j % 3) * g->pitch + m, low = (j / 3) * g->pitch + m, hiu = lou + g->cell - 2 * m, hiw = low + g->cell - 2 * m;
        float qu = clampf(*u, lou, hiu), qw = clampf(*w, low, hiw);
        float du = qu - *u, dw = qw - *w, d = sqrtf(du * du + dw * dw);
        if (d < best) {
            best = d;
            float iu = d > 1e-5f ? du / d : 0, iw = d > 1e-5f ? dw / d : 0;
            tu = clampf(qu + iu * depth, lou, hiu); tw = clampf(qw + iw * depth, low, hiw);
        }
    }
    if (best > 1e8f) return;
    *u = mixf(*u, tu, 1 - pres); *w = mixf(*w, tw, 1 - pres);
}

// The AI's hands. Returns an alpha multiplier (flowing dots fade at the ends of their run).
static float apply_fx(const dust_view_t *v, uint32_t i, float ph, float *u, float *w)
{
    float fade = 1;
    for (int k = 0; k < DUST_MAX_FX; k++) {
        const dust_fx_t *f = &v->fx[k];
        if (f->type == DUST_FX_NONE || f->amount < 0.001f) continue;
        const float hu = *u, hw = *w;
        float du = *u - f->x, dw = *w - f->y, r = sqrtf(du * du + dw * dw), fd = 1;
        switch (f->type) {
        case DUST_FX_FLOW: {
            float kk = fractf(v->t * 0.22f + hf(i, 21));
            float dl = sqrtf(f->dx * f->dx + f->dy * f->dy) + 1e-5f, dxn = f->dx / dl, dyn = f->dy / dl;
            float side = fsin(kk * 9 + ph * 3) * 0.02f * f->amount;
            *u += dxn * (kk - 0.5f) * 0.55f * f->amount - dyn * side;
            *w += dyn * (kk - 0.5f) * 0.55f * f->amount + dxn * side;
            fd = mixf(1, smoothstepf(0, 0.15f, kk) * (1 - smoothstepf(0.8f, 1, kk)), f->amount);
            break; }
        case DUST_FX_SWIRL: {
            float fall = expf(-r * r / (f->radius * f->radius)), an = f->phase * fall * f->amount, ca = fcos(an), sa = fsin(an);
            *u = f->x + du * ca - dw * sa; *w = f->y + du * sa + dw * ca;
            break; }
        case DUST_FX_GATHER: {
            float fall = expf(-r * r / (f->radius * f->radius * 4)), keep = 0.12f + 0.1f * hf(i, 22), k2 = f->amount * fall;
            *u = mixf(*u, f->x + du * keep, k2); *w = mixf(*w, f->y + dw * keep, k2);
            break; }
        case DUST_FX_PULSE: {
            float front = f->phase * 0.55f, q = (r - front) / 0.06f;
            float amp = expf(-q * q) * expf(-f->phase * 1.2f) * f->amount * 0.06f;
            if (r > 1e-5f) { *u += du / r * amp; *w += dw / r * amp; }
            break; }
        }
        // release: the effect stays where it ended, and each dot drifts home on its own,
        // staggered, wandering a little on the way, like dust settling (never a rewind)
        if (f->release > 0) {
            float kk = smooth01(f->release * 1.7f - hf(i, 50 + k) * 0.7f), s = fsin(kk * PI_F) * 0.03f;
            *u = mixf(*u, hu, kk) + s * fsin(v->t * .9f + ph * 3 + k);
            *w = mixf(*w, hw, kk) + s * fcos(v->t * .8f + ph * 2 + k);
            fd = mixf(fd, 1, kk);
        }
        fade *= fd;
    }
    return fade;
}

/* ---------- splat ---------- */
static inline void splat(float fx, float fy, float r, float g, float b, float a)
{
    // fx, fy in internal pixels; bilinear footprint gives soft sub-pixel dots
    if (fx < 0 || fy < 0 || fx >= DUST_RES - 1 || fy >= DUST_RES - 1 || a <= 0.004f) return;
    int ix = (int)fx, iy = (int)fy;
    float ax = fx - ix, ay = fy - iy;
    float w[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
    int base[4] = { iy * DUST_RES + ix, iy * DUST_RES + ix + 1, (iy + 1) * DUST_RES + ix, (iy + 1) * DUST_RES + ix + 1 };
    float s = a * 255.f * 2.2f;
    for (int k = 0; k < 4; k++) {
        uint16_t *px = &s_acc[base[k] * 3];
        float ws = w[k] * s;
        uint32_t v;
        v = px[0] + (uint32_t)(r * ws); px[0] = v > 65535 ? 65535 : (uint16_t)v;
        v = px[1] + (uint32_t)(g * ws); px[1] = v > 65535 ? 65535 : (uint16_t)v;
        v = px[2] + (uint32_t)(b * ws); px[2] = v > 65535 ? 65535 : (uint16_t)v;
    }
}

/* ---------- shading (mirrors the web shader) ---------- */
static const float ICE[3] = { .37f, .89f, 1.f }, TEAL[3] = { .10f, .40f, .52f }, EMBER[3] = { 1.f, .56f, .2f }, WHITE[3] = { .86f, .97f, 1.f };

static void shade(const fpoint_t *p, float nzr, float nxr, const dust_view_t *v, float ph, float *rgb, float *alpha)
{
    float t = v->t;
    if (p->kind == 3) {
        float k = .25f + .55f * v->talk;
        static const float WARM2[3] = { 1.f, .82f, .55f };
        for (int i = 0; i < 3; i++) rgb[i] = mixf(EMBER[i], WARM2[i], k);
        *alpha = .32f + .6f * v->talk + .25f * v->think + .12f * fsin(t * 3 + ph);
        return;
    }
    float rim = clampf(1 - fabsf(nzr), 0, 1), r = rim * sqrtf(sqrtf(rim)), r5 = r * r * r * r * r * .55f;
    float wm = (p->warm / 255.f) * fmaxf(nzr, 0), em = smoothstepf(.25f, .75f, wm) * (.7f + .3f * v->talk);
    for (int i = 0; i < 3; i++) {
        float c = mixf(TEAL[i], ICE[i], r);
        c = mixf(c, WHITE[i], r5);
        rgb[i] = mixf(c, EMBER[i], em);
    }
    float fade = p->fade / 255.f;
    *alpha = fade * (nzr < 0 ? .38f : (p->kind == 0 ? .68f : .55f) + .4f * r) + .5f * v->talk * (p->warm / 255.f);
    if (p->kind == 0 && v->listen > .01f)
        *alpha += .35f * v->listen * nxr * nxr * (.6f + .4f * fsin(t * 6 - fabsf(p->x) * 12));
}

/* ---------- per-dot 3D motion (cheap stand-in for the web version's curl noise) ---------- */
static void animate(const fpoint_t *p, const dust_view_t *v, float ph, float *x, float *y, float *z)
{
    float t = v->t;
    *x = p->x; *y = p->y; *z = p->z;
    float fade = p->fade / 255.f;
    if (p->kind == 3) {   // ember core spins
        float a = t * (.25f + 1.4f * v->think), ca = fcos(a), sa = fsin(a), qz = *z - .05f;
        float nx = *x * ca + qz * sa, nz = -*x * sa + qz * ca;
        *x = nx; *z = nz + .05f;
        *y += fsin(t * 2 + ph) * .02f * (1 + v->think);
    }
    float amp = p->kind == 2 ? .006f + (1 - fade) * .16f : .005f + .012f * v->think + .008f * v->talk;
    amp *= 1.6f;
    *x += fsin(t * .7f + ph + p->y * 2.3f) * amp;
    *y += fsin(t * .53f + ph * 1.7f + p->z * 2.9f) * amp;
    *z += fcos(t * .61f + ph * 2.3f + p->x * 2.1f) * amp;
    if (p->kind == 2) *y += (1 - fade) * .22f * fractf(t * .035f + ph * .159f);
    float breath = fsin(t * 1.15f) * .012f;
    if (p->kind == 2) { *y += breath; *x *= 1 + breath * .6f; }
    else if (p->kind == 1) *y += breath * .5f;
    if (p->kind == 0) { float s = 1 + .012f * v->listen * fsin(t * 3.2f); *x *= s; *y *= s; *z *= s; }
}

static inline float flicker(const dust_view_t *v, float ph)
{
    float a = .78f + .22f * fsin(v->t * 2.7f + ph * 7);
    if (v->shake > .001f && fractf(fsin(ph * 37.1f + floorf(v->t * 18)) * 9137.3f) > .7f) a *= 1 - .5f * v->shake;
    return a;
}

static inline void decode(const dust_point_t *bp, fpoint_t *p)
{
    *p = (fpoint_t){ bp->x / 8192.f, bp->y / 8192.f, bp->z / 8192.f, bp->nx / 127.f, bp->ny / 127.f, bp->nz / 127.f,
                     bp->kind, bp->phase, bp->fade, bp->warm, bp->delay, bp->rnd };
}

static inline void to_uv(const dust_view_t *v, float nx, float ny, float *u, float *w)
{
    *u = v->frame_cx + nx * 0.5f * v->frame_sx;
    *w = v->frame_cy - ny * 0.5f * v->frame_sy;
}

/* ---------- which dots can land on this panel ---------- */
static bool view_is_dynamic(const dust_view_t *v)
{
    if (v->assemble < 1.2f || v->form_amt > 0.001f || v->scatter > 0.001f || v->shake > 0.02f) return true;
    for (int k = 0; k < DUST_MAX_FX; k++) if (v->fx[k].type && v->fx[k].amount > 0.001f) return true;
    for (int g = 0; g < 9; g++) {
        if (v->take[g] > 0.001f && v->take[g] < 0.999f) return true;
        if (v->pres[g] > 0.01f && v->pres[g] < 0.99f) return true;
    }
    return false;
}

static void build_cull(const dust_view_t *v, const wallgeo_t *g)
{
    float key[13] = { (float)v->slot, v->frame_cx, v->frame_cy, v->frame_sx, v->frame_sy };
    for (int j = 0; j < 8; j++) key[5 + j] = v->pres[j] > 0.5f ? 1.f : 0.f;
    if (v->pres[8] < 0.5f) key[12] += 2;   // fold the 9th screen in
    bool same = s_cull_slot == v->slot;
    for (int j = 0; j < 13 && same; j++) if (fabsf(key[j] - s_cull_key[j]) > 0.002f) same = false;
    if (same) return;
    memcpy(s_cull_key, key, sizeof key);
    s_cull_slot = v->slot; s_cull_n = 0;
    cam_t c = { 1, 1, 0, 0, WALL_CAM_Y, WALL_CAM_D };
    float ou = (v->slot % 3) * g->pitch, ow = (v->slot / 3) * g->pitch, m = g->cell * 0.4f;
    for (int i = 0; i < DUST_POINT_COUNT; i++) {
        const dust_point_t *p = &DUST_POINTS[i];
        float nx, ny, d, u, w;
        if (!cam_ndc(&c, p->x / 8192.f, p->y / 8192.f, p->z / 8192.f, &nx, &ny, &d)) continue;
        to_uv(v, nx, ny, &u, &w);
        if (v->warp) { float du, dw; dust_warp_sample(v->warp, u, w, &du, &dw); u += du; w += dw; }
        contain(v, g, 0.5f, &u, &w);
        if (u > ou - m && u < ou + g->cell + m && w > ow - m && w < ow + g->cell + m) s_cull[s_cull_n++] = (uint16_t)i;
    }
}

/* ---------- wall slice ---------- */
static void render_wall(const dust_view_t *v)
{
    const wallgeo_t g = wall_geo(v->gap_px);
    const bool full = view_is_dynamic(v);
    if (!full) build_cull(v, &g);
    const int count = full ? DUST_POINT_COUNT : s_cull_n;

    float yaw = fsin(v->t * .21f) * .28f, pitch = fsin(v->t * .17f) * .04f;
    cam_t c = { fcos(yaw), fcos(pitch), fsin(yaw), fsin(pitch), WALL_CAM_Y, WALL_CAM_D };
    const float ou = (v->slot % 3) * g.pitch, ow = (v->slot / 3) * g.pitch;
    const float to_px = DUST_RES / g.cell;                    // uv -> internal pixels
    const float margin = 0.08f * g.cell;
    const float density = .5f + .5f * clampf(0.5f * (v->frame_sx + v->frame_sy), 0, 1);
    float fsx = 1, fsy = 1, spin = 0, wave = 0;
    if (v->form_amt > 0.001f) { form_scale(v, &fsx, &fsy); form_motion(v->form_b.id, &spin, &wave); }
    const float sa = fsin(spin * v->t), ca = fcos(spin * v->t);

    int drawn = 0;
    for (int j = 0; j < count; j++) {
        const int i = full ? j : s_cull[j];
        const dust_point_t *bp = &DUST_POINTS[i];
        fpoint_t p; decode(bp, &p);
        float ph = p.phase * (2 * PI_F / 255.f), delay = p.delay * (.55f / 255.f);
        float x, y, z;
        animate(&p, v, ph, &x, &y, &z);

        // startup pour: rise from below in a swirl, bottom first
        if (v->assemble < 1.2f) {
            float k = smooth01((v->assemble - delay) / .45f);
            float r = .4f + (p.rnd / 255.f) * 2.8f, sxx = fcos(ph) * r, szz = fsin(ph) * r * .6f, syy = -2.1f - (p.rnd / 255.f) * .8f;
            float a = (1 - k) * 2.6f, cA = fcos(a), sA = fsin(a);
            float fx = sxx * cA + szz * sA, fz = -sxx * sA + szz * cA;
            x = mixf(fx, x, k); y = mixf(syy, y, k); z = mixf(fz, z, k);
        }

        float nx, ny, depth, u, w;
        if (!cam_ndc(&c, x, y, z, &nx, &ny, &depth)) continue;
        to_uv(v, nx, ny, &u, &w);

        // shapes
        float kf = 0;
        if (v->form_amt > 0.001f) {
            kf = smooth01(v->form_amt * 1.5f - delay);
            if (kf > 0) {
                float ks = smooth01(v->form_swap * 1.5f - delay), ax, ay, bx, by;
                form_target(&v->form_a, 0, (uint32_t)i, &ax, &ay);
                form_target(&v->form_b, 1, (uint32_t)i, &bx, &by);
                float lx = mixf(ax, bx, ks), ly = mixf(ay, by, ks);
                float rx = lx * ca - ly * sa, ry = lx * sa + ly * ca;
                ry += wave * 0.1f * fsin(rx * 6 + v->t * 2.2f + ph * 0.3f);
                rx += fsin(v->t * .6f + ph * 3) * 0.006f; ry += fcos(v->t * .5f + ph * 2) * 0.006f;
                u = mixf(u, v->frame_cx + rx * 0.5f * fsx, kf);
                w = mixf(w, v->frame_cy - ry * 0.5f * fsy, kf);
            }
        }

        float fade = apply_fx(v, (uint32_t)i, ph, &u, &w);
        if (v->warp) { float du, dw; dust_warp_sample(v->warp, u, w, &du, &dw); u += du; w += dw; }
        contain(v, &g, powf(p.rnd / 255.f, 1.6f), &u, &w);

        if (v->scatter > 0.001f) {
            float s = smooth01(v->scatter * 1.5f - delay), an = hf((uint32_t)i, 31) * 2 * PI_F, rr = 0.6f + hf((uint32_t)i, 32) * 0.6f;
            u = mixf(u, v->frame_cx + fcos(an) * rr, s); w = mixf(w, v->frame_cy + fsin(an) * rr, s);
        }
        if (v->shake > .001f) {
            float gl = fractf(fsin(ph * 91.7f + floorf(v->t * 22)) * 43758.5f) > .8f ? 1.f : 0.f;
            u += v->shake * (0.006f * fsin(v->t * 41 + ph * 7) + gl * 0.02f * fsin(ph * 13));
            w += v->shake * 0.006f * fsin(v->t * 37 + ph * 11);
        }

        // this dot's ninth drains into its screen when that screen is lifted or removed
        float tk = 0;
        const int grp = bp->group;
        if (grp < 9 && v->take[grp] > 0.001f) {
            tk = smooth01(v->take[grp] * 1.7f - delay * 1.3f);
            if (tk >= 0.999f) continue;
            float tu = (grp % 3) * g.pitch + g.cell * 0.5f, tw = (grp / 3) * g.pitch + g.cell * 0.5f, bend = fsin(tk * PI_F) * 0.05f;
            u = mixf(u, tu, tk) + bend * fsin(ph * 3.1f);
            w = mixf(w, tw, tk) + bend * fcos(ph * 2.3f);
        }

        float lu = u - ou, lw = w - ow;
        if (lu < -margin || lw < -margin || lu > g.cell + margin || lw > g.cell + margin) continue;

        float nzr = -p.nx * c.sy + p.nz * c.cy, nxr = p.nx * c.cy + p.nz * c.sy;
        float rgb[3], a;
        shade(&p, nzr, nxr, v, ph, rgb, &a);
        if (kf > 0) a = mixf(a, fmaxf(a, 0.5f + 0.3f * hf((uint32_t)i, 41)), kf);   // in a shape every dot reads the same
        if (tk > 0) {
            float s = fsin(tk * PI_F);
            for (int k = 0; k < 3; k++) rgb[k] = mixf(rgb[k], WHITE[k], s * .6f);
            a = mixf(a, a * 1.6f, s) * (1 - smoothstepf(.88f, 1, tk));
        }
        a *= fade * flicker(v, ph) * (.55f + .45f * clampf(depth, 0, 1.4f)) * .85f * density;
        if (v->scatter > 0.001f) a *= mixf(1, .8f, v->scatter);
        splat(lu * to_px, lw * to_px, rgb[0], rgb[1], rgb[2], a);
        drawn++;
    }

    // think ring and listening field, generated on the fly (they follow the body, not shapes)
    if ((v->think > .01f || v->listen > .01f) && v->form_amt < 0.5f) {
        for (int i = 0; i < RING_PTS + FIELD_PTS; i++) {
            float ph = fractf(fsin(i * 12.9898f) * 43758.5f) * 2 * PI_F;
            float x, y, z, a; const float *col;
            if (i < RING_PTS) {
                if (v->think < .01f) continue;
                float an = (float)i / RING_PTS * 2 * PI_F + v->t * (.5f + .6f * v->think), r = .92f + .05f * fsin(an * 3 + v->t);
                x = fcos(an) * r; z = fsin(an) * r; y = HEADY + fsin(an) * .16f + fsin(an * 2 + v->t * .7f) * .03f;
                a = v->think * (.55f + .35f * fsin(ph * 5 + v->t * 4)); col = WHITE;
            } else {
                if (v->listen < .01f) continue;
                float uu = fractf(fsin(i * 78.233f) * 12345.6f) * 2 - 1, th = ph;
                float dy = uu * .55f, rr = sqrtf(fmaxf(0, 1 - uu * uu)), dx = fcos(th) * rr, dz = fsin(th) * rr, h = sqrtf(dx * dx + dy * dy + dz * dz);
                float f = fractf(v->t * .42f + ph * .159f), s = smooth01(f), r = mixf(1.9f, .5f, s);
                x = dx / h * r; y = HEADY + dy / h * r; z = dz / h * r;
                a = v->listen * smoothstepf(0, .3f, f) * (1 - smoothstepf(.82f, 1, f)) * .85f; col = ICE;
            }
            float nx, ny, depth, u, w;
            if (!cam_ndc(&c, x, y, z, &nx, &ny, &depth)) continue;
            to_uv(v, nx, ny, &u, &w);
            if (v->warp) { float du, dw; dust_warp_sample(v->warp, u, w, &du, &dw); u += du; w += dw; }
            contain(v, &g, 0.3f, &u, &w);
            splat((u - ou) * to_px, (w - ow) * to_px, col[0], col[1], col[2], a * .62f * (1 - 2 * v->form_amt));
        }
    }
    s_last_count = drawn;
}

/* ---------- ball (the lifted panel) ---------- */
static void render_ball(const dust_view_t *v)
{
    float spin = v->t * .35f, pitch = fsin(v->t * .17f) * .04f;
    cam_t c = { fcos(spin), fcos(pitch), fsin(spin), fsin(pitch), HEADY, BALL_CAM_D };
    const float P = DUST_PANEL_PX, to_internal = (float)DUST_RES / DUST_PANEL_PX;

    // dots pour in from the edge that faced the rest of the wall
    int r0 = v->home_slot / 3, c0 = v->home_slot % 3;
    float ex = (float)(1 - c0), ey = (float)(r0 - 1);
    if (ex == 0 && ey == 0) ey = 1;
    float eh = sqrtf(ex * ex + ey * ey); ex /= eh; ey /= eh;

    int drawn = 0;
    for (int i = 0; i < BALL_SHELL + BALL_CORE; i++) {
        const fpoint_t *p = &s_ball[i];
        float ph = p->phase * (2 * PI_F / 255.f), delay = p->delay * (.5f / 255.f);
        float x, y, z, nx, ny, depth;
        animate(p, v, ph, &x, &y, &z);
        if (!cam_ndc(&c, x, y, z, &nx, &ny, &depth)) continue;
        float px = P * .5f + nx * .5f * P, py = P * .5f - ny * .5f * P;

        float k = smooth01((v->ball_in - delay) / .45f);
        if (k < 1) {
            float spread = ((p->rnd / 255.f) - .5f) * P * 1.1f, dist = P * (.75f + (p->delay / 255.f) * .5f);
            float sx = P * .5f - ex * dist - ey * spread, sy = P * .5f - ey * dist + ex * spread;
            px = mixf(sx, px, k); py = mixf(sy, py, k);
        }
        if (v->ball_out > 0) {
            float s = smooth01(v->ball_out * 1.5f - delay);
            px = mixf(px, P * .5f - ex * P * .9f + (p->rnd - 128) * 1.2f, s);
            py = mixf(py, P * .5f - ey * P * .9f + (p->phase - 128) * 1.2f, s);
        }
        float nzr = -p->nx * c.sy + p->nz * c.cy, nxr = p->nx * c.cy + p->nz * c.sy;
        float rgb[3], a;
        shade(p, nzr, nxr, v, ph, rgb, &a);
        a *= flicker(v, ph) * (.55f + .45f * clampf(depth, 0, 1.4f)) * .7f;
        splat(px * to_internal, py * to_internal, rgb[0], rgb[1], rgb[2], a);
        drawn++;
    }
    s_last_count = drawn;
}

/* ---------- bloom + output ---------- */
static void build_glow(void)
{
    // 4x4 box downsample
    for (int gy = 0; gy < GLOW_RES; gy++)
        for (int gx = 0; gx < GLOW_RES; gx++) {
            uint32_t s[3] = { 0, 0, 0 };
            for (int y = 0; y < 4; y++) {
                const uint16_t *row = &s_acc[((gy * 4 + y) * DUST_RES + gx * 4) * 3];
                for (int x = 0; x < 4; x++) { s[0] += row[x * 3]; s[1] += row[x * 3 + 1]; s[2] += row[x * 3 + 2]; }
            }
            uint16_t *g = &s_glow[(gy * GLOW_RES + gx) * 3];
            g[0] = s[0] >> 4; g[1] = s[1] >> 4; g[2] = s[2] >> 4;
        }
    // two separable 5-tap blurs
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < GLOW_RES; y++)
            for (int x = 0; x < GLOW_RES; x++)
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t s = 0;
                    for (int k = -2; k <= 2; k++) { int xx = x + k; xx = xx < 0 ? 0 : xx >= GLOW_RES ? GLOW_RES - 1 : xx; s += s_glow[(y * GLOW_RES + xx) * 3 + ch] * (k == 0 ? 6 : (k == 1 || k == -1) ? 4 : 1); }
                    s_glow_tmp[(y * GLOW_RES + x) * 3 + ch] = s >> 4;
                }
        for (int y = 0; y < GLOW_RES; y++)
            for (int x = 0; x < GLOW_RES; x++)
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t s = 0;
                    for (int k = -2; k <= 2; k++) { int yy = y + k; yy = yy < 0 ? 0 : yy >= GLOW_RES ? GLOW_RES - 1 : yy; s += s_glow_tmp[(yy * GLOW_RES + x) * 3 + ch] * (k == 0 ? 6 : (k == 1 || k == -1) ? 4 : 1); }
                    s_glow[(y * GLOW_RES + x) * 3 + ch] = s >> 4;
                }
    }
}

static void write_output(uint16_t *fb, bool swap, float ember_cx, float ember_cy, float ember_r, float ember_a)
{
    const float glow_k = 1.6f;
    for (int iy = 0; iy < DUST_RES; iy++) {
        float gyf = (iy + .5f) / 4.f - .5f; int gy0 = (int)floorf(gyf); float fy = gyf - gy0;
        int gy1 = gy0 + 1; gy0 = gy0 < 0 ? 0 : gy0; gy1 = gy1 >= GLOW_RES ? GLOW_RES - 1 : gy1;
        for (int ix = 0; ix < DUST_RES; ix++) {
            float gxf = (ix + .5f) / 4.f - .5f; int gx0 = (int)floorf(gxf); float fx = gxf - gx0;
            int gx1 = gx0 + 1; gx0 = gx0 < 0 ? 0 : gx0; gx1 = gx1 >= GLOW_RES ? GLOW_RES - 1 : gx1;
            const uint16_t *a = &s_acc[(iy * DUST_RES + ix) * 3];
            float col[3];
            for (int ch = 0; ch < 3; ch++) {
                float g00 = s_glow[(gy0 * GLOW_RES + gx0) * 3 + ch], g10 = s_glow[(gy0 * GLOW_RES + gx1) * 3 + ch];
                float g01 = s_glow[(gy1 * GLOW_RES + gx0) * 3 + ch], g11 = s_glow[(gy1 * GLOW_RES + gx1) * 3 + ch];
                float g = mixf(mixf(g00, g10, fx), mixf(g01, g11, fx), fy);
                col[ch] = a[ch] + g * glow_k;
            }
            // ember halo behind the face / ball centre
            if (ember_a > 0) {
                float dx = (ix - ember_cx) / ember_r, dy = (iy - ember_cy) / ember_r, d2 = dx * dx + dy * dy;
                if (d2 < 1) { float f = (1 - d2) * (1 - d2) * ember_a * 255; col[0] += f; col[1] += f * .59f; col[2] += f * .24f; }
            }
            // background 03060b
            int r = (int)(col[0] + 3), g = (int)(col[1] + 6), b = (int)(col[2] + 11);
            r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
            uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            if (swap) c = (uint16_t)((c >> 8) | (c << 8));
            uint16_t *o = &fb[(iy * 2) * DUST_PANEL_PX + ix * 2];
            o[0] = c; o[1] = c; o[DUST_PANEL_PX] = c; o[DUST_PANEL_PX + 1] = c;
        }
    }
}

void dust_render(const dust_view_t *v, uint16_t *fb565, bool swap_bytes)
{
    memset(s_acc, 0, DUST_RES * DUST_RES * 3 * sizeof(uint16_t));
    float ember_a = .16f + .5f * v->talk + .1f * v->think + .04f * fsin(v->t * 1.15f);
    float ecx = -1, ecy = -1, er = 1;
    if (v->slot >= 0) {
        render_wall(v);
        // the glow follows the head through the same wall mapping (or the shape's centre)
        const wallgeo_t g = wall_geo(v->gap_px);
        float yaw = fsin(v->t * .21f) * .28f;
        cam_t c = { fcos(yaw), 1, fsin(yaw), 0, WALL_CAM_Y, WALL_CAM_D };
        float nx = 0, ny = 0, d = 0, u, w;
        cam_ndc(&c, 0, HEADY - .02f, .15f, &nx, &ny, &d);
        to_uv(v, nx, ny, &u, &w);
        if (v->warp) { float du, dw; dust_warp_sample(v->warp, u, w, &du, &dw); u += du; w += dw; }
        contain(v, &g, 0.f, &u, &w);
        u = mixf(u, v->frame_cx, v->form_amt); w = mixf(w, v->frame_cy, v->form_amt);
        const float to_px = DUST_RES / g.cell;
        ecx = (u - (v->slot % 3) * g.pitch) * to_px;
        ecy = (w - (v->slot / 3) * g.pitch) * to_px;
        er = 0.2f * fmaxf(.3f, .5f * (v->frame_sx + v->frame_sy)) * (1 + v->form_amt * .6f) * to_px;
        if (v->assemble < 1.2f) ember_a *= smooth01(v->assemble);
        ember_a *= 1 - v->scatter;
    } else {
        render_ball(v);
        ecx = DUST_RES / 2.f; ecy = DUST_RES / 2.f; er = DUST_RES * .32f;
        ember_a *= smooth01(v->ball_in) * (1 - v->ball_out);
    }
    build_glow();
    write_output(fb565, swap_bytes, ecx, ecy, er, ember_a);
}
