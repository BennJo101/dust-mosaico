// Neighbour sensing from the edge magnets — portable C (tested on the desktop in host/).
//
// Each tile has two magnetometers. A neighbour snapped onto one of its four edges brings two
// magnets close to that edge, which adds a fixed field to both sensor readings. After a quick
// calibration (once per tile: alone, then one neighbour on each side) a tile can tell which of
// its sides are joined from any reading, by finding the combination of sides that best explains
// the field it sees.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// bit n of a side mask = side n joined. BACK = mounted on the wall plate (its holding magnets).
enum { SIDE_TOP = 0, SIDE_RIGHT, SIDE_BOTTOM, SIDE_LEFT, SIDE_BACK, SIDE_COUNT };

typedef struct {
    float alone[2][3];        // each sensor's reading with no neighbours (µT)
    float side[2][SIDE_COUNT][3];   // the extra field each side's neighbour (or the plate) adds, per sensor
    bool  have_alone, have_side[SIDE_COUNT];
} topo_cal_t;

// Best side mask for a reading. residual (µT) says how well it fits; large means "something
// else magnetic is nearby", and the caller should hold its last answer.
uint8_t topo_classify(const topo_cal_t *cal, const float reading[2][3], float *residual);

// Turn a mask from the tile's own frame into the wall's frame. rot = quarter turns clockwise
// that the tile is mounted at (from the motion sensor). The BACK bit is left as it is.
uint8_t topo_rotate(uint8_t mask, int rot);

// Full 3x3: when nine tiles report their (wall-frame) masks, each one's position follows from
// which sides are joined: no top neighbour = top row, and so on. Returns the slot (0..8) for
// each entry, or -1 for all if the masks don't describe one complete 3x3.
bool topo_layout_full(const uint8_t masks[9], int slots[9]);

// Mask a tile at this slot should see on a complete 3x3 (for checks and simulation).
uint8_t topo_expected_mask(int slot, uint16_t present);

#ifdef __cplusplus
}
#endif
