/* SPDX-License-Identifier: MIT OR Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "protocol.h"

int main(void) {
    uint8_t q[GX_REQUEST_SIZE];
    gx_request(q, 2, UINT64_C(0x0807060504030201), 0x1234, 0xabcd);
    const uint8_t golden[] = {'G','X','I','P',1,2,0,0,1,2,3,4,5,6,7,8,0x34,0x12,0xcd,0xab,0,0,0,0};
    assert(!memcmp(q, golden, sizeof(q)));
    uint8_t p[GX_RESPONSE_SIZE + 1] = {'G','X','I','P',1,0x81,0,0,1,0,0,0,0,0,0,0,
        0,0,0,0,42,0,0,0,1,18,128,255,255,127,0,128,1,0,255,255,0,0,0,0,0};
    gx_state s; uint32_t status;
    assert(gx_response(p, 40, 1, 1, &status, &s));
    assert(!status && s.sequence == 42 && s.buttons == 0x1201 && s.lt == 128 && s.rt == 255);
    assert(s.lx == 32767 && s.ly == -32768 && s.rx == 1 && s.ry == -1);
    for (size_t n = 0; n < 40; ++n) assert(!gx_response(p, n, 1, 1, &status, &s));
    assert(!gx_response(p, 41, 1, 1, &status, &s));
    assert(!gx_response(p, 40, 2, 1, &status, &s));
    assert(!gx_response(p, 40, 1, 2, &status, &s));
    const unsigned fields[] = {0,4,5,6,7,16,36,37,38,39};
    for (size_t i = 0; i < sizeof(fields)/sizeof(*fields); ++i) {
        unsigned pos = fields[i]; uint8_t old = p[pos]; p[pos] = 255;
        assert(!gx_response(p, 40, 1, 1, &status, &s)); p[pos] = old;
    }
    p[25] |= 4; assert(!gx_response(p, 40, 1, 1, &status, &s)); p[25] &= (uint8_t)~4;
    gx_put16(p + 16, 1167); assert(!gx_response(p, 40, 1, 1, &status, &s));
    memset(p + 24, 0, 12); assert(gx_response(p, 40, 1, 1, &status, &s) && status == 1167);
    puts("C protocol tests passed"); return 0;
}
