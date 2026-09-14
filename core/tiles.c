// The tile engine (§8): one layer, one display line.
//
// One engine for both layers. The layer number picks a register block (§5) and
// its two §12 levels, and nothing else in here knows which of the two it is
// drawing. Every mode is a name table mapping cells to patterns, a pattern
// table of pixels and a source of colour: Graphics I is the per-pattern-group
// source, Text is no attribute fetch at all (§9).
//
// Reads only the render side (PLAN.md section 3): render_reg and render_vram,
// as they stood at the latch.
//
// Phase 5: 1bpp, with all four attribute sources, COLOR, LxPAL, transparency
// and the legacy submode's pins. The cell walk takes the scroll registers
// (§13) from the start, because the walk is the same loop either way.
// Phase 6: each pixel written records its §12 level, for the sprites.
// Phase 7: 2, 4 and 8bpp, the attribute byte — flips, priority, pattern bit 8
// — §8's palette mapping, §18's 4bpp unpacking table, and a layer judged
// against the levels already on the line.
// Phase 8: 8-pixel cells a cell at a time in 32-bit words, as Phase 1's spike
// drew them: a cell's eight pixels are two words of indices and two of
// masks, merged against two words of levels. Phase 7's pixel loop, which the
// words are checked against (tests/unit/test_render), still draws Text's
// 6-pixel cells.

#include "vdp_internal.h"

#include <string.h>

// §8: a pattern is 8 rows of 8 pixels; at 1bpp, one byte to a row.
#define CELL_HEIGHT 8
#define CELL_PIXELS 8
#define PATTERN_BYTES_1BPP 8

// §5: a layer's seven registers, by offset from its block.
#define LREG_NAME 0
#define LREG_ATTR 1
#define LREG_PAT 2
#define LREG_SCRX 3
#define LREG_SCRY 4
#define LREG_CTRL 5
#define LREG_PAL 6

static const uint8_t layer_block[2] = {VDP_REG_L0NAME, VDP_REG_L1NAME};

// §12: each layer's level with attribute b6 clear, and with it set.
static const uint8_t layer_level[2] = {VDP_LEVEL_LAYER0, VDP_LEVEL_LAYER1};
static const uint8_t layer_level_front[2] = {VDP_LEVEL_LAYER0_FRONT, VDP_LEVEL_LAYER1_FRONT};

// §8's attribute byte at 2, 4 and 8bpp.
#define ATTR_SUBPALETTE 0x0f
#define ATTR_FLIP_X 0x10
#define ATTR_FLIP_Y 0x20
#define ATTR_PRIORITY 0x40
#define ATTR_PATTERN_BIT8 0x80

// One pattern row of a cell at 2, 4 or 8bpp: its eight pixels as palette
// indices, leftmost first and before any horizontal flip, and a bit for each
// pixel whose value is not 0 (§8's transparency), bit n for pixel n.
static inline unsigned decode_row(const uint8_t *vram, uint16_t address, unsigned depth, uint8_t group,
                                  uint8_t indices[CELL_PIXELS]) {
    unsigned solid = 0;
    switch (depth) {
    case VDP_DEPTH_2BPP: {
        // §8: (group × 4 + value) & $FF.
        uint8_t base = (uint8_t)(group << 2);
        for (unsigned i = 0; i < CELL_PIXELS; i++) {
            unsigned value = (vram[(uint16_t)(address + (i >> 2))] >> (6 - 2 * (i & 3))) & 3;
            indices[i] = (uint8_t)(base + value);
            if (value) solid |= 1u << i;
        }
        break;
    }
    case VDP_DEPTH_4BPP: {
        // Through the table: a value of 0 is an index whose low nibble is 0.
        const uint16_t *pairs = vdp_unpack4[group & 0x0f];
        for (unsigned i = 0; i < CELL_PIXELS; i += 2) {
            uint16_t pair = pairs[vram[(uint16_t)(address + (i >> 1))]];
            indices[i] = (uint8_t)pair;
            indices[i + 1] = (uint8_t)(pair >> 8);
            if (pair & 0x000f) solid |= 1u << i;
            if (pair & 0x0f00) solid |= 2u << i;
        }
        break;
    }
    default:
        // 8bpp: one group of 256, so the value is the index.
        for (unsigned i = 0; i < CELL_PIXELS; i++) {
            indices[i] = vram[(uint16_t)(address + i)];
            if (indices[i]) solid |= 1u << i;
        }
        break;
    }
    return solid;
}

// Phase 7's pixel loop: any cell width.
static void VDP_HOT(draw_pixels)(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
                                  vdp_legacy_mode_t legacy, uint8_t *pixels, uint8_t *levels, unsigned x0, unsigned x1) {
    const uint8_t *reg = v->render_reg + layer_block[layer];
    const uint8_t *vram = v->render_vram;
    uint8_t control = reg[LREG_CTRL];
    // §9 pins layer 0's depth and attribute source in the legacy submode, and
    // ignores its index 0 opaque bit: TMS9918 colour 0 is transparent. Layer 1
    // is unaffected, so the caller passes VDP_LEGACY_NONE for it.
    bool pinned = legacy != VDP_LEGACY_NONE;

    unsigned depth = pinned ? VDP_DEPTH_1BPP : control & VDP_LXCTRL_DEPTH;
    unsigned source = pinned ? (legacy == VDP_LEGACY_TEXT ? VDP_ATTR_NONE : VDP_ATTR_PER_GROUP)
                             : (control & VDP_LXCTRL_ATTR_SOURCE) >> 2;
    bool opaque = !pinned && (control & VDP_LXCTRL_INDEX0_OPAQUE);
    uint8_t palette_high = (uint8_t)((reg[LREG_PAL] & 0x0f) << 4);
    uint8_t colour = v->render_reg[VDP_REG_COLOR];

    // §12. Layer 0 is drawn first, over the backdrop, and wins every pixel it
    // writes; layer 1 is judged against it. Where `levels` is NULL nothing on
    // the line can outrank this layer, and nothing later needs its levels.
    uint8_t normal = layer_level[layer];
    uint8_t front = layer_level_front[layer];
    bool judged = levels && normal != VDP_LEVEL_LAYER0;

    // §5: 1 KB granules for the name and attribute tables, 2 KB for patterns,
    // but ×$40 for layer 0's attribute table in the legacy submode (§9). Every
    // table address wraps at 64 KB (§7), which uint16_t arithmetic does.
    uint16_t name_base = (uint16_t)(reg[LREG_NAME] << 10);
    uint16_t attr_base = (uint16_t)(reg[LREG_ATTR] << (pinned ? 6 : 10));
    uint16_t pattern_base = (uint16_t)(reg[LREG_PAT] << 11);

    // §13: the map is the picture, and wraps onto itself. X is nine bits, the
    // register and LxCTRL b6.
    unsigned width = g->width;
    unsigned scroll_x = ((control & VDP_LXCTRL_SCRX_BIT8 ? 0x100u : 0) | reg[LREG_SCRX]) % width;
    unsigned map_y = line + reg[LREG_SCRY] % g->lines;
    if (map_y >= g->lines) map_y -= g->lines;

    unsigned row = map_y & (CELL_HEIGHT - 1);
    uint16_t cell_base = (uint16_t)((map_y / CELL_HEIGHT) * g->cols);
    uint16_t name_row = (uint16_t)(name_base + cell_base);

    // §8: a tile is 8 << depth bytes and a pattern row 1 << depth.
    unsigned tile_bytes = CELL_HEIGHT << depth;
    unsigned row_bytes = 1u << depth;

    // Cells, not columns: with a scroll that is not a multiple of the cell
    // width the first and last cells are partial, so the walk starts wherever
    // column x0 lands in the map and takes as much of each cell as fits.
    unsigned cell_width = g->cell_width;
    unsigned map_x = (scroll_x + x0) % width;
    unsigned col = map_x / cell_width;
    unsigned first = map_x - col * cell_width;
    unsigned x = x0;
    while (x < x1) {
        unsigned count = cell_width - first;
        if (count > x1 - x) count = x1 - x;
        uint8_t *out = pixels + x;

        unsigned pattern = vram[(uint16_t)(name_row + col)];

        // §8's attribute sources, each addressed by the 8-bit name byte and
        // the row before any flip. "None" is COLOR at 1bpp; elsewhere it is a
        // byte with no flip, no priority and no ninth bit, whose sub-palette
        // is LxPAL at 4bpp — the layer draws from palette row LxPAL — and 0 at
        // 2bpp, where LxPAL already picks the quarter.
        uint8_t attribute;
        switch (source) {
        case VDP_ATTR_PER_CELL:
            attribute = vram[(uint16_t)(attr_base + cell_base + col)];
            break;
        case VDP_ATTR_PER_GROUP:
            attribute = vram[(uint16_t)(attr_base + (pattern >> 3))];
            break;
        case VDP_ATTR_PER_ROW:
            attribute = vram[(uint16_t)(attr_base + pattern * CELL_HEIGHT + row)];
            break;
        default:
            attribute = depth == VDP_DEPTH_1BPP ? colour : depth == VDP_DEPTH_4BPP ? palette_high >> 4 : 0;
            break;
        }

        if (depth == VDP_DEPTH_1BPP) {
            // §8: foreground b7:4, background b3:0, each a 4-bit index into the
            // sixteen colours LxPAL names. A nibble of 0 is transparent, either
            // one, unless index 0 is opaque. No priority bit: b6 is half the
            // foreground.
            unsigned foreground = attribute >> 4;
            unsigned background = attribute & 0x0f;
            bool fg_opaque = foreground != 0 || opaque;
            bool bg_opaque = background != 0 || opaque;
            uint8_t fg = (uint8_t)(palette_high | foreground);
            uint8_t bg = (uint8_t)(palette_high | background);

            // Pattern bits leftmost first, from b7 (§8); a partial cell starts
            // partway into the byte. In Text only the top six bits are reached.
            unsigned bits = (unsigned)(vram[(uint16_t)(pattern_base + pattern * PATTERN_BYTES_1BPP + row)] << first);

            if (fg_opaque && bg_opaque && !judged) {
                // Every pixel written: the BIOS console's loop, with no branch.
                for (unsigned i = 0; i < count; i++, bits <<= 1) out[i] = (bits & 0x80) ? fg : bg;
                if (levels) memset(levels + x, normal, count);
            } else {
                // A transparent pixel is one not written, which leaves what is
                // behind it: the backdrop, or layer 0 under layer 1 (§12).
                for (unsigned i = 0; i < count; i++, bits <<= 1) {
                    bool on = (bits & 0x80) != 0;
                    if (!(on ? fg_opaque : bg_opaque)) continue;
                    if (levels) {
                        if (normal <= levels[x + i]) continue;
                        levels[x + i] = normal;
                    }
                    out[i] = on ? fg : bg;
                }
            }
        } else {
            // §8's attribute byte. 8bpp ignores the sub-palette — the group's
            // shift leaves nothing of it — and the ninth bit: 512 tiles of 64
            // bytes do not fit in 64 KB.
            if (depth != VDP_DEPTH_8BPP && (attribute & ATTR_PATTERN_BIT8)) pattern |= 0x100;
            unsigned pattern_row = (attribute & ATTR_FLIP_Y) ? CELL_HEIGHT - 1 - row : row;
            uint16_t address = (uint16_t)(pattern_base + pattern * tile_bytes + pattern_row * row_bytes);
            // §8's mapping: group LxPAL × 16 + sub-palette, the index
            // (group × 2^bpp + value) & $FF.
            uint8_t group = (uint8_t)(palette_high | (attribute & ATTR_SUBPALETTE));
            uint8_t indices[CELL_PIXELS];
            unsigned solid = decode_row(vram, address, depth, group, indices);
            if (opaque) solid = 0xff;
            bool flip = (attribute & ATTR_FLIP_X) != 0;
            uint8_t level = (attribute & ATTR_PRIORITY) ? front : normal;

            for (unsigned i = 0; i < count; i++) {
                // A flip mirrors the pixels the cell draws: in Text, the
                // leftmost six, not all eight (§8).
                unsigned column = first + i;
                unsigned from = flip ? cell_width - 1 - column : column;
                if (!(solid >> from & 1)) continue;
                if (levels) {
                    if (judged && level <= levels[x + i]) continue;
                    levels[x + i] = level;
                }
                out[i] = indices[from];
            }
        }

        x += count;
        first = 0;
        if (++col >= g->cols) col = 0;
    }
}

// ---- Phase 8: 8-pixel cells in words ----

enum { PLAIN, RECORD, JUDGE };  // levels: none kept; layer 0 recording its own; layer 1 judged too

// One layer's line of 8-pixel cells. Everything a cell costs is decided before
// the walk, and `depth`, `source` and `how` are constants in each copy the
// dispatcher below makes of it.
static inline __attribute__((always_inline)) void cells(const vdp_t *v, unsigned layer, uint16_t line,
                                                        const vdp_geometry_t *g, vdp_legacy_mode_t legacy,
                                                        uint8_t *pixels, uint8_t *levels, const unsigned x0,
                                                        const unsigned x1, const unsigned depth,
                                                        const unsigned source, const int how, const bool opaque) {
    const uint8_t *reg = v->render_reg + layer_block[layer];
    const uint8_t *vram = v->render_vram;
    const uint8_t control = reg[LREG_CTRL];
    const bool pinned = legacy != VDP_LEGACY_NONE;
    const uint8_t palette_high = (uint8_t)((reg[LREG_PAL] & 0x0f) << 4);
    const uint8_t colour = v->render_reg[VDP_REG_COLOR];
    const uint8_t normal = layer_level[layer], front = layer_level_front[layer];

    const uint16_t name_base = (uint16_t)(reg[LREG_NAME] << 10);
    const uint16_t attr_base = (uint16_t)(reg[LREG_ATTR] << (pinned ? 6 : 10));
    const uint16_t pattern_base = (uint16_t)(reg[LREG_PAT] << 11);

    // §13, as the pixel loop takes it.
    const unsigned width = g->width, cols = g->cols;
    const unsigned scroll_x = ((control & VDP_LXCTRL_SCRX_BIT8 ? 0x100u : 0) | reg[LREG_SCRX]) % width;
    unsigned map_y = line + reg[LREG_SCRY] % g->lines;
    if (map_y >= g->lines) map_y -= g->lines;
    const unsigned row = map_y & (CELL_HEIGHT - 1);
    const uint16_t cell_base = (uint16_t)((map_y / CELL_HEIGHT) * cols);
    const uint16_t name_row = (uint16_t)(name_base + cell_base);
    const unsigned tile_bytes = CELL_HEIGHT << depth, row_bytes = 1u << depth;

    // "None" (§8): COLOR at 1bpp; at depth a byte of no flip, priority or bit
    // 8, whose sub-palette is LxPAL at 4bpp and 0 otherwise.
    const uint8_t none = depth == VDP_DEPTH_1BPP ? colour : depth == VDP_DEPTH_4BPP ? palette_high >> 4 : 0;

    // Whole cells from the one column x0 lands in, the first starting left of
    // it by its offset into the cell (the half's slack, or its neighbour's
    // columns, which are not the half's).
    const unsigned map_x = (scroll_x + x0) % width;
    unsigned col = map_x / CELL_PIXELS;
    uint8_t *out = pixels + x0 - (map_x & (CELL_PIXELS - 1));
    uint8_t *lv = levels ? levels + x0 - (map_x & (CELL_PIXELS - 1)) : NULL;
    const uint8_t *const end = pixels + x1;

    for (; out < end; out += CELL_PIXELS) {
        unsigned pattern = vram[(uint16_t)(name_row + col)];
        uint8_t attribute;
        switch (source) {
        case VDP_ATTR_PER_CELL:
            attribute = vram[(uint16_t)(attr_base + cell_base + col)];
            break;
        case VDP_ATTR_PER_GROUP:
            attribute = vram[(uint16_t)(attr_base + (pattern >> 3))];
            break;
        case VDP_ATTR_PER_ROW:
            attribute = vram[(uint16_t)(attr_base + pattern * CELL_HEIGHT + row)];
            break;
        default:
            attribute = none;
            break;
        }
        if (++col >= cols) col = 0;

        uint32_t w0, w1, m0, m1;
        uint8_t level = normal;
        if (depth == VDP_DEPTH_1BPP) {
            // Foreground b7:4 where the pattern bit is set, background b3:0
            // where it is not; a nibble of 0 transparent unless index 0 is opaque.
            const uint8_t bits = vram[(uint16_t)(pattern_base + pattern * PATTERN_BYTES_1BPP + row)];
            const uint32_t f0 = vdp_nibble_msb[bits >> 4], f1 = vdp_nibble_msb[bits & 15];
            const unsigned foreground = attribute >> 4, background = attribute & 0x0f;
            const uint32_t fg = (uint32_t)(palette_high | foreground) * VDP_ONES;
            const uint32_t bg = (uint32_t)(palette_high | background) * VDP_ONES;
            w0 = (fg & f0) | (bg & ~f0);
            w1 = (fg & f1) | (bg & ~f1);
            const uint32_t fm = (foreground || opaque) ? ~0u : 0, bm = (background || opaque) ? ~0u : 0;
            m0 = (fm & f0) | (bm & ~f0);
            m1 = (fm & f1) | (bm & ~f1);
        } else {
            if (depth != VDP_DEPTH_8BPP && (attribute & ATTR_PATTERN_BIT8)) pattern |= 0x100;
            const unsigned pattern_row = (attribute & ATTR_FLIP_Y) ? CELL_HEIGHT - 1 - row : row;
            // The guard past $FFFF lets a row be read whole where it wraps (§7).
            const uint8_t *p = vram + (uint16_t)(pattern_base + pattern * tile_bytes + pattern_row * row_bytes);
            const uint8_t group = (uint8_t)(palette_high | (attribute & ATTR_SUBPALETTE));
            if (depth == VDP_DEPTH_2BPP) {
                // (group × 4 + value) & $FF.
                const uint32_t v0 = vdp_values2[p[0]], v1 = vdp_values2[p[1]];
                const uint32_t base = (uint32_t)(uint8_t)(group << 2) * VDP_ONES;
                w0 = base | v0;
                w1 = base | v1;
                m0 = vdp_nonzero_bytes(v0);
                m1 = vdp_nonzero_bytes(v1);
            } else if (depth == VDP_DEPTH_4BPP) {
                // Through the table; a value of 0 is an index whose low nibble is 0.
                const uint32_t bytes = vdp_load32(p);
                const uint16_t *pairs = vdp_unpack4[group & 0x0f];
                w0 = pairs[bytes & 0xff] | (uint32_t)pairs[bytes >> 8 & 0xff] << 16;
                w1 = pairs[bytes >> 16 & 0xff] | (uint32_t)pairs[bytes >> 24] << 16;
                m0 = vdp_nonzero_bytes(w0 & 0x0f0f0f0fu);
                m1 = vdp_nonzero_bytes(w1 & 0x0f0f0f0fu);
            } else {
                w0 = vdp_load32(p);
                w1 = vdp_load32(p + 4);
                m0 = vdp_nonzero_bytes(w0);
                m1 = vdp_nonzero_bytes(w1);
            }
            if (attribute & ATTR_FLIP_X) {
                const uint32_t t = w0, u = m0;
                w0 = vdp_swap32(w1);
                w1 = vdp_swap32(t);
                m0 = vdp_swap32(m1);
                m1 = vdp_swap32(u);
            }
            if (attribute & ATTR_PRIORITY) level = front;
        }
        if (opaque) {
            // Every pixel: nothing under the cell survives it, unless judged.
            if (how != JUDGE) {
                if (how == RECORD) {
                    vdp_store32(lv, (uint32_t)level * VDP_ONES);
                    vdp_store32(lv + 4, (uint32_t)level * VDP_ONES);
                    lv += CELL_PIXELS;
                }
                vdp_store32(out, w0);
                vdp_store32(out + 4, w1);
                continue;
            }
            m0 = m1 = ~0u;
        }

        if (how != PLAIN) {
            const uint32_t l0 = vdp_load32(lv), l1 = vdp_load32(lv + 4);
            if (how == JUDGE) {
                m0 &= vdp_beaten(l0, level);
                m1 &= vdp_beaten(l1, level);
            }
            const uint32_t lw = (uint32_t)level * VDP_ONES;
            vdp_store32(lv, (l0 & ~m0) | (lw & m0));
            vdp_store32(lv + 4, (l1 & ~m1) | (lw & m1));
            lv += CELL_PIXELS;
        }
        vdp_store32(out, (vdp_load32(out) & ~m0) | (w0 & m0));
        vdp_store32(out + 4, (vdp_load32(out + 4) & ~m1) | (w1 & m1));
    }
}

#define CELLS_FOR_SOURCE(depth, how)                                                                                  \
    switch (source) {                                                                                                 \
    case VDP_ATTR_PER_CELL: cells(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, VDP_ATTR_PER_CELL, how, opaque); break;   \
    case VDP_ATTR_PER_GROUP: cells(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, VDP_ATTR_PER_GROUP, how, opaque); break; \
    case VDP_ATTR_PER_ROW: cells(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, VDP_ATTR_PER_ROW, how, opaque); break;     \
    default: cells(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, VDP_ATTR_NONE, how, opaque); break;                      \
    }

#define CELLS_FOR_DEPTH(name, how, opaque_)                                                                   \
    static void VDP_HOT(name)(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,          \
                              vdp_legacy_mode_t legacy, uint8_t *pixels, uint8_t *levels, unsigned x0,         \
                              unsigned x1, unsigned depth, unsigned source) {                                 \
        const bool opaque = opaque_;                                                                          \
        switch (depth) {                                                                                      \
        case VDP_DEPTH_1BPP: CELLS_FOR_SOURCE(VDP_DEPTH_1BPP, how) break;                                     \
        case VDP_DEPTH_2BPP: CELLS_FOR_SOURCE(VDP_DEPTH_2BPP, how) break;                                     \
        case VDP_DEPTH_4BPP: CELLS_FOR_SOURCE(VDP_DEPTH_4BPP, how) break;                                     \
        default: CELLS_FOR_SOURCE(VDP_DEPTH_8BPP, how) break;                                                 \
        }                                                                                                     \
    }

CELLS_FOR_DEPTH(cells_plain, PLAIN, false)
CELLS_FOR_DEPTH(cells_record, RECORD, false)
CELLS_FOR_DEPTH(cells_judge, JUDGE, false)
// Index 0 opaque: every pixel of every cell written.
CELLS_FOR_DEPTH(cells_plain_opaque, PLAIN, true)
CELLS_FOR_DEPTH(cells_record_opaque, RECORD, true)
CELLS_FOR_DEPTH(cells_judge_opaque, JUDGE, true)

void VDP_HOT(vdp_draw_layer)(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
                             vdp_legacy_mode_t legacy, uint8_t *pixels, uint8_t *levels, int from, int to) {
    const unsigned x0 = from < 0 ? 0 : (unsigned)from;
    const unsigned x1 = to > g->width ? g->width : (unsigned)to;
    if (x1 <= x0) return;
    if (g->cell_width != CELL_PIXELS) {
        draw_pixels(v, layer, line, g, legacy, pixels, levels, x0, x1);
        return;
    }
    const uint8_t control = v->render_reg[layer_block[layer] + LREG_CTRL];
    const bool pinned = legacy != VDP_LEGACY_NONE;
    const unsigned depth = pinned ? VDP_DEPTH_1BPP : control & VDP_LXCTRL_DEPTH;
    const unsigned source = pinned ? (legacy == VDP_LEGACY_TEXT ? VDP_ATTR_NONE : VDP_ATTR_PER_GROUP)
                                   : (control & VDP_LXCTRL_ATTR_SOURCE) >> 2;
    // §9: the legacy submode ignores index 0 opaque — TMS9918 colour 0 is transparent.
    const bool opaque = !pinned && (control & VDP_LXCTRL_INDEX0_OPAQUE);
    // §12: layer 0 is drawn first and wins every pixel it writes; layer 1 is
    // judged against the levels there, where levels are kept.
    if (opaque) {
        if (!levels) cells_plain_opaque(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
        else if (layer_level[layer] == VDP_LEVEL_LAYER0) cells_record_opaque(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
        else cells_judge_opaque(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
    } else {
        if (!levels) cells_plain(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
        else if (layer_level[layer] == VDP_LEVEL_LAYER0) cells_record(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
        else cells_judge(v, layer, line, g, legacy, pixels, levels, x0, x1, depth, source);
    }
}
