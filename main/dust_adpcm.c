#include "dust_adpcm.h"

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767 };
static const int8_t INDEX_ADJ[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// Apply one 4-bit code to the state; returns the new sample.
static int16_t step_state(adpcm_state_t *s, uint8_t code)
{
    int step = STEP[s->index], diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int p = s->pred + ((code & 8) ? -diff : diff);
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    s->pred = (int16_t)p;
    int idx = s->index + INDEX_ADJ[code & 7];
    s->index = (uint8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return s->pred;
}

static uint8_t encode_one(adpcm_state_t *s, int16_t x)
{
    int step = STEP[s->index], diff = x - s->pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1; if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1; if (diff >= step) code |= 1;
    step_state(s, code);   // track exactly what the decoder will reconstruct
    return code;
}

void adpcm_encode(adpcm_state_t *s, const int16_t *pcm, int n, uint8_t *out)
{
    for (int i = 0; i + 1 < n; i += 2) {
        uint8_t lo = encode_one(s, pcm[i]), hi = encode_one(s, pcm[i + 1]);
        out[i / 2] = (uint8_t)(lo | (hi << 4));
    }
}

void adpcm_decode(adpcm_state_t *s, const uint8_t *in, int n, int16_t *pcm)
{
    for (int i = 0; i + 1 < n; i += 2) {
        pcm[i] = step_state(s, in[i / 2] & 0xF);
        pcm[i + 1] = step_state(s, in[i / 2] >> 4);
    }
}
