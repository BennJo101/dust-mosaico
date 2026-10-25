#include "dust_layout.h"
#include "dust_render.h"
#include <math.h>

dust_frame_t dust_fit(uint16_t present, float gap_px)
{
    dust_frame_t f = { 0.5f, 0.5f, 1, 1, false };
    const float wall = 3 * DUST_PANEL_PX + 2 * gap_px, C = DUST_PANEL_PX / wall, P = (DUST_PANEL_PX + gap_px) / wall;
    int n = 0, c0 = 3, c1 = -1, r0 = 3, r1 = -1;
    float mx = 0, my = 0;
    for (int i = 0; i < 9; i++) {
        if (!(present >> i & 1)) continue;
        int c = i % 3, r = i / 3;
        n++; mx += c * P + C / 2; my += r * P + C / 2;
        if (c < c0) c0 = c;
        if (c > c1) c1 = c;
        if (r < r0) r0 = r;
        if (r > r1) r1 = r;
    }
    if (!n) return f;
    mx /= n; my /= n;
    float x0 = c0 * P, x1 = c1 * P + C, y0 = r0 * P, y1 = r1 * P + C, bw = x1 - x0, bh = y1 - y0;
    float s = sqrtf(n / 9.f);
    float sx = fminf(s * sqrtf(bw / bh), bw), sy = fminf(s * sqrtf(bh / bw), bh);
    f.sx = sx; f.sy = sy;
    f.cx = fminf(x1 - sx / 2, fmaxf(x0 + sx / 2, mx));
    f.cy = fminf(y1 - sy / 2, fmaxf(y0 + sy / 2, my));
    if (n < 9) { f.sx *= 0.94f; f.sy *= 0.94f; }   // a little breathing room when he isn't filling the whole wall
    // a full rectangle of screens (2x2, 2x3, a column...): stand dead centre on it
    if (n == (c1 - c0 + 1) * (r1 - r0 + 1)) { f.cx = (x0 + x1) / 2; f.cy = (y0 + y1) / 2; }
    f.valid = true;
    return f;
}
