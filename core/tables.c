// The renderer's lookup tables (§18's unpacking tables, and the word
// renderer's own), const and built by macros: no initialisation and no global
// state (PLAN.md section 3). Every word here is little-endian: byte 0 is the
// leftmost pixel.

#include "vdp_internal.h"

#define EACH16(F, h)                                                                                         \
    F((h) << 4 | 0), F((h) << 4 | 1), F((h) << 4 | 2), F((h) << 4 | 3), F((h) << 4 | 4), F((h) << 4 | 5),   \
    F((h) << 4 | 6), F((h) << 4 | 7), F((h) << 4 | 8), F((h) << 4 | 9), F((h) << 4 | 10), F((h) << 4 | 11), \
    F((h) << 4 | 12), F((h) << 4 | 13), F((h) << 4 | 14), F((h) << 4 | 15)
#define EACH256(F)                                                                                           \
    EACH16(F, 0), EACH16(F, 1), EACH16(F, 2), EACH16(F, 3), EACH16(F, 4), EACH16(F, 5), EACH16(F, 6),       \
    EACH16(F, 7), EACH16(F, 8), EACH16(F, 9), EACH16(F, 10), EACH16(F, 11), EACH16(F, 12), EACH16(F, 13),  \
    EACH16(F, 14), EACH16(F, 15)

// §18's 4bpp unpacking table: a pattern byte's two pixels, drawn in sub-palette
// g, as palette indices — the left pixel in the low byte. At 4bpp the sixteen
// groups cover the palette, so g × 16 + value is the whole of §8's mapping.
// 16 × 256 × 2 bytes; Phase 1 measured it saving 16% of an opaque 4bpp layer.
#define UNPACK4(g, b) (uint16_t)(((g) << 4 | (b) >> 4) | ((g) << 4 | ((b) & 15)) << 8)
#define UNPACK4_16(g, h)                                                                                      \
    UNPACK4(g, h << 4 | 0), UNPACK4(g, h << 4 | 1), UNPACK4(g, h << 4 | 2), UNPACK4(g, h << 4 | 3),           \
    UNPACK4(g, h << 4 | 4), UNPACK4(g, h << 4 | 5), UNPACK4(g, h << 4 | 6), UNPACK4(g, h << 4 | 7),           \
    UNPACK4(g, h << 4 | 8), UNPACK4(g, h << 4 | 9), UNPACK4(g, h << 4 | 10), UNPACK4(g, h << 4 | 11),         \
    UNPACK4(g, h << 4 | 12), UNPACK4(g, h << 4 | 13), UNPACK4(g, h << 4 | 14), UNPACK4(g, h << 4 | 15)
#define UNPACK4_GROUP(g)                                                                                      \
    {UNPACK4_16(g, 0), UNPACK4_16(g, 1), UNPACK4_16(g, 2), UNPACK4_16(g, 3), UNPACK4_16(g, 4),                \
     UNPACK4_16(g, 5), UNPACK4_16(g, 6), UNPACK4_16(g, 7), UNPACK4_16(g, 8), UNPACK4_16(g, 9),                \
     UNPACK4_16(g, 10), UNPACK4_16(g, 11), UNPACK4_16(g, 12), UNPACK4_16(g, 13), UNPACK4_16(g, 14),           \
     UNPACK4_16(g, 15)}
const uint16_t vdp_unpack4[16][256] = {
    UNPACK4_GROUP(0), UNPACK4_GROUP(1), UNPACK4_GROUP(2), UNPACK4_GROUP(3),
    UNPACK4_GROUP(4), UNPACK4_GROUP(5), UNPACK4_GROUP(6), UNPACK4_GROUP(7),
    UNPACK4_GROUP(8), UNPACK4_GROUP(9), UNPACK4_GROUP(10), UNPACK4_GROUP(11),
    UNPACK4_GROUP(12), UNPACK4_GROUP(13), UNPACK4_GROUP(14), UNPACK4_GROUP(15),
};

// A 2bpp pattern byte's four values, one to a byte, leftmost in the low byte.
#define VALUES2(b) ((uint32_t)((b) >> 6 & 3) | (uint32_t)((b) >> 4 & 3) << 8 | (uint32_t)((b) >> 2 & 3) << 16 | (uint32_t)((b) & 3) << 24)
const uint32_t vdp_values2[256] = {EACH256(VALUES2)};

// A byte's bits in the other order: a 1bpp pattern byte, MSB leftmost, as a
// mask with bit 0 leftmost.
#define REVERSE8(b)                                                                                           \
    (uint8_t)(((b) >> 7 & 1) | ((b) >> 5 & 2) | ((b) >> 3 & 4) | ((b) >> 1 & 8) | ((b) << 1 & 16) |           \
              ((b) << 3 & 32) | ((b) << 5 & 64) | ((b) << 7 & 128))
const uint8_t vdp_reverse8[256] = {EACH256(REVERSE8)};

// Every bit of a byte doubled: a mask of magnified pixels.
#define SPREAD8(b)                                                                                            \
    (uint16_t)(((b) & 1 ? 3u : 0) | ((b) & 2 ? 3u << 2 : 0) | ((b) & 4 ? 3u << 4 : 0) | ((b) & 8 ? 3u << 6 : 0) | \
               ((b) & 16 ? 3u << 8 : 0) | ((b) & 32 ? 3u << 10 : 0) | ((b) & 64 ? 3u << 12 : 0) |               \
               ((b) & 128 ? 3u << 14 : 0))
const uint16_t vdp_spread8[256] = {EACH256(SPREAD8)};

// Four bits to four bytes of $FF: MSB leftmost (a 1bpp pattern nibble), and
// bit 0 leftmost (a mask).
#define NIBBLE_MSB(n) ((n) & 8 ? 0xffu : 0) | ((n) & 4 ? 0xff00u : 0) | ((n) & 2 ? 0xff0000u : 0) | ((n) & 1 ? 0xff000000u : 0)
#define NIBBLE_LSB(n) ((n) & 1 ? 0xffu : 0) | ((n) & 2 ? 0xff00u : 0) | ((n) & 4 ? 0xff0000u : 0) | ((n) & 8 ? 0xff000000u : 0)
const uint32_t vdp_nibble_msb[16] = {
    NIBBLE_MSB(0), NIBBLE_MSB(1), NIBBLE_MSB(2), NIBBLE_MSB(3), NIBBLE_MSB(4), NIBBLE_MSB(5), NIBBLE_MSB(6), NIBBLE_MSB(7),
    NIBBLE_MSB(8), NIBBLE_MSB(9), NIBBLE_MSB(10), NIBBLE_MSB(11), NIBBLE_MSB(12), NIBBLE_MSB(13), NIBBLE_MSB(14), NIBBLE_MSB(15),
};
const uint32_t vdp_nibble_lsb[16] = {
    NIBBLE_LSB(0), NIBBLE_LSB(1), NIBBLE_LSB(2), NIBBLE_LSB(3), NIBBLE_LSB(4), NIBBLE_LSB(5), NIBBLE_LSB(6), NIBBLE_LSB(7),
    NIBBLE_LSB(8), NIBBLE_LSB(9), NIBBLE_LSB(10), NIBBLE_LSB(11), NIBBLE_LSB(12), NIBBLE_LSB(13), NIBBLE_LSB(14), NIBBLE_LSB(15),
};
