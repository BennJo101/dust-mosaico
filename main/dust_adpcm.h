// IMA ADPCM, 4 bits per sample — portable C (tested on the desktop in host/net_test.c; the
// Jetson's copy is jetson/dust/proto.py and both must produce the same bytes).
//
// Speech at 16 kHz is 32 KB/s as raw samples and 8 KB/s like this, small enough to stream over
// ESP-NOW and to keep a few minutes of recorded lines in flash. Every audio packet carries the
// coder state it starts from, so a lost packet costs 20 ms of sound and nothing after it.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int16_t pred; uint8_t index; } adpcm_state_t;

// n samples in, n/2 bytes out (n even). Low nibble first.
void adpcm_encode(adpcm_state_t *s, const int16_t *pcm, int n, uint8_t *out);
// n/2 bytes in, n samples out.
void adpcm_decode(adpcm_state_t *s, const uint8_t *in, int n, int16_t *pcm);

#ifdef __cplusplus
}
#endif
