// Desktop test of neighbour sensing and automatic layout:  make topo_test && ./topo_test
// Signatures are rough values like tools/magnet_check.py predicts, plus 2 µT noise.
#include "dust_topo.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static float noise(void) { float u = (rand() + 1.f) / (RAND_MAX + 2.f), v = (rand() + 1.f) / (RAND_MAX + 2.f); return 2.f * sqrtf(-2 * logf(u)) * cosf(6.2831853f * v); }

int main(void)
{
    // per sensor, per side (and the plate behind): field added, µT. Rough values in the range
    // tools/magnet_check.py predicts for its default layout.
    topo_cal_t cal = { .alone = { { 75, 75, 0 }, { -60, 30, 9 } },
        .side = { { { -120, 1100, 40 }, { 30, -8, 2 }, { -10, 8, -1 }, { -1100, 120, 40 }, { 15, 20, -180 } },
                  { { 22, -36, 3 }, { 250, -150, 20 }, { 40, -140, -12 }, { -8, 13, 1 }, { -25, 10, -160 } } },
        .have_alone = true, .have_side = { true, true, true, true, true } };
    int wrong = 0, trials = 0;
    for (int rep = 0; rep < 300; rep++) for (int m = 0; m < 32; m++) {
        float r[2][3];
        for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) {
            r[s][a] = cal.alone[s][a] + noise();
            for (int k = 0; k < SIDE_COUNT; k++) if (m >> k & 1) r[s][a] += cal.side[s][k][a];
        }
        float res; if (topo_classify(&cal, r, &res) != m) wrong++;
        trials++;
    }
    printf("classify: %d wrong of %d noisy readings\n", wrong, trials);

    // layout: nine tiles in a shuffled order report their wall-frame masks; some mounted rotated
    int order[9] = { 4, 7, 0, 2, 8, 1, 6, 3, 5 }, slots[9]; uint8_t masks[9];
    for (int i = 0; i < 9; i++) { int rot = i % 4; uint8_t own = topo_rotate(topo_expected_mask(order[i], 0x1FF), (4 - rot) % 4); masks[i] = topo_rotate(own, rot); }
    bool ok = topo_layout_full(masks, slots); int bad = 0;
    for (int i = 0; i < 9; i++) if (slots[i] != order[i]) bad++;
    printf("layout: %s, %d misplaced\n", ok ? "complete 3x3 found" : "not found", bad);
    masks[3] = 0; printf("layout with one tile lifted: %s (expected not found)\n", topo_layout_full(masks, slots) ? "found" : "not found");
    return wrong || !ok || bad;
}
