// Frames between the bridge and the Jetson — portable C (tested on the desktop in
// host/net_test.c; the Jetson's copy is jetson/dust/link.py).
//
// Each frame is: kind, a MAC address, a signal strength, the payload, and a CRC-16, all
// COBS-encoded so that 0x00 only ever appears as the end of a frame. A reader that starts in the
// middle of a frame, or gets a garbled one, just waits for the next 0x00.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LF_RX = 1,       // bridge → Jetson: a packet heard over ESP-NOW (mac = sender, rssi)
    LF_TX = 2,       // Jetson → bridge: send this (mac = destination, ff:ff:ff:ff:ff:ff = everyone)
    LF_STATUS = 3,   // bridge → Jetson, once a second: data = lf_status_t
    LF_CHANNEL = 4,  // Jetson → bridge: switch to channel data[0]
};

#define LF_MAX_DATA 250
#define LF_MAX_RAW  (1 + 6 + 1 + LF_MAX_DATA + 2)
#define LF_MAX_ENC  (LF_MAX_RAW + LF_MAX_RAW / 254 + 2)   // COBS overhead and the closing 0x00

typedef struct { uint8_t kind; uint8_t mac[6]; int8_t rssi; uint8_t len; uint8_t data[LF_MAX_DATA]; } lf_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t  version, channel;
    uint8_t  mac[6];            // the bridge's own
    uint32_t heard, sent, send_failed, dropped;   // dropped: packets the link to the Jetson couldn't keep up with
} lf_status_t;

uint16_t lf_crc16(const uint8_t *p, int n);   // CRC-16/CCITT-FALSE
// Encode a message; returns the number of bytes written (including the closing 0x00).
int lf_encode(const lf_msg_t *m, uint8_t *out);

typedef struct { uint8_t buf[LF_MAX_ENC]; int n; bool overflow; } lf_reader_t;
// Feed one byte from the link. Returns 1 when *m holds a whole, good frame; -1 when a frame
// ended but was damaged; 0 otherwise.
int lf_feed(lf_reader_t *r, uint8_t byte, lf_msg_t *m);

#ifdef __cplusplus
}
#endif
