// The renderer as Phase 7 left it: a pixel at a time, written to be checked
// (docs/results/phase-07.md). Kept here, unchanged but for names, as the
// reference Phase 8's word-at-a-time renderer is held to (test_render).
//
// Builds the row the render side has caught up to, on one thread, with every
// sprite drawn straight into it, and reports what the sprites collided, as
// vdp_build_line does.

#include "reference.h"

#include <string.h>

#include "vdp_internal.h"

// Phase 7's sprite line, before Phase 8 took its owner map for claim bitmaps.
typedef struct reference_sprline {
    int16_t x0, x1;
    bool collided;
    uint64_t collisions;
    uint8_t level[VDP_WIDTH];
    uint8_t index[VDP_WIDTH];
    uint8_t owner[VDP_WIDTH];
} reference_sprline_t;

// ---- layers ----

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
static const uint16_t reference_unpack4[16][256] = {
    UNPACK4_GROUP(0), UNPACK4_GROUP(1), UNPACK4_GROUP(2), UNPACK4_GROUP(3),
    UNPACK4_GROUP(4), UNPACK4_GROUP(5), UNPACK4_GROUP(6), UNPACK4_GROUP(7),
    UNPACK4_GROUP(8), UNPACK4_GROUP(9), UNPACK4_GROUP(10), UNPACK4_GROUP(11),
    UNPACK4_GROUP(12), UNPACK4_GROUP(13), UNPACK4_GROUP(14), UNPACK4_GROUP(15),
};

// One pattern row of a cell at 2, 4 or 8bpp: its eight pixels as palette
// indices, leftmost first and before any horizontal flip, and a bit for each
// pixel whose value is not 0 (§8's transparency), bit n for pixel n.
static inline unsigned layer_decode_row(const uint8_t *vram, uint16_t address, unsigned depth, uint8_t group,
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
        const uint16_t *pairs = reference_unpack4[group & 0x0f];
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

static void reference_draw_layer(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
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
            unsigned solid = layer_decode_row(vram, address, depth, group, indices);
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

// ---- sprites ----

// §10: 64 slots of four bytes.
#define SPRITE_SLOTS 64
#define SLOT_BYTES 4
#define SLOT_Y 0
#define SLOT_X 1
#define SLOT_PATTERN 2
#define SLOT_ATTRIBUTES 3

// A Y that ends the list while SPRCTRL b2 is set, or always in the legacy
// submode (§9, §10).
#define TERMINATOR 0xd0

// §10: Y 241-255 are -15…-1; X is nine bits, and 384-511 are -128…-1.
#define Y_NEGATIVE 241
#define X_NEGATIVE 384
#define X_RANGE 512

// §9: the TMS9918's reading. Y $E1-$FF are -31…-1 and the first row is drawn on
// the line after Y; attribute b7 is the early clock, 32 pixels left.
#define Y_NEGATIVE_LEGACY 0xe1
#define Y_OFFSET_LEGACY 1
#define EARLY_CLOCK_PIXELS 32

// §10's attribute byte. The legacy submode reads b3:0 as a palette index and
// ignores b4-b6.
#define SPRITE_ATTR_COLOUR 0x0f
#define SPRITE_ATTR_FLIP_X 0x10
#define SPRITE_ATTR_FLIP_Y 0x20
#define SPRITE_ATTR_PRIORITY 0x40
#define SPRITE_ATTR_X_BIT8 0x80

// §10: size is MODE1 b1 and magnification b0, for every sprite.
static inline unsigned sprite_size(const uint8_t *reg) {
    return (reg[VDP_REG_MODE1] & VDP_MODE1_SIZE16) ? 16 : 8;
}

static inline unsigned sprite_shift(const uint8_t *reg) {
    return (reg[VDP_REG_MODE1] & VDP_MODE1_MAG) ? 1 : 0;
}

// §10: a sprite's pattern values on its row, by pattern column before a
// horizontal flip — 8, or 16 from two quadrants. The pattern index counts 8 x 8
// patterns at every depth, and a 16 x 16's quadrants N to N + 3 are top left,
// bottom left, top right, bottom right. Pixels are MSB or high nibble first.
static inline void sprite_decode_row(const uint8_t *vram, const vdp_sprite_t *s, unsigned size, unsigned depth,
                              uint16_t table, uint8_t *values) {
    unsigned bits = 1u << depth;
    unsigned row_bytes = 1u << depth;
    unsigned pattern_bytes = 8u << depth;
    unsigned per_byte = 8u >> depth;
    unsigned mask = (1u << bits) - 1;
    for (unsigned half = 0; half < size; half += 8) {
        unsigned quadrant = size == 16 ? ((half >> 3) << 1) | (s->row >> 3) : 0;
        uint16_t address = (uint16_t)(table + (s->pattern + quadrant) * pattern_bytes + (s->row & 7) * row_bytes);
        for (unsigned column = 0; column < 8; column++) {
            uint8_t byte = vram[(uint16_t)(address + (column >> (3 - depth)))];
            unsigned shift = (per_byte - 1 - (column & (per_byte - 1))) * bits;
            values[half + column] = (uint8_t)((byte >> shift) & mask);
        }
    }
}

// Picture columns [x0, x1) of the line's sprites, into `s`'s claims. With
// `picture`, a pixel is drawn straight into it where its level beats the
// layer's in `levels` (§12); without, into `s` for a merge. Reads only the
// render side and the list; the collisions found are left in `s`.
static void reference_draw_columns(const vdp_t *v, reference_sprline_t *s, int x0, int x1, uint8_t *picture, const uint8_t *levels) {
    const uint8_t *reg = v->render_reg;
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(reg, &legacy);
    int width = g->width;
    if (x0 < 0) x0 = 0;
    if (x1 > width) x1 = width;
    // A line with no sprite on it costs nothing, and merges nothing.
    if (!v->sprite_count || x1 < x0) x1 = x0;
    s->x0 = (int16_t)x0;
    s->x1 = (int16_t)x1;
    s->collided = false;
    s->collisions = 0;
    if (x0 == x1) return;
    memset(s->level + x0, 0, (size_t)(x1 - x0));
    memset(s->owner + x0, 0, (size_t)(x1 - x0));

    // §9: the legacy submode pins sprites to 1bpp and ignores SPRPAL.
    uint8_t control = reg[VDP_REG_SPRCTRL];
    bool pinned = legacy != VDP_LEGACY_NONE;
    unsigned depth = pinned ? VDP_DEPTH_1BPP : (control & VDP_SPRCTRL_DEPTH) >> 4;
    bool collision = (control & VDP_SPRCTRL_COLLISION) != 0;
    bool detailed = collision && (control & VDP_SPRCTRL_DETAILED);  // §10: b3 does nothing while b1 is clear
    unsigned size = sprite_size(reg);
    unsigned shift = sprite_shift(reg);
    uint16_t table = (uint16_t)(reg[VDP_REG_SPRPAT] << 11);  // §5: x $800
    uint8_t palette_high = pinned ? 0 : (uint8_t)((reg[VDP_REG_SPRPAL] & 0x0f) << 4);
    uint8_t values[16];

    for (unsigned i = 0; i < v->sprite_count; i++) {
        const vdp_sprite_t *sprite = &v->sprite[i];
        int left = sprite->left;
        int from = left > x0 ? left : x0;
        int to = left + (int)(size << shift);
        if (to > x1) to = x1;
        if (from >= to) continue;  // counted, and drawn by the other core if anywhere

        sprite_decode_row(v->render_vram, sprite, size, depth, table, values);
        uint8_t attributes = sprite->attributes;
        bool flip = !pinned && (attributes & SPRITE_ATTR_FLIP_X);

        // §10's mapping, §8's with SPRPAL for LxPAL: ((SPRPAL x 16 + subpal) x
        // 2^bpp + value) & $FF. In the legacy submode b3:0 is the index itself,
        // into row 0; colour 0 is invisible but still collides (§9).
        uint8_t group = (uint8_t)(palette_high | (attributes & SPRITE_ATTR_COLOUR));
        uint8_t group_base = (uint8_t)(group << (1u << depth));
        bool invisible = pinned && (attributes & SPRITE_ATTR_COLOUR) == 0;
        // §12: b6 lifts the sprite above layer 1; legacy sprites stay at level 2.
        uint8_t level = (!pinned && (attributes & SPRITE_ATTR_PRIORITY)) ? VDP_LEVEL_SPRITE_FRONT : VDP_LEVEL_SPRITE;
        uint8_t owner = (uint8_t)(sprite->slot + 1);
        uint64_t bit = UINT64_C(1) << sprite->slot;

        for (int x = from; x < to; x++) {
            unsigned column = (unsigned)(x - left) >> shift;
            uint8_t value = values[flip ? size - 1 - column : column];
            if (!value) continue;  // 0 is transparent at every depth (§10)

            // Collision is on coverage, before any priority (§10, §12): a sprite
            // hidden behind another, or behind a layer, still collides. The
            // lowest slot on the pixel is paired with each one after it, so
            // every sprite on a pixel two share is named.
            uint8_t covering = s->owner[x];
            if (!covering) {
                s->owner[x] = owner;
            } else if (collision) {
                s->collided = true;
                if (detailed) s->collisions |= bit | (UINT64_C(1) << (covering - 1));
            }

            // The lowest slot to paint a pixel owns it (§10), even where a layer
            // then outranks it: a sprite behind does not show through.
            if (invisible || s->level[x]) continue;
            s->level[x] = level;
            uint8_t index = pinned ? group : (uint8_t)(group_base + value);
            if (!picture) {
                s->index[x] = index;
            } else if (level > levels[x]) {
                picture[x] = index;
            }
        }
    }
}


void reference_build_line(vdp_t *v, uint8_t *indices, bool *collided, uint64_t *collisions) {
    uint8_t backdrop = (uint8_t)(((v->render_reg[VDP_REG_L0PAL] & 0x0f) << 4) | (v->render_reg[VDP_REG_COLOR] & 0x0f));
    memset(indices, backdrop, VDP_WIDTH);
    static uint8_t levels[VDP_WIDTH];
    static reference_sprline_t line;
    *collided = false;
    *collisions = 0;
    if (v->render_screen_line >= VDP_HEIGHT) return;
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(v->render_reg, &legacy);
    uint16_t display = vdp_display_line_of(v->render_screen_line, g);
    if (display >= g->lines) return;
    if (!(v->render_reg[VDP_REG_MODE1] & VDP_MODE1_DISP)) return;
    uint8_t *picture = indices + g->origin_x;
    bool layer1 = (v->render_reg[VDP_REG_L1CTRL] & VDP_LXCTRL_ENABLE) != 0;
    uint8_t *lv = NULL;
    if (v->sprite_count || (layer1 && vdp_layer0_has_priority(v->render_reg, legacy))) {
        lv = levels;
        memset(lv, VDP_LEVEL_BACKDROP, g->width);
    }
    if (v->render_reg[VDP_REG_L0CTRL] & VDP_LXCTRL_ENABLE) reference_draw_layer(v, 0, display, g, legacy, picture, lv);
    if (layer1) reference_draw_layer(v, 1, display, g, VDP_LEGACY_NONE, picture, lv);
    reference_draw_columns(v, &line, 0, VDP_WIDTH, picture, levels);
    *collided = line.collided;
    *collisions = line.collisions;
}
