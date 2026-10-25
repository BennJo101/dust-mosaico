#include "dust_warp.h"
#include "dust_render.h"
#include "dust_layout.h"
#include <math.h>
#include <string.h>

#define G DUST_WARP_G
#define NODES (G * G)

typedef struct { uint16_t mask; float gap; float d[NODES * 2]; int used; } warp_cache_t;
static warp_cache_t s_cache[3];
static int s_tick;

static int nearest_present(const bool present[9], float P, float C, float x, float y, float *qx, float *qy)
{
    if (x < 0 || y < 0 || x > 1 || y > 1) return 0;
    int c = (int)(x / P), r = (int)(y / P);
    c = c > 2 ? 2 : c; r = r > 2 ? 2 : r;
    if (present[r * 3 + c]) return 0;
    float m = C * 0.06f, best = 1e9f;
    for (int j = 0; j < 9; j++) {
        if (!present[j]) continue;
        float lx = (j % 3) * P + m, ly = (j / 3) * P + m, hx = lx + C - 2 * m, hy = ly + C - 2 * m;
        float ax = x < lx ? lx : x > hx ? hx : x, ay = y < ly ? ly : y > hy ? hy : y;
        float d = (ax - x) * (ax - x) + (ay - y) * (ay - y);
        if (d < best) { best = d; *qx = ax; *qy = ay; }
    }
    return best < 1e8f;
}

static void solve(uint16_t mask, float gap_px, float *D)
{
    memset(D, 0, sizeof(float) * NODES * 2);
    bool present[9]; int n = 0;
    for (int i = 0; i < 9; i++) { present[i] = mask >> i & 1; n += present[i]; }
    if (n == 0 || n == 9) return;
    const float wall = 3 * DUST_PANEL_PX + 2 * gap_px, C = DUST_PANEL_PX / wall, P = (DUST_PANEL_PX + gap_px) / wall;
    static float T[NODES * 2];
    // only gaps that Dust actually reaches need bending: skip nodes outside where he stands
    const dust_frame_t fr = dust_fit(mask, gap_px);
    const float fm = 0.03f;
    // anchors: nodes well inside a present screen stay put, so the bend only happens near gaps
    // and never shoves Dust as a whole into a corner
    static uint8_t anchor[NODES];
    const float band = C * 0.6f;
    for (int j = 0; j < G; j++) for (int i = 0; i < G; i++) {
        float x = (float)i / (G - 1), y = (float)j / (G - 1), dmin = 1e9f;
        for (int q = 0; q < 9; q++) {
            if (present[q]) continue;
            float lx = (q % 3) * P, ly = (q / 3) * P, hx = lx + C, hy = ly + C;
            float dx = fmaxf(fmaxf(lx - x, 0), x - hx), dy = fmaxf(fmaxf(ly - y, 0), y - hy), d = sqrtf(dx * dx + dy * dy);
            if (d < dmin) dmin = d;
        }
        anchor[j * G + i] = dmin > band;
    }
    for (int it = 0; it <= 220; it++) {
        for (int j = 0; j < G; j++) for (int i = 0; i < G; i++) {     // pin
            int k = (j * G + i) * 2;
            if (anchor[j * G + i]) { D[k] = D[k + 1] = 0; continue; }
            float x = (float)i / (G - 1) + D[k], y = (float)j / (G - 1) + D[k + 1], qx = x, qy = y;
            // only nodes near a gap's edge are pinned: deep inside a gap there is no dust worth
            // moving, and pinning there would shove the whole figure
            if (nearest_present(present, P, C, x, y, &qx, &qy) && (qx - x) * (qx - x) + (qy - y) * (qy - y) < C * C * 0.25f
                && fabsf((float)i / (G - 1) - fr.cx) < fr.sx / 2 + fm && fabsf((float)j / (G - 1) - fr.cy) < fr.sy / 2 + fm) { D[k] += qx - x; D[k + 1] += qy - y; }
        }
        if (it == 220) break;
        for (int j = 0; j < G; j++) for (int i = 0; i < G; i++) {     // relax
            int k = (j * G + i) * 2; float sx = 0, sy = 0; int cnt = 0;
            static const int DI[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
            for (int q = 0; q < 4; q++) {
                int ii = i + DI[q][0], jj = j + DI[q][1];
                if (ii < 0 || jj < 0 || ii >= G || jj >= G) continue;
                sx += D[(jj * G + ii) * 2]; sy += D[(jj * G + ii) * 2 + 1]; cnt++;
            }
            T[k] = D[k] * 0.4f + sx / cnt * 0.6f; T[k + 1] = D[k + 1] * 0.4f + sy / cnt * 0.6f;
        }
        memcpy(D, T, sizeof(float) * NODES * 2);
    }
}

const float *dust_warp_field(uint16_t mask, float gap_px)
{
    mask &= 0x1FF;
    s_tick++;
    for (int c = 0; c < 3; c++) if (s_cache[c].used && s_cache[c].mask == mask && s_cache[c].gap == gap_px) { s_cache[c].used = s_tick; return s_cache[c].d; }
    int lru = 0;
    for (int c = 1; c < 3; c++) if (s_cache[c].used < s_cache[lru].used) lru = c;
    s_cache[lru].mask = mask; s_cache[lru].gap = gap_px; s_cache[lru].used = s_tick;
    solve(mask, gap_px, s_cache[lru].d);
    return s_cache[lru].d;
}

void dust_warp_blend(uint16_t from, uint16_t to, float k, float gap_px, float out[DUST_WARP_G * DUST_WARP_G * 2])
{
    const float *a = dust_warp_field(from, gap_px);
    const float *b = dust_warp_field(to, gap_px);
    for (int i = 0; i < NODES * 2; i++) out[i] = a[i] + (b[i] - a[i]) * k;
}

void dust_warp_sample(const float *f, float u, float w, float *du, float *dw)
{
    if (!f) { *du = *dw = 0; return; }
    float gx = (u < 0 ? 0 : u > 1 ? 1 : u) * (G - 1), gy = (w < 0 ? 0 : w > 1 ? 1 : w) * (G - 1);
    int i0 = (int)gx, j0 = (int)gy;
    if (i0 > G - 2) i0 = G - 2;
    if (j0 > G - 2) j0 = G - 2;
    float fx = gx - i0, fy = gy - j0;
    const float *a = &f[(j0 * G + i0) * 2], *b = a + 2, *c = a + G * 2, *d = c + 2;
    *du = (a[0] + (b[0] - a[0]) * fx) * (1 - fy) + (c[0] + (d[0] - c[0]) * fx) * fy;
    *dw = (a[1] + (b[1] - a[1]) * fx) * (1 - fy) + (c[1] + (d[1] - c[1]) * fx) * fy;
}
