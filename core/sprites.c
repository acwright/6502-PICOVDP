// Sprites (§10), the legacy submode's sprites (§9), and where sprites stand
// among the layers (§12).
//
// Evaluation runs at the latch, from vdp_line_start, on the render side just
// brought up to date: a line that drops a sprite reports its overflow there, as
// §14 wants, and leaves the list of sprites it draws for the build. Drawing is
// the build's (PLAN.md section 3): vdp_build_sprites on core 0 into a sprite
// line, reading only the render side and writing no status; vdp_draw_sprites on
// core 1 straight into the line, after the layers; and vdp_merge_sprites on
// core 1, which brings core 0's sprite line in and publishes its collisions.
//
// Both halves draw with one worker over a range of picture columns. Priority
// among sprites and collision are decided pixel by pixel — a pixel belongs to
// the lowest slot that paints it, and collides if two slots cover it — so any
// division of the columns gives the same line and the same status.

#include "vdp_internal.h"

#include <string.h>

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
#define ATTR_COLOUR 0x0f
#define ATTR_FLIP_X 0x10
#define ATTR_FLIP_Y 0x20
#define ATTR_PRIORITY 0x40
#define ATTR_X_BIT8 0x80

// §10: size is MODE1 b1 and magnification b0, for every sprite.
static inline unsigned sprite_size(const uint8_t *reg) {
    return (reg[VDP_REG_MODE1] & VDP_MODE1_SIZE16) ? 16 : 8;
}

static inline unsigned sprite_shift(const uint8_t *reg) {
    return (reg[VDP_REG_MODE1] & VDP_MODE1_MAG) ? 1 : 0;
}

// §6, §14: a line dropped `slot`. STAT0's index field keeps the first since
// STAT0 was read; STAT7 follows the most recent line; the interrupt is the
// frame's first.
static void report_overflow(vdp_t *v, unsigned slot) {
    if (!(v->stat0 & VDP_STAT0_OVF)) v->stat0 |= (uint8_t)(VDP_STAT0_OVF | (slot & VDP_STAT0_SPRITE));
    vdp_frame_event(v, VDP_IRQ_OVERFLOW);
    v->overflow_sprite = (uint8_t)slot;
}

void VDP_HOT(vdp_sprites_evaluate)(vdp_t *v, bool publish) {
    v->sprite_count = 0;
    if (v->render_screen_line >= VDP_HEIGHT) return;

    // The picture's lines only, with the display on (§3, §10), and none at all
    // in legacy Text (§9). VMODE's Text geometry has them.
    const uint8_t *reg = v->render_reg;
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(reg, &legacy);
    uint16_t line = vdp_display_line_of(v->render_screen_line, g);
    if (line >= g->lines || !(reg[VDP_REG_MODE1] & VDP_MODE1_DISP) || legacy == VDP_LEGACY_TEXT) return;
    uint8_t control = reg[VDP_REG_SPRCTRL];
    if (!(control & VDP_SPRCTRL_ENABLE)) return;

    bool pinned = legacy != VDP_LEGACY_NONE;
    bool terminates = pinned || (control & VDP_SPRCTRL_TERMINATOR);
    // §5: SPRCOUNT bounds the table and SPRLIMIT the line, each against its
    // ceiling. SPRLIMIT 0 draws none and overflows on the first sprite covering
    // the line, which the comparison below does as it stands.
    unsigned slots = reg[VDP_REG_SPRCOUNT] < SPRITE_SLOTS ? reg[VDP_REG_SPRCOUNT] : SPRITE_SLOTS;
    unsigned limit = reg[VDP_REG_SPRLIMIT] < VDP_SPRITES_PER_LINE ? reg[VDP_REG_SPRLIMIT] : VDP_SPRITES_PER_LINE;
    unsigned size = sprite_size(reg);
    unsigned shift = sprite_shift(reg);
    int span = (int)(size << shift);

    const uint8_t *vram = v->render_vram;
    uint16_t table = (uint16_t)(reg[VDP_REG_SPRATTR] << 7);  // §5: x $80
    unsigned covering = 0;

    for (unsigned slot = 0; slot < slots; slot++) {
        uint16_t at = (uint16_t)(table + slot * SLOT_BYTES);
        uint8_t y = vram[(uint16_t)(at + SLOT_Y)];
        if (terminates && y == TERMINATOR) break;

        // Y is the top edge as a display line; a magnified sprite covers twice
        // the lines from the same top.
        int top = pinned ? (y >= Y_NEGATIVE_LEGACY ? y - 256 : y) + Y_OFFSET_LEGACY : (y >= Y_NEGATIVE ? y - 256 : y);
        int row = (int)line - top;
        if (row < 0 || (unsigned)row >> shift >= size) continue;

        // More than SPRLIMIT: the excess is dropped, highest index first, which
        // in table order is stopping at the first that does not fit (§10).
        if (covering >= limit) {
            if (publish) report_overflow(v, slot);
            break;
        }
        covering++;

        // X: nine bits (§10), or the legacy early clock (§9). A sprite wholly
        // off the picture's left or right counted, and draws nothing.
        uint8_t attributes = vram[(uint16_t)(at + SLOT_ATTRIBUTES)];
        int x = vram[(uint16_t)(at + SLOT_X)];
        int left;
        if (pinned) {
            left = (attributes & ATTR_X_BIT8) ? x - EARLY_CLOCK_PIXELS : x;
        } else {
            x |= (attributes & ATTR_X_BIT8) << 1;
            left = x >= X_NEGATIVE ? x - X_RANGE : x;
        }
        if (left >= (int)g->width || left + span <= 0) continue;

        // §10: a vertical flip is the whole sprite's, quadrants included, so it
        // is taken in sprite space. The legacy submode ignores it (§9).
        unsigned pattern_row = (unsigned)row >> shift;
        if (!pinned && (attributes & ATTR_FLIP_Y)) pattern_row = size - 1 - pattern_row;

        v->sprite[v->sprite_count++] = (vdp_sprite_t){
            .left = (int16_t)left,
            .slot = (uint8_t)slot,
            .row = (uint8_t)pattern_row,
            .pattern = vram[(uint16_t)(at + SLOT_PATTERN)],
            .attributes = attributes,
        };
    }
}

// §10: a sprite's pattern values on its row, by pattern column before a
// horizontal flip — 8, or 16 from two quadrants. The pattern index counts 8 x 8
// patterns at every depth, and a 16 x 16's quadrants N to N + 3 are top left,
// bottom left, top right, bottom right. Pixels are MSB or high nibble first.
static inline void decode_row(const uint8_t *vram, const vdp_sprite_t *s, unsigned size, unsigned depth,
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
static void VDP_HOT(draw_columns)(const vdp_t *v, vdp_sprline_t *s, int x0, int x1, uint8_t *picture, const uint8_t *levels) {
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

        decode_row(v->render_vram, sprite, size, depth, table, values);
        uint8_t attributes = sprite->attributes;
        bool flip = !pinned && (attributes & ATTR_FLIP_X);

        // §10's mapping, §8's with SPRPAL for LxPAL: ((SPRPAL x 16 + subpal) x
        // 2^bpp + value) & $FF. In the legacy submode b3:0 is the index itself,
        // into row 0; colour 0 is invisible but still collides (§9).
        uint8_t group = (uint8_t)(palette_high | (attributes & ATTR_COLOUR));
        uint8_t group_base = (uint8_t)(group << (1u << depth));
        bool invisible = pinned && (attributes & ATTR_COLOUR) == 0;
        // §12: b6 lifts the sprite above layer 1; legacy sprites stay at level 2.
        uint8_t level = (!pinned && (attributes & ATTR_PRIORITY)) ? VDP_LEVEL_SPRITE_FRONT : VDP_LEVEL_SPRITE;
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

// §6, §10, §14: COL, the frame's collision interrupt, and with SPRCTRL b3 the
// map. Every one of them only ever sets, so the order the halves of a line are
// published in, or publishing one twice, changes nothing.
static void publish(vdp_t *v, const vdp_sprline_t *s) {
    if (!s->collided) return;
    v->stat0 |= VDP_STAT0_COL;
    vdp_frame_event(v, VDP_IRQ_COLLISION);
    for (unsigned byte = 0; byte < sizeof v->collision_map; byte++) {
        v->collision_map[byte] |= (uint8_t)(s->collisions >> (8 * byte));
    }
}

void VDP_HOT(vdp_build_sprites)(const vdp_t *v, vdp_sprline_t *s, int x0, int x1) {
    draw_columns(v, s, x0, x1, NULL, NULL);
}

void VDP_HOT(vdp_draw_sprites)(vdp_t *v, uint8_t *indices, int x0, int x1) {
    const vdp_geometry_t *g = vdp_geometry(v->render_reg, NULL);
    draw_columns(v, &v->sprline, x0, x1, indices + g->origin_x, v->level);
    publish(v, &v->sprline);
}

void VDP_HOT(vdp_merge_sprites)(vdp_t *v, uint8_t *indices, const vdp_sprline_t *s) {
    uint8_t *picture = indices + vdp_geometry(v->render_reg, NULL)->origin_x;
    for (int x = s->x0; x < s->x1; x++) {
        if (s->level[x] > v->level[x]) picture[x] = s->index[x];
    }
    publish(v, s);
}

// PLAN.md section 3: the picture column core 0's sprites end at, on a 32-pixel
// boundary, balancing them against core 1's layers and the rest of the
// sprites. O(1), from the list's length: the sprites as if spread evenly
// across the picture. Until Phase 8 measures the real line, the costs are
// Phase 1's spike's, in cycles, rounded. 0 when there is nothing for core 0.
int VDP_HOT(vdp_split_choose)(const vdp_t *v) {
    if (!v->sprite_count) return 0;
    const uint8_t *reg = v->render_reg;
    int width = vdp_geometry(reg, NULL)->width;
    uint8_t control = reg[VDP_REG_SPRCTRL];
    bool detailed = (control & VDP_SPRCTRL_COLLISION) && (control & VDP_SPRCTRL_DETAILED);
    int64_t per_sprite = (detailed ? 290 : 200) + 7 * (int64_t)(sprite_size(reg) << sprite_shift(reg));
    int64_t sprites = v->sprite_count * per_sprite;
    int64_t layers = 0;
    if (reg[VDP_REG_L0CTRL] & VDP_LXCTRL_ENABLE) layers += 9 * width;
    if (reg[VDP_REG_L1CTRL] & VDP_LXCTRL_ENABLE) layers += 16 * width;
    const int64_t core0_fixed = 500, core1_fixed = 2000;

    // split/width x sprites + core0_fixed = layers + core1_fixed + (1 - split/width) x sprites
    int64_t numerator = layers + core1_fixed + sprites - core0_fixed;
    int split = numerator <= 0 ? 0 : (int)(numerator * width / (2 * sprites));
    split = (split + 16) & ~31;
    int floor = (width / 2) & ~31;
    if (split < floor) split = floor;
    if (split > width) split = width;
    return split;
}
