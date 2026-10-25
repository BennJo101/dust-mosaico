#include "dust_topo.h"
#include <math.h>
#include <string.h>

uint8_t topo_classify(const topo_cal_t *cal, const float r[2][3], float *residual)
{
    // try every combination of joined sides; keep the one whose predicted field fits best
    float best = 1e30f; uint8_t best_mask = 0;
    for (int m = 0; m < (1 << SIDE_COUNT); m++) {
        bool usable = true;
        for (int k = 0; k < SIDE_COUNT; k++) if ((m >> k & 1) && !cal->have_side[k]) usable = false;
        if (!usable) continue;
        float err = 0;
        for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) {
            float pred = cal->alone[s][a];
            for (int k = 0; k < SIDE_COUNT; k++) if (m >> k & 1) pred += cal->side[s][k][a];
            float d = r[s][a] - pred; err += d * d;
        }
        if (err < best) { best = err; best_mask = (uint8_t)m; }
    }
    if (residual) *residual = sqrtf(best);
    return best_mask;
}

uint8_t topo_rotate(uint8_t mask, int rot)
{
    rot = ((rot % 4) + 4) % 4;
    uint8_t e = mask & 0xF;
    return (uint8_t)((((e << rot) | (e >> (4 - rot))) & 0xF) | (mask & (1 << SIDE_BACK)));
}

uint8_t topo_expected_mask(int slot, uint16_t present)
{
    int r = slot / 3, c = slot % 3; uint8_t m = 0;
    if (r > 0 && (present >> (slot - 3) & 1)) m |= 1 << SIDE_TOP;
    if (c < 2 && (present >> (slot + 1) & 1)) m |= 1 << SIDE_RIGHT;
    if (r < 2 && (present >> (slot + 3) & 1)) m |= 1 << SIDE_BOTTOM;
    if (c > 0 && (present >> (slot - 1) & 1)) m |= 1 << SIDE_LEFT;
    return m;
}

bool topo_layout_full(const uint8_t masks[9], int slots[9])
{
    bool used[9] = { false };
    for (int i = 0; i < 9; i++) {
        uint8_t m = masks[i] & 0xF;
        int row = !(m >> SIDE_TOP & 1) ? 0 : !(m >> SIDE_BOTTOM & 1) ? 2 : 1;
        int col = !(m >> SIDE_LEFT & 1) ? 0 : !(m >> SIDE_RIGHT & 1) ? 2 : 1;
        int slot = row * 3 + col;
        if (used[slot] || topo_expected_mask(slot, 0x1FF) != m) { for (int j = 0; j < 9; j++) slots[j] = -1; return false; }
        used[slot] = true; slots[i] = slot;
    }
    return true;
}
