// The bend field — portable C. Same solver as the web simulator's solveWarp().
//
// A 13x13 grid of displacements over the wall (uv). Nodes that sit on a missing screen are
// pinned to the nearest screen that's there; the rest relax smoothly between them, so Dust
// is squeezed and bent around gaps instead of disappearing behind them.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUST_WARP_G 13

// Field for a set of present screens (cached: solving is only done when the mask changes).
const float *dust_warp_field(uint16_t present_mask, float gap_px);

// Blend of two fields, k = 0 → from, 1 → to (k may overshoot slightly for the slosh).
void dust_warp_blend(uint16_t from_mask, uint16_t to_mask, float k, float gap_px, float out[DUST_WARP_G * DUST_WARP_G * 2]);

// Bilinear sample of a field at (u, w).
void dust_warp_sample(const float *field, float u, float w, float *du, float *dw);

#ifdef __cplusplus
}
#endif
