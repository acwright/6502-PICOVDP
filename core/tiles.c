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
static const uint16_t unpack4[16][256] = {
    UNPACK4_GROUP(0), UNPACK4_GROUP(1), UNPACK4_GROUP(2), UNPACK4_GROUP(3),
    UNPACK4_GROUP(4), UNPACK4_GROUP(5), UNPACK4_GROUP(6), UNPACK4_GROUP(7),
    UNPACK4_GROUP(8), UNPACK4_GROUP(9), UNPACK4_GROUP(10), UNPACK4_GROUP(11),
    UNPACK4_GROUP(12), UNPACK4_GROUP(13), UNPACK4_GROUP(14), UNPACK4_GROUP(15),
};

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
        const uint16_t *pairs = unpack4[group & 0x0f];
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

void VDP_HOT(vdp_draw_layer)(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
                             vdp_legacy_mode_t legacy, uint8_t *pixels, uint8_t *levels) {
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
    // column 0 lands in the map and takes as much of each cell as fits.
    unsigned cell_width = g->cell_width;
    unsigned col = scroll_x / cell_width;
    unsigned first = scroll_x - col * cell_width;
    unsigned x = 0;
    while (x < width) {
        unsigned count = cell_width - first;
        if (count > width - x) count = width - x;
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
