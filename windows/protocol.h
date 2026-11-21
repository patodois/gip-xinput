/* SPDX-License-Identifier: MIT OR Apache-2.0 */
#ifndef GX_PROTOCOL_H
#define GX_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Tests build the native API test against another port, so they can run beside a real host. */
#ifndef GX_PORT
#define GX_PORT 26785
#endif
#define GX_REQUEST_SIZE 24
#define GX_RESPONSE_SIZE 40
#define GX_DISCONNECTED 1167u
#define GX_BUTTON_MASK 0xf3ffu
/* How long the DLL keeps reporting the last good state when replies stop arriving in time.
 * Each call waits at most 2 ms; under Wine and Rosetta one late wake-up of the host is enough to
 * miss that, and games treat ERROR_DEVICE_NOT_CONNECTED as an unplugged pad (prompts change,
 * held input drops). 100 ms is about six frames at 60 Hz: it rides over scheduling hiccups many
 * times longer than the budget, yet a hung host still shows as disconnected within a tenth of a
 * second. An unplugged pad is reported by the host itself and a dead host is refused at once;
 * neither waits for this. */
#define GX_HOLD_MS 100

typedef struct {
    uint32_t sequence;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} gx_state;

static inline uint16_t gx_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t gx_u32(const uint8_t *p) {
    return (uint32_t)gx_u16(p) | (uint32_t)gx_u16(p + 2) << 16;
}
static inline uint64_t gx_u64(const uint8_t *p) {
    return (uint64_t)gx_u32(p) | (uint64_t)gx_u32(p + 4) << 32;
}
static inline void gx_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void gx_put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void gx_request(uint8_t p[GX_REQUEST_SIZE], uint8_t op, uint64_t id,
                              uint16_t left, uint16_t right) {
    memset(p, 0, GX_REQUEST_SIZE); memcpy(p, "GXIP", 4); p[4] = 1; p[5] = op;
    gx_put64(p + 8, id); gx_put16(p + 16, left); gx_put16(p + 18, right);
}
/* Return 1 only for a fully validated response to this particular request. */
static inline int gx_response(const uint8_t *p, size_t n, uint8_t op, uint64_t id,
                               uint32_t *status, gx_state *s) {
    if (n != GX_RESPONSE_SIZE || memcmp(p, "GXIP", 4) || p[4] != 1 ||
        p[5] != (uint8_t)(0x80 | op) || gx_u16(p + 6) || gx_u64(p + 8) != id ||
        gx_u32(p + 36)) return 0;
    uint32_t code = gx_u32(p + 16);
    if (code != 0 && code != GX_DISCONNECTED) return 0;
    if (gx_u16(p + 24) & ~GX_BUTTON_MASK) return 0;
    if (code) { for (unsigned i = 24; i < 36; ++i) if (p[i]) return 0; }
    *status = code;
    s->sequence = gx_u32(p + 20); s->buttons = gx_u16(p + 24);
    s->lt = p[26]; s->rt = p[27];
    /* Decode signed values arithmetically, without implementation-defined casts. */
    int32_t v[4];
    for (unsigned i = 0; i < 4; ++i) {
        v[i] = gx_u16(p + 28 + i * 2);
        if (v[i] >= 32768) v[i] -= 65536;
    }
    s->lx = (int16_t)v[0]; s->ly = (int16_t)v[1];
    s->rx = (int16_t)v[2]; s->ry = (int16_t)v[3];
    return 1;
}
#endif
