// Software decoders for the block-compressed D3D9 texture formats (DXT1-5, ATI1, ATI2).
//
// iPhone/iPad GPUs (Apple GPU families) cannot sample BC textures: creating an MTLTexture with a BC pixel
// format fails Metal's descriptor validation and aborts the process. The Metal backend therefore decodes
// these levels on the CPU at upload time and stores them as plain BGRA8 / R8 / RG8 textures.
//
// Header-only and free of Apple headers so tests/bc_decode_test.c can build it anywhere.
#ifndef FNV_BC_DECODE_H
#define FNV_BC_DECODE_H
#include <stdint.h>
#include <string.h>

typedef enum { BC_NONE = 0, BC_DXT1, BC_DXT3, BC_DXT5, BC_ATI1, BC_ATI2 } BCKind;

#define BCD_FCC(a, b, c_, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c_) << 16) | ((uint32_t)(d) << 24))

// D3DFORMAT fourcc -> decoder kind (DXT2/DXT4 are the premultiplied variants; the bits decode identically)
static inline BCKind bc_kind(uint32_t f) {
    if (f == BCD_FCC('D', 'X', 'T', '1')) return BC_DXT1;
    if (f == BCD_FCC('D', 'X', 'T', '2') || f == BCD_FCC('D', 'X', 'T', '3')) return BC_DXT3;
    if (f == BCD_FCC('D', 'X', 'T', '4') || f == BCD_FCC('D', 'X', 'T', '5')) return BC_DXT5;
    if (f == BCD_FCC('A', 'T', 'I', '1')) return BC_ATI1;
    if (f == BCD_FCC('A', 'T', 'I', '2')) return BC_ATI2;
    return BC_NONE;
}
// bytes per pixel of the decoded form: BGRA8 / R8 / RG8
static inline uint32_t bc_out_bpp(BCKind k) { return k == BC_ATI1 ? 1 : k == BC_ATI2 ? 2 : 4; }
static inline uint32_t bc_block_bytes(BCKind k) { return (k == BC_DXT1 || k == BC_ATI1) ? 8 : 16; }

static inline void bc_565(uint16_t v, uint8_t rgb[3]) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    rgb[0] = (uint8_t)((r << 3) | (r >> 2)); rgb[1] = (uint8_t)((g << 2) | (g >> 4)); rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

// The 8-byte colour block shared by DXT1-5. `four_color` forces the opaque 4-colour mode (DXT2-5 always
// use it; DXT1 picks the 3-colour + transparent mode when c0 <= c1). Writes 16 BGRA pixels' colour and,
// for DXT1, alpha; `px` is 4 rows of 4 pixels, B,G,R,A order.
static inline void bc_color_block(const uint8_t *s, int four_color, int write_alpha, uint8_t px[16][4]) {
    uint16_t c0 = (uint16_t)(s[0] | (s[1] << 8)), c1 = (uint16_t)(s[2] | (s[3] << 8));
    uint8_t pal[4][4], e0[3], e1[3];
    bc_565(c0, e0); bc_565(c1, e1);
    int opaque4 = four_color || c0 > c1;
    for (int i = 0; i < 3; i++) {
        pal[0][i] = e0[i]; pal[1][i] = e1[i];
        if (opaque4) { pal[2][i] = (uint8_t)((2 * e0[i] + e1[i]) / 3); pal[3][i] = (uint8_t)((e0[i] + 2 * e1[i]) / 3); }
        else         { pal[2][i] = (uint8_t)((e0[i] + e1[i]) / 2);     pal[3][i] = 0; }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255; pal[3][3] = opaque4 ? 255 : 0;
    uint32_t idx = (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 24);
    for (int p = 0; p < 16; p++) {
        const uint8_t *c = pal[(idx >> (2 * p)) & 3];
        px[p][0] = c[2]; px[p][1] = c[1]; px[p][2] = c[0];           // BGRA
        if (write_alpha) px[p][3] = c[3];
    }
}

// 8-byte interpolated-alpha block (DXT4/5 alpha, ATI1 red, each half of ATI2): 16 values, one per pixel
static inline void bc_alpha_block(const uint8_t *s, uint8_t out[16]) {
    uint8_t a[8]; a[0] = s[0]; a[1] = s[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int p = 0; p < 16; p++) out[p] = a[(bits >> (3 * p)) & 7];
}

// Decode one mip level. `src` is the compressed level (rows of `src_pitch` bytes, one row per 4 pixel rows);
// `dst` receives w*h pixels of bc_out_bpp(kind) bytes, `dst_pitch` bytes per row. Blocks that hang over the
// right/bottom edge (levels smaller than 4x4, or sizes that are not a multiple of 4) are clipped.
static inline void bc_decode_level(BCKind kind, const uint8_t *src, uint32_t src_pitch, uint32_t w, uint32_t h,
                                   uint8_t *dst, uint32_t dst_pitch) {
    uint32_t bpp = bc_out_bpp(kind), blk = bc_block_bytes(kind);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *s = src + (size_t)by * src_pitch + (size_t)bx * blk;
            uint8_t px[16][4], al[16], al2[16];
            switch (kind) {
            case BC_DXT1: bc_color_block(s, 0, 1, px); break;
            case BC_DXT3:
                bc_color_block(s + 8, 1, 0, px);
                for (int p = 0; p < 16; p++) { uint32_t n = (s[p >> 1] >> ((p & 1) * 4)) & 15; px[p][3] = (uint8_t)(n * 17); }
                break;
            case BC_DXT5:
                bc_color_block(s + 8, 1, 0, px); bc_alpha_block(s, al);
                for (int p = 0; p < 16; p++) px[p][3] = al[p];
                break;
            case BC_ATI1: bc_alpha_block(s, al); break;
            case BC_ATI2: bc_alpha_block(s, al); bc_alpha_block(s + 8, al2); break;
            default: return;
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++) {
                    uint8_t *d = dst + (size_t)(by * 4 + y) * dst_pitch + (size_t)(bx * 4 + x) * bpp;
                    uint32_t p = y * 4 + x;
                    if (kind == BC_ATI1) d[0] = al[p];
                    else if (kind == BC_ATI2) { d[0] = al[p]; d[1] = al2[p]; }
                    else memcpy(d, px[p], 4);
                }
        }
}
#endif
