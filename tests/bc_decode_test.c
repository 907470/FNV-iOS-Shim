// Known-answer tests for the software BC decoders (upstream/recomp/host/bc_decode.h).
#include <stdio.h>
#include <stdlib.h>
#include "bc_decode.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void px_eq(const char *what, const uint8_t *got, int b, int g, int r, int a) {
    CHECK(got[0] == b && got[1] == g && got[2] == r && got[3] == a, "%s: got BGRA %d,%d,%d,%d want %d,%d,%d,%d", what, got[0], got[1], got[2], got[3], b, g, r, a);
}

int main(void) {
    uint8_t out[16 * 16 * 4];

    // DXT1, 4-colour mode: c0 = pure red (F800), c1 = pure blue (001F); pixels 0..3 pick palette 0..3
    {
        uint8_t blk[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4 /* 11 10 01 00 */, 0, 0, 0};
        bc_decode_level(BC_DXT1, blk, 8, 4, 4, out, 16);
        px_eq("dxt1 c0", out + 0, 0, 0, 255, 255);
        px_eq("dxt1 c1", out + 4, 255, 0, 0, 255);
        px_eq("dxt1 2/3c0+1/3c1", out + 8, 85, 0, 170, 255);
        px_eq("dxt1 1/3c0+2/3c1", out + 12, 170, 0, 85, 255);
        px_eq("dxt1 row1 -> index 0", out + 16, 0, 0, 255, 255);
    }
    // DXT1, 3-colour mode (c0 <= c1): index 2 = midpoint, index 3 = transparent black
    {
        uint8_t blk[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};   // c0 = blue, c1 = red
        bc_decode_level(BC_DXT1, blk, 8, 4, 4, out, 16);
        px_eq("dxt1 3c mid", out + 8, 127, 0, 127, 255);
        px_eq("dxt1 3c clear", out + 12, 0, 0, 0, 0);
    }
    // DXT3: explicit 4-bit alpha, colour block always 4-colour even when c0 <= c1
    {
        uint8_t blk[16] = {0x10, 0x32, 0, 0, 0, 0, 0, 0,  0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};   // alpha 0,1,2,3; c0 blue, c1 red
        bc_decode_level(BC_DXT3, blk, 16, 4, 4, out, 16);
        px_eq("dxt3 p0", out + 0, 255, 0, 0, 0);
        px_eq("dxt3 p1", out + 4, 0, 0, 255, 17);
        px_eq("dxt3 p2 (2/3 blue+1/3 red)", out + 8, 170, 0, 85, 34);
        px_eq("dxt3 p3", out + 12, 85, 0, 170, 51);
    }
    // DXT5: 8-alpha mode (a0 > a1) and 6-alpha mode (a0 <= a1 with 0 and 255 endpoints)
    {
        uint8_t al[16];
        uint8_t b8[8] = {255, 0, 0, 0, 0, 0, 0, 0};
        bc_alpha_block(b8, al);
        CHECK(al[0] == 255, "bc3 a0 = %d", al[0]);
        uint8_t bi[8] = {255, 0, 0xFA, 0x08, 0, 0, 0, 0};   // 3-bit indices: 2,3,4,5,6,7 ... (0xFA,0x08 -> bits 010 111 ...)
        uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)bi[2 + i] << (8 * i);
        bc_alpha_block(bi, al);
        for (int p = 0; p < 16; p++) {
            int idx = (int)((bits >> (3 * p)) & 7);
            int want = idx == 0 ? 255 : idx == 1 ? 0 : ((8 - idx) * 255) / 7;    // idx 2..7 = (6..1)*a0/7
            CHECK(al[p] == want, "bc3 8-alpha px %d idx %d got %d want %d", p, idx, al[p], want);
        }
        // a0 <= a1: six interpolants plus literal 0 and 255. Indices 2 and 5 are (4*a0+a1)/5 and (a0+4*a1)/5.
        uint8_t c6[8] = {0, 255, 0, 0, 0, 0, 0, 0};
        c6[2] = (uint8_t)(2 | (5 << 3) | (6 << 6)); c6[3] = (uint8_t)((6 >> 2) | (7 << 1));   // pixels 0..3 = idx 2,5,6,7
        bc_alpha_block(c6, al);
        CHECK(al[0] == 51 && al[1] == 204 && al[2] == 0 && al[3] == 255, "bc3 6-alpha got %d,%d,%d,%d want 51,204,0,255", al[0], al[1], al[2], al[3]);
    }
    // DXT5 end to end
    {
        uint8_t blk[16] = {200, 100, 0, 0, 0, 0, 0, 0,  0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};   // alpha all idx0 = 200, colour all red
        bc_decode_level(BC_DXT5, blk, 16, 4, 4, out, 16);
        for (int p = 0; p < 16; p++) px_eq("dxt5 flat", out + (p / 4) * 16 + (p % 4) * 4, 0, 0, 255, 200);
    }
    // ATI1: single channel; ATI2: first block -> R, second -> G
    {
        uint8_t b1[8] = {77, 0, 0, 0, 0, 0, 0, 0};
        bc_decode_level(BC_ATI1, b1, 8, 4, 4, out, 4);
        for (int p = 0; p < 16; p++) CHECK(out[p] == 77, "ati1 px %d = %d", p, out[p]);
        uint8_t b2[16] = {10, 0, 0, 0, 0, 0, 0, 0,  20, 0, 0, 0, 0, 0, 0, 0};
        bc_decode_level(BC_ATI2, b2, 16, 4, 4, out, 8);
        for (int p = 0; p < 16; p++) CHECK(out[2 * p] == 10 && out[2 * p + 1] == 20, "ati2 px %d = %d,%d", p, out[2 * p], out[2 * p + 1]);
    }
    // clipping: 1x1, 2x2 and 5x3 levels must not write outside w*h (guard bytes stay 0xAA)
    {
        uint8_t blk[2][8] = {{0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}, {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}};
        struct { uint32_t w, h; } sz[] = {{1, 1}, {2, 2}, {5, 3}};
        for (int i = 0; i < 3; i++) {
            memset(out, 0xAA, sizeof out);
            uint32_t pitch = sz[i].w * 4, bw = (sz[i].w + 3) / 4;
            uint8_t src[16]; for (uint32_t b = 0; b < bw; b++) memcpy(src + 8 * b, blk[0], 8);
            bc_decode_level(BC_DXT1, src, bw * 8, sz[i].w, sz[i].h, out, pitch);
            for (uint32_t y = 0; y < sz[i].h; y++) for (uint32_t x = 0; x < sz[i].w; x++) px_eq("clip px", out + y * pitch + x * 4, 0, 0, 255, 255);
            for (uint32_t k = pitch * sz[i].h; k < pitch * sz[i].h + 16; k++) CHECK(out[k] == 0xAA, "clip %ux%u wrote past the end at %u", sz[i].w, sz[i].h, k);
        }
    }
    CHECK(bc_kind(BCD_FCC('D', 'X', 'T', '2')) == BC_DXT3 && bc_kind(BCD_FCC('D', 'X', 'T', '4')) == BC_DXT5 && bc_kind(21) == BC_NONE, "bc_kind mapping");
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("bc_decode: ok");
    return 0;
}
