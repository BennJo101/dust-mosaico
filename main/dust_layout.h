// Where Dust stands when screens are missing — portable C. Same rule as the web simulator.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cx, cy;   // centre, wall uv (0..1 across the whole wall, y down)
    float sx, sy;   // stretch per axis (1 = the whole wall)
    bool  valid;    // false when no screen is on the wall
} dust_frame_t;

// Fluid fit: Dust keeps his volume (minus the ninths that left: area = present/9), stretches
// to the bounding box of the screens that are there and stands centred on them (on an uneven
// shape like an L he leans toward the side with more screens). Anything that crosses a gap is
// bent out of it by dust_warp.c.
dust_frame_t dust_fit(uint16_t present_mask, float gap_px);

#ifdef __cplusplus
}
#endif
