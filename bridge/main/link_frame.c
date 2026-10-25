#include "link_frame.h"
#include <string.h>

uint16_t lf_crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

int lf_encode(const lf_msg_t *m, uint8_t *out)
{
    uint8_t raw[LF_MAX_RAW];
    int n = 0, len = m->len > LF_MAX_DATA ? LF_MAX_DATA : m->len;
    raw[n++] = m->kind;
    memcpy(raw + n, m->mac, 6); n += 6;
    raw[n++] = (uint8_t)m->rssi;
    memcpy(raw + n, m->data, (size_t)len); n += len;
    uint16_t crc = lf_crc16(raw, n);
    raw[n++] = (uint8_t)(crc & 0xFF); raw[n++] = (uint8_t)(crc >> 8);
    // COBS: each run of non-zero bytes is prefixed by its length + 1; zeros vanish
    int o = 0, code_at = o++; uint8_t code = 1;
    for (int i = 0; i < n; i++) {
        if (raw[i]) { out[o++] = raw[i]; code++; }
        if (!raw[i] || code == 0xFF) { out[code_at] = code; code_at = o++; code = 1; }
    }
    out[code_at] = code;
    out[o++] = 0;
    return o;
}

static int cobs_decode(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int o = 0;
    for (int i = 0; i < n;) {
        uint8_t code = in[i++];
        if (!code) return -1;
        for (int k = 1; k < code; k++) { if (i >= n || o >= cap) return -1; out[o++] = in[i++]; }
        if (code != 0xFF && i < n) { if (o >= cap) return -1; out[o++] = 0; }
    }
    return o;
}

int lf_feed(lf_reader_t *r, uint8_t byte, lf_msg_t *m)
{
    if (byte) {
        if (r->n < (int)sizeof r->buf) r->buf[r->n++] = byte; else r->overflow = true;
        return 0;
    }
    int n = r->n; bool over = r->overflow;
    r->n = 0; r->overflow = false;
    if (!n) return 0;   // back-to-back zeros: nothing between them
    uint8_t raw[LF_MAX_RAW];
    int len = over ? -1 : cobs_decode(r->buf, n, raw, sizeof raw);
    if (len < 10) return -1;
    uint16_t crc = (uint16_t)(raw[len - 2] | (raw[len - 1] << 8));
    if (lf_crc16(raw, len - 2) != crc) return -1;
    m->kind = raw[0];
    memcpy(m->mac, raw + 1, 6);
    m->rssi = (int8_t)raw[7];
    m->len = (uint8_t)(len - 10);
    memcpy(m->data, raw + 8, m->len);
    return 1;
}
