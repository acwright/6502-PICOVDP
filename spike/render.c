// Phase 1 timing spike: the renderer. See render.h.
//
// Second pass. The first drew a pixel at a time and ran at 20 to 50 cycles a
// pixel (docs/results/phase-01.md). This one works a cell at a time in 32-bit
// words: a cell's eight pixels are two words of indices and two of solid
// masks, merged by masks against two words of priority classes. Sprites claim
// pixels in a 320-bit bitmap, so collision is a word AND and only newly
// claimed pixels are drawn, four pixels to a word.

#include "render.h"

#include <string.h>

#define ONES 0x01010101u
#define ALWAYS_INLINE inline __attribute__((always_inline))

static uint16_t tab4[16][256];      // §18's table: a 4bpp byte in sub-palette g → two indices, left in the low byte
static uint16_t tab4_solid[256];    // $FF over each non-zero pixel of a 4bpp byte
static uint32_t nib_bytes[16];      // four 1bpp pattern bits → $FF per set bit, leftmost in the low byte
static uint32_t nib_lsb_bytes[16];  // four bits → $FF per set bit, bit 0 in the low byte
static uint32_t vals2[256];         // a 2bpp byte → its four values, leftmost in the low byte
static uint8_t rev2[256];           // a 2bpp byte with its pixels reversed
static uint8_t rev8[256];           // bits reversed
static uint8_t nzbits2[256];        // a 2bpp byte → a solid bit per pixel, bit 0 leftmost
static uint8_t nzbits4[256];        // a 4bpp byte → a solid bit per pixel, bit 0 leftmost
static uint16_t spread8[256];       // every bit doubled, for magnified sprites

void spike_init_tables(void) {
    for (unsigned b = 0; b < 256; b++) {
        const unsigned hi = b >> 4, lo = b & 15;
        for (unsigned g = 0; g < 16; g++)
            tab4[g][b] = (uint16_t)((g << 4 | hi) | (g << 4 | lo) << 8);
        tab4_solid[b] = (uint16_t)((hi ? 0x00FF : 0) | (lo ? 0xFF00 : 0));
        uint32_t v2 = 0;
        unsigned r8 = 0, nz2 = 0, sp = 0;
        for (unsigned k = 0; k < 4; k++) {
            const unsigned val = (b >> (6 - 2 * k)) & 3;
            v2 |= (uint32_t)val << (8 * k);
            if (val) nz2 |= 1u << k;
        }
        for (unsigned k = 0; k < 8; k++) {
            if (b & (1u << k)) {
                r8 |= 0x80u >> k;
                sp |= 3u << (2 * k);
            }
        }
        vals2[b] = v2;
        rev2[b] = (uint8_t)((b >> 6 & 3) | (b >> 2 & 0x0C) | (b << 2 & 0x30) | (b << 6 & 0xC0));
        rev8[b] = (uint8_t)r8;
        nzbits2[b] = (uint8_t)nz2;
        nzbits4[b] = (uint8_t)((hi ? 1 : 0) | (lo ? 2 : 0));
        spread8[b] = (uint16_t)sp;
    }
    for (unsigned n = 0; n < 16; n++) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4; k++)
            if (n & (8u >> k)) w |= 0xFFu << (8 * k);
        nib_bytes[n] = w;
        uint32_t l = 0;
        for (unsigned k = 0; k < 4; k++)
            if (n & (1u << k)) l |= 0xFFu << (8 * k);
        nib_lsb_bytes[n] = l;
    }
}

void spike_set_geometry(spike_t *v, int geom) {
    if (geom == SPIKE_FULL) {
        v->width = 320; v->cols = 40; v->left = 0;
    } else {
        v->width = 256; v->cols = 32; v->left = 32;
    }
}

void spike_vram_sealed(spike_t *v) {
    memcpy(v->vram + 0x10000, v->vram, SPIKE_VRAM_GUARD);
}

static ALWAYS_INLINE uint32_t load32(const uint8_t *p) { uint32_t w; memcpy(&w, p, 4); return w; }
static ALWAYS_INLINE void store32(uint8_t *p, uint32_t w) { memcpy(p, &w, 4); }
static ALWAYS_INLINE uint32_t bswap32(uint32_t w) {
    return w << 24 | (w << 8 & 0xFF0000u) | (w >> 8 & 0xFF00u) | w >> 24;
}
// $01 in each byte that is non-zero.
static ALWAYS_INLINE uint32_t nonzero_bytes(uint32_t w) {
    return ((((w & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | w) >> 7) & ONES;
}

// ---------------------------------------------------------------------------
// Layers, §8 and §13
//
// Whole cells are drawn from `pic - ox`, where ox is the scroll's offset into
// the first cell, so `pic` lines up with picture column 0.

enum { DRAW, MERGE_OPAQUE, MERGE };     // DRAW: an opaque layer 0 straight onto the line

static ALWAYS_INLINE void cells(const spike_t *v, const spike_layer_t *L, int line, uint8_t *pic_i,
                                uint8_t *pic_p, const unsigned depth, const bool tab, const int how,
                                const uint8_t cls_norm, const uint8_t cls_pri) {
    const uint8_t *const vr = v->vram;
    const unsigned W = v->width, cols = v->cols;
    const unsigned mapy = ((unsigned)line + L->scry % SPIKE_HEIGHT) % SPIKE_HEIGHT;   // §13
    const unsigned mapx = L->scrx % W;
    const unsigned py = mapy & 7;
    const uint32_t nrow = L->name + (mapy >> 3) * cols;
    const uint32_t arow = L->attr + (mapy >> 3) * cols;
    const uint32_t pat = L->pat;
    const unsigned ox = mapx & 7;
    const unsigned ncells = (W + ox + 7) >> 3;
    const uint8_t pal = L->pal;
    const bool solid_all = how != MERGE;
    unsigned col = mapx >> 3;
    uint8_t *di = pic_i - ox, *dp = pic_p - ox;

    for (unsigned c = 0; c < ncells; c++) {
        const uint8_t name = vr[(nrow + col) & 0xFFFF], a = vr[(arow + col) & 0xFFFF];
        if (++col == cols) col = 0;
        uint32_t s0, s1, m0 = ~0u, m1 = ~0u;
        bool pri;

        switch (depth) {
        case SPIKE_1BPP: {
            // fg/bg nibbles; no flips, no priority, 256 tiles.
            const uint8_t b = vr[(pat + name * 8u + py) & 0xFFFF];
            const uint32_t f0 = nib_bytes[b >> 4], f1 = nib_bytes[b & 15];
            const uint8_t fg = a >> 4, bg = a & 15;
            const uint32_t fw = (uint32_t)(pal << 4 | fg) * ONES, bw = (uint32_t)(pal << 4 | bg) * ONES;
            s0 = (fw & f0) | (bw & ~f0);
            s1 = (fw & f1) | (bw & ~f1);
            if (!solid_all) {
                const uint32_t fm = fg ? ~0u : 0, bm = bg ? ~0u : 0;
                m0 = (fm & f0) | (bm & ~f0);
                m1 = (fm & f1) | (bm & ~f1);
            }
            pri = false;
            break;
        }
        case SPIKE_2BPP: {
            const unsigned tile = name | (a & 0x80u) << 1;
            const unsigned prow = (a & 0x20) ? 7 - py : py;
            const uint8_t *p = vr + ((pat + tile * 16u + prow * 2u) & 0xFFFF);
            uint8_t b0 = p[0], b1 = p[1];
            if (a & 0x10) { const uint8_t t = rev2[b0]; b0 = rev2[b1]; b1 = t; }
            const uint32_t v0 = vals2[b0], v1 = vals2[b1];
            const uint32_t g = (uint32_t)((pal & 3) << 6 | (a & 15) << 2) * ONES;
            s0 = v0 | g;
            s1 = v1 | g;
            if (!solid_all) {
                m0 = nonzero_bytes(v0) * 0xFF;
                m1 = nonzero_bytes(v1) * 0xFF;
            }
            pri = a & 0x40;
            break;
        }
        case SPIKE_4BPP: {
            const unsigned tile = name | (a & 0x80u) << 1;
            const unsigned prow = (a & 0x20) ? 7 - py : py;
            uint32_t w = load32(vr + ((pat + tile * 32u + prow * 4u) & 0xFFFF));
            if (a & 0x10) {
                w = bswap32(w);
                w = (w >> 4 & 0x0F0F0F0Fu) | (w << 4 & 0xF0F0F0F0u);
            }
            if (tab) {
                const uint16_t *const t = tab4[a & 15];
                s0 = t[w & 255] | (uint32_t)t[w >> 8 & 255] << 16;
                s1 = t[w >> 16 & 255] | (uint32_t)t[w >> 24] << 16;
                if (!solid_all) {
                    m0 = tab4_solid[w & 255] | (uint32_t)tab4_solid[w >> 8 & 255] << 16;
                    m1 = tab4_solid[w >> 16 & 255] | (uint32_t)tab4_solid[w >> 24] << 16;
                }
            } else {
                // Byte k of the row holds pixels 2k (high nibble) and 2k + 1.
                const uint32_t v0 = (w >> 4 & 15) | (w & 15) << 8 | (w >> 12 & 15) << 16 | (w >> 8 & 15) << 24;
                const uint32_t v1 = (w >> 20 & 15) | (w >> 16 & 15) << 8 | (w >> 28) << 16 | (w >> 24 & 15) << 24;
                const uint32_t g = (uint32_t)((a & 15) << 4) * ONES;
                s0 = v0 | g;
                s1 = v1 | g;
                if (!solid_all) {
                    m0 = nonzero_bytes(v0) * 0xFF;
                    m1 = nonzero_bytes(v1) * 0xFF;
                }
            }
            pri = a & 0x40;
            break;
        }
        default: {  // SPIKE_8BPP
            const unsigned prow = (a & 0x20) ? 7 - py : py;
            const uint8_t *p = vr + ((pat + name * 64u + prow * 8u) & 0xFFFF);
            uint32_t w0 = load32(p), w1 = load32(p + 4);
            if (a & 0x10) { const uint32_t t = bswap32(w0); w0 = bswap32(w1); w1 = t; }
            s0 = w0;
            s1 = w1;
            if (!solid_all) {
                m0 = nonzero_bytes(w0) * 0xFF;
                m1 = nonzero_bytes(w1) * 0xFF;
            }
            pri = a & 0x40;
            break;
        }
        }

        const uint32_t cls = (pri ? cls_pri : cls_norm) * ONES;
        if (how == DRAW) {
            store32(di, s0);
            store32(di + 4, s1);
            store32(dp, cls);
            store32(dp + 4, cls);
        } else {
            const uint32_t p0 = load32(dp), p1 = load32(dp + 4);
            if (!pri) {     // a normal tile loses to a layer 0 priority tile, §12
                m0 &= ~((p0 >> 2 & ONES) * 0xFF);
                m1 &= ~((p1 >> 2 & ONES) * 0xFF);
            }
            store32(di, (load32(di) & ~m0) | (s0 & m0));
            store32(di + 4, (load32(di + 4) & ~m1) | (s1 & m1));
            store32(dp, (p0 & ~m0) | (cls & m0));
            store32(dp + 4, (p1 & ~m1) | (cls & m1));
        }
        di += 8;
        dp += 8;
    }
}

#define CELLS_VARIANTS(name, how_)                                                                         \
    static void name(const spike_t *v, const spike_layer_t *L, int line, uint8_t *pi, uint8_t *pp,        \
                     uint8_t cn, uint8_t cp) {                                                             \
        switch (L->depth) {                                                                                \
        case SPIKE_1BPP: cells(v, L, line, pi, pp, SPIKE_1BPP, false, how_, cn, cp); break;                \
        case SPIKE_2BPP: cells(v, L, line, pi, pp, SPIKE_2BPP, false, how_, cn, cp); break;                \
        case SPIKE_4BPP:                                                                                   \
            if (v->use_tab4) cells(v, L, line, pi, pp, SPIKE_4BPP, true, how_, cn, cp);                    \
            else cells(v, L, line, pi, pp, SPIKE_4BPP, false, how_, cn, cp);                               \
            break;                                                                                         \
        default: cells(v, L, line, pi, pp, SPIKE_8BPP, false, how_, cn, cp); break;                        \
        }                                                                                                  \
    }
CELLS_VARIANTS(cells_draw, DRAW)
CELLS_VARIANTS(cells_merge_opaque, MERGE_OPAQUE)
CELLS_VARIANTS(cells_merge, MERGE)

void spike_layer0(const spike_t *v, spike_line_t *ln, int line) {
    const spike_layer_t *L = &v->layer[0];
    uint8_t *pi = ln->idx_buf + SPIKE_SLACK_L + v->left;
    uint8_t *pp = ln->pr_buf + SPIKE_SLACK_L + v->left;

    if (L->enabled && L->opaque) {
        cells_draw(v, L, line, pi, pp, 0x00, 0x05);
    } else {
        memset(pi - SPIKE_SLACK_L, v->backdrop, v->width + SPIKE_SLACK_L + 8u);
        memset(pp - SPIKE_SLACK_L, 0, v->width + SPIKE_SLACK_L + 8u);
        if (L->enabled) cells_merge(v, L, line, pi, pp, 0x00, 0x05);
    }
}

void spike_layer1(const spike_t *v, spike_line_t *ln, int line) {
    const spike_layer_t *L = &v->layer[1];
    if (!L->enabled) return;
    uint8_t *pi = ln->idx_buf + SPIKE_SLACK_L + v->left;
    uint8_t *pp = ln->pr_buf + SPIKE_SLACK_L + v->left;
    if (L->opaque) cells_merge_opaque(v, L, line, pi, pp, 0x01, 0x03);
    else cells_merge(v, L, line, pi, pp, 0x01, 0x03);
}

// ---------------------------------------------------------------------------
// Sprites, §10

void spike_sprite_eval(spike_t *v, spike_line_t *ln, int line) {
    ln->nlist = 0;
    if (!v->spr_enabled) return;
    const uint8_t *const vr = v->vram;
    const int h = (v->size16 ? 16 : 8) << v->mag;
    const unsigned limit = v->sprlimit, count = v->sprcount;
    const bool d0term = v->d0term;
    uint32_t addr = v->sprattr;
    unsigned n = 0;
    bool dropped = false;
    for (unsigned s = 0; s < count; s++, addr += 4) {
        const unsigned y = vr[addr & 0xFFFF];
        if (d0term && y == 0xD0) break;
        const int top = y > 240 ? (int)y - 256 : (int)y;     // 241–255 enter from the top
        if ((unsigned)(line - top) >= (unsigned)h) continue;
        if (n < limit) {
            ln->list[n++] = (uint8_t)s;
        } else if (!dropped) {
            dropped = true;
            if (!v->ovf) {
                v->ovf = true;
                v->first_drop = (uint8_t)s;
            }
        }
    }
    ln->nlist = (uint8_t)n;
}

// A solid bit per pixel of an 8 × 8 pattern row, bit 0 leftmost.
static ALWAYS_INLINE unsigned row_solid(const uint8_t *row, const unsigned depth) {
    switch (depth) {
    case SPIKE_1BPP: return rev8[row[0]];
    case SPIKE_2BPP: return nzbits2[row[0]] | nzbits2[row[1]] << 4;
    case SPIKE_4BPP:
        return nzbits4[row[0]] | nzbits4[row[1]] << 2 | nzbits4[row[2]] << 4 | nzbits4[row[3]] << 6;
    default:
        return (nonzero_bytes(load32(row)) * 0x01020408u) >> 24 |
               ((nonzero_bytes(load32(row + 4)) * 0x01020408u) >> 24) << 4;
    }
}

// Detailed collision: per claim word, which sprites own which of its pixels.
typedef struct {
    uint8_t n;
    uint8_t who[32];
    uint32_t bits[32];
} owners_t;
static owners_t owners[12];

static ALWAYS_INLINE void sprites(spike_t *v, spike_line_t *ln, int line, const unsigned depth, const bool detailed) {
    const uint8_t *const vr = v->vram;
    const int W = v->width;
    const unsigned shift = v->mag;
    const unsigned size = v->size16 ? 16 : 8;
    const int wpx = (int)(size << shift);
    const unsigned row_bytes = 1u << depth, pat_bytes = 8u << depth, bpp = 1u << depth;
    const uint32_t sprpat = v->sprpat, sprattr = v->sprattr;
    const unsigned sprpal = v->sprpal;
    const bool collision = v->collision;
    uint8_t *const pi = ln->idx_buf + SPIKE_SLACK_L + v->left;
    const uint8_t *const pp = ln->pr_buf + SPIKE_SLACK_L + v->left;
    uint32_t claim[12] = { 0 };
    uint32_t map_lo = 0, map_hi = 0;
    bool col = false;
    uint8_t srow[40];     // the sprite's pixels on this line as indices, by screen column, padded

    if (detailed)
        for (unsigned w = 0; w < 12; w++) owners[w].n = 0;

    for (unsigned i = 0; i < ln->nlist; i++) {
        const unsigned s = ln->list[i];
        const uint8_t *const e = vr + ((sprattr + 4u * s) & 0xFFFF);
        const uint8_t y = e[0], a = e[3];
        const unsigned x9 = e[1] | (a & 0x80u) << 1;
        const int sx = x9 >= 384 ? (int)x9 - 512 : (int)x9;
        if (sx >= W || sx + wpx <= 0) continue;        // counted, not drawn

        const int top = y > 240 ? (int)y - 256 : (int)y;
        unsigned row = (unsigned)(line - top) >> shift;
        if (a & 0x20) row = size - 1 - row;

        // Rows of the two quadrant patterns this line crosses, §10: N, N+1 on
        // the left, N+2, N+3 on the right.
        const unsigned half = size == 16 && row >= 8;
        const uint32_t prow = (row & 7) * row_bytes;
        const uint8_t *const rl = vr + ((sprpat + (e[2] + half) * pat_bytes + prow) & 0xFFFF);
        const uint8_t *const rr = vr + ((sprpat + (e[2] + 2u + half) * pat_bytes + prow) & 0xFFFF);
        unsigned m = row_solid(rl, depth);
        if (size == 16) m |= row_solid(rr, depth) << 8;

        // Indices, four words of four pixels, before the flip.
        const uint8_t group = (uint8_t)((sprpal * 16u + (a & 15u)) << bpp);
        uint32_t w0, w1, w2 = 0, w3 = 0;
        switch (depth) {
        case SPIKE_1BPP: {
            const uint32_t g = (uint8_t)(group + 1) * ONES;
            w0 = nib_bytes[rl[0] >> 4] & g; w1 = nib_bytes[rl[0] & 15] & g;
            w2 = nib_bytes[rr[0] >> 4] & g; w3 = nib_bytes[rr[0] & 15] & g;
            break;
        }
        case SPIKE_2BPP: {
            const uint32_t g = group * ONES;
            w0 = vals2[rl[0]] | g; w1 = vals2[rl[1]] | g;
            w2 = vals2[rr[0]] | g; w3 = vals2[rr[1]] | g;
            break;
        }
        case SPIKE_4BPP: {
            const uint16_t *const t = tab4[a & 15];     // SPRPAL drops out at 4bpp, §10
            w0 = t[rl[0]] | (uint32_t)t[rl[1]] << 16; w1 = t[rl[2]] | (uint32_t)t[rl[3]] << 16;
            w2 = t[rr[0]] | (uint32_t)t[rr[1]] << 16; w3 = t[rr[2]] | (uint32_t)t[rr[3]] << 16;
            break;
        }
        default:
            w0 = load32(rl); w1 = load32(rl + 4);
            w2 = load32(rr); w3 = load32(rr + 4);
            break;
        }
        if (a & 0x10) {     // the whole sprite, quadrants included
            if (size == 16) {
                const uint32_t t0 = w0, t1 = w1;
                w0 = bswap32(w3); w1 = bswap32(w2); w2 = bswap32(t1); w3 = bswap32(t0);
                m = (unsigned)(rev8[m & 255] << 8 | rev8[m >> 8]);
            } else {
                const uint32_t t0 = w0;
                w0 = bswap32(w1); w1 = bswap32(t0);
                m = rev8[m];
            }
        }
        store32(srow, w0); store32(srow + 4, w1);
        store32(srow + 8, w2); store32(srow + 12, w3);
        uint32_t mm = m;
        if (shift) {
            for (unsigned c = size; c-- > 0;) srow[2 * c] = srow[2 * c + 1] = srow[c];
            mm = spread8[m & 255] | (uint32_t)spread8[m >> 8] << 16;
        }

        // Clip to the picture, then claim: collision is any solid pixel
        // already claimed, and only what is newly claimed is drawn.
        int x0 = sx;
        if (x0 < 0) { mm >>= -x0; x0 = 0; }
        const int x1 = sx + wpx > W ? W : sx + wpx;
        if (sx + wpx > W) mm &= (1u << (W - x0)) - 1;  // W - x0 < wpx ≤ 32
        if (!mm) continue;

        const unsigned k = (unsigned)x0 >> 5, b = (unsigned)x0 & 31;
        const uint32_t cl = claim[k], ch = claim[k + 1];
        const uint32_t cw = b ? (cl >> b | ch << (32 - b)) : cl;
        const uint32_t fresh = mm & ~cw;
        claim[k] = cl | mm << b;
        if (b) claim[k + 1] = ch | mm >> (32 - b);

        if ((mm & cw) && collision) {
            col = true;
            if (detailed) {
                const uint32_t ov = mm & cw;
                const uint32_t ol = ov << b, oh = b ? ov >> (32 - b) : 0;
                for (unsigned wd = 0; wd < 2; wd++) {
                    const uint32_t o_bits = wd ? oh : ol;
                    if (!o_bits) continue;
                    const owners_t *o = &owners[k + wd];
                    for (unsigned j = 0; j < o->n; j++) {
                        if (o->bits[j] & o_bits) {
                            if (o->who[j] < 32) map_lo |= 1u << o->who[j];
                            else map_hi |= 1u << (o->who[j] - 32);
                        }
                    }
                }
                if (s < 32) map_lo |= 1u << s;
                else map_hi |= 1u << (s - 32);
            }
        }
        if (detailed) {
            const uint32_t fl = fresh << b, fh = b ? fresh >> (32 - b) : 0;
            if (fl) { owners_t *o = &owners[k]; o->who[o->n] = (uint8_t)s; o->bits[o->n++] = fl; }
            if (fh) { owners_t *o = &owners[k + 1]; o->who[o->n] = (uint8_t)s; o->bits[o->n++] = fh; }
        }

        // Draw what this sprite newly owns, where no layer outranks it (§12),
        // four pixels at a time.
        const unsigned block_shift = (a & 0x40) ? 1 : 0;   // SPIKE_PR_BLOCK_SPRP or _SPR
        const uint8_t *sp = srow + (x0 - sx);
        uint8_t *di = pi + x0;
        const uint8_t *dp = pp + x0;
        for (int c = 0; c < x1 - x0; c += 4, sp += 4, di += 4, dp += 4) {
            const unsigned nib = (fresh >> c) & 15;
            if (!nib) continue;
            const uint32_t mask = nib_lsb_bytes[nib] & ~((load32(dp) >> block_shift & ONES) * 0xFF);
            store32(di, (load32(di) & ~mask) | (load32(sp) & mask));
        }
    }

    if (col) v->col = true;
    v->colmap |= (uint64_t)map_hi << 32 | map_lo;
}

void spike_sprites(spike_t *v, spike_line_t *ln, int line) {
    const bool det = v->collision && v->detailed;
    switch (v->spr_depth) {
    case SPIKE_1BPP: if (det) sprites(v, ln, line, SPIKE_1BPP, true); else sprites(v, ln, line, SPIKE_1BPP, false); break;
    case SPIKE_2BPP: if (det) sprites(v, ln, line, SPIKE_2BPP, true); else sprites(v, ln, line, SPIKE_2BPP, false); break;
    case SPIKE_4BPP: if (det) sprites(v, ln, line, SPIKE_4BPP, true); else sprites(v, ln, line, SPIKE_4BPP, false); break;
    default:         if (det) sprites(v, ln, line, SPIKE_8BPP, true); else sprites(v, ln, line, SPIKE_8BPP, false); break;
    }
}

// ---------------------------------------------------------------------------
// The border, then expansion: 320 indices to 640 12-bit pixels through the
// paired cache.

void spike_finish(const spike_t *v, spike_line_t *ln, uint16_t *rgb) {
    uint8_t *const idx = SPIKE_IDX(ln);
    const unsigned right = v->left + v->width;
    if (v->left) {
        memset(idx, v->backdrop, v->left);
        memset(idx + right, v->backdrop, SPIKE_WIDTH - right);
    }
    const uint32_t *const pal2 = v->pal2;
    for (unsigned x = 0; x < SPIKE_WIDTH; x += 4) {
        const uint32_t p0 = pal2[idx[x]], p1 = pal2[idx[x + 1]], p2 = pal2[idx[x + 2]], p3 = pal2[idx[x + 3]];
        memcpy(rgb + 2 * x, &p0, 4);
        memcpy(rgb + 2 * x + 2, &p1, 4);
        memcpy(rgb + 2 * x + 4, &p2, 4);
        memcpy(rgb + 2 * x + 6, &p3, 4);
    }
}

void spike_build_line(spike_t *v, spike_line_t *ln, int line, uint16_t *rgb) {
    spike_sprite_eval(v, ln, line);
    spike_layer0(v, ln, line);
    spike_layer1(v, ln, line);
    spike_sprites(v, ln, line);
    spike_finish(v, ln, rgb);
}
