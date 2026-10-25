// Dust renderer — portable C (no ESP-IDF headers), so it also builds on a desktop for testing.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUST_PANEL_PX 480   // ESP-Mosaico panel: 480 x 480
#define DUST_RES      240   // internal render resolution, upscaled 2x on output
#define DUST_MAX_FX   6
#define DUST_TEXT_MAX 10

// One baked point (see tools/bake_dust.py). Coordinates are model units * 8192.
typedef struct {
    int16_t x, y, z;
    int8_t  nx, ny, nz;
    uint8_t kind;   // 0 head, 1 neck, 2 torso, 3 ember core
    uint8_t phase;  // 0..255 -> 0..2pi
    uint8_t fade;   // 0..255 brightness falloff (chest dissolve)
    uint8_t warm;   // 0..255 ember tint on the face
    uint8_t delay;  // 0..255 -> 0..0.55 s pour/transfer stagger
    uint8_t rnd;    // per-point random byte
    uint8_t group;  // 0..8: the screen whose ninth this point belongs to (spread evenly over the body)
} dust_point_t;

typedef enum { DUST_IDLE = 0, DUST_LISTENING, DUST_THINKING, DUST_SPEAKING } dust_state_t;

// Shapes the AI can pour Dust into.
typedef enum { DUST_FORM_BODY = 0, DUST_FORM_ORB, DUST_FORM_RING, DUST_FORM_WAVE, DUST_FORM_SPIRAL,
               DUST_FORM_CLOUD, DUST_FORM_TEXT, DUST_FORM_COUNT } dust_form_t;

// Ways the AI can push the dust around.
typedef enum { DUST_FX_NONE = 0, DUST_FX_FLOW, DUST_FX_SWIRL, DUST_FX_GATHER, DUST_FX_PULSE } dust_fx_type_t;

typedef struct {
    uint8_t type;             // dust_fx_type_t
    float amount;             // 0..1, already shaped by the attack/release envelope
    float radius;             // wall uv
    float phase;              // swirl: accumulated angle; pulse: age in seconds
    float x, y;               // centre, wall uv (0..1 across the whole wall, y down)
    float dx, dy;             // flow direction
    float release;            // 0 held .. 1+ every dot has drifted home
} dust_fx_t;

typedef struct {
    uint8_t id;               // dust_form_t
    char    text[DUST_TEXT_MAX + 1];
} dust_form_spec_t;

// Everything one screen needs to draw a frame. All screens get the same values except
// slot / home_slot, so their slices line up into one picture.
typedef struct {
    float t;                  // shared wall time in seconds
    float talk, listen, think;
    float assemble;           // 0..1.2 startup pour; >= 1.2 fully formed
    float shake;              // 0..1 destabilize
    float scatter;            // 0..1 blown apart
    float take[9];            // per screen 0..1: that screen's ninth has drained into it
    float pres[9];            // per screen 0..1: how present it is (the container's shape)
    float frame_cx, frame_cy; // where Dust stands, wall uv
    float frame_sx, frame_sy; // how far he is stretched (1 = the whole wall)
    const float *warp;        // bend field (dust_warp.h), NULL when every screen is there
    dust_form_spec_t form_a, form_b;  // blending from shape a to shape b
    float form_amt;           // 0 body .. 1 fully in shape
    float form_swap;          // 0..1.2 progress from form_a to form_b
    dust_fx_t fx[DUST_MAX_FX];
    int   slot;               // 0..8 = render this slice of the wall; -1 = render the ball
    float ball_in, ball_out;  // ball view progress
    int   home_slot;          // ball view: slot this panel came from (sets entry edge)
    float gap_px;             // gap between neighbouring screens' lit areas, in panel pixels
} dust_view_t;

bool dust_render_init(void *(*alloc_fn)(size_t));
void dust_render(const dust_view_t *v, uint16_t *fb565, bool swap_bytes);
int  dust_render_last_count(void);

// Parse "orb", "ring", ... (case-insensitive). Returns -1 if unknown.
int dust_form_from_name(const char *name);

#ifdef __cplusplus
}
#endif
