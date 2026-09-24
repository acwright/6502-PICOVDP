// Sprites (§10), the legacy submode's sprites (§9), and where sprites stand
// among the layers (§12).
//
// Evaluation runs at the latch's catch-up, on the render side just brought up
// to date, and leaves the list of sprites the line draws for the build and the
// slot it dropped, if any, for vdp_publish. Drawing is the build's (PLAN.md
// section 3): each core draws the sprites over its half of the row straight
// into its own line, after its layers, reading only the render side and
// writing no status. vdp_publish, on core 1, reports the row's overflow and both
// halves' collisions (line.c). Through vdp_line_start and vdp_build_line that is
// at the latch and in the build, as §14 wants.
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
void VDP_HOT(vdp_report_overflow)(vdp_t *v, unsigned slot) {
    if (!(v->stat0 & VDP_STAT0_OVF)) v->stat0 |= (uint8_t)(VDP_STAT0_OVF | (slot & VDP_STAT0_SPRITE));
    vdp_frame_event(v, VDP_IRQ_OVERFLOW);
    v->overflow_sprite = (uint8_t)slot;
}

// The walk down the table, with the legacy reading or VMODE's a constant.
static inline __attribute__((always_inline)) uint8_t evaluate(vdp_t *v, const unsigned line, const unsigned width,
                                                              const unsigned slots, const unsigned limit,
                                                              const unsigned size, const unsigned shift,
                                                              const bool terminates, const bool pinned) {
    const unsigned span = size << shift;
    // SPRATTR x $80 + 64 slots never passes $8080: the table does not wrap.
    const uint8_t *slot_bytes = v->render_vram + (v->render_reg[VDP_REG_SPRATTR] << 7);
    vdp_sprite_t *out = v->sprite;
    unsigned covering = 0;

    for (unsigned slot = 0; slot < slots; slot++, slot_bytes += SLOT_BYTES) {
        const unsigned y = slot_bytes[SLOT_Y];
        if (terminates && y == TERMINATOR) break;

        // Y is the top edge as a display line; a magnified sprite covers twice
        // the lines from the same top. The row is unsigned, so a line above
        // the top is a row too large.
        unsigned row;
        if (pinned) {
            row = line - (y >= Y_NEGATIVE_LEGACY ? y - 256 : y) - Y_OFFSET_LEGACY;
        } else {
            row = y >= Y_NEGATIVE ? line + 256 - y : line - y;
        }
        if (row >> shift >= size) continue;

        // More than SPRLIMIT: the excess is dropped, highest index first, which
        // in table order is stopping at the first that does not fit (§10).
        if (covering >= limit) return (uint8_t)(slot + 1);
        covering++;

        // X: nine bits (§10), or the legacy early clock (§9). A sprite wholly
        // off the picture's left or right counted, and draws nothing.
        const uint8_t attributes = slot_bytes[SLOT_ATTRIBUTES];
        int left = slot_bytes[SLOT_X];
        if (pinned) {
            if (attributes & ATTR_X_BIT8) left -= EARLY_CLOCK_PIXELS;
        } else {
            left |= (attributes & ATTR_X_BIT8) << 1;
            if (left >= X_NEGATIVE) left -= X_RANGE;
        }
        if (left >= (int)width || left + (int)span <= 0) continue;

        // §10: a vertical flip is the whole sprite's, quadrants included, so it
        // is taken in sprite space. The legacy submode ignores it (§9).
        row >>= shift;
        if (!pinned && (attributes & ATTR_FLIP_Y)) row = size - 1 - row;
        out->left = (int16_t)left;
        out->slot = (uint8_t)slot;
        out->row = (uint8_t)row;
        out->pattern = slot_bytes[SLOT_PATTERN];
        out->attributes = attributes;
        out++;
        v->sprite_count++;
    }
    return 0;
}

uint8_t VDP_HOT(vdp_sprites_evaluate)(vdp_t *v) {
    v->sprite_count = 0;
    if (v->render_screen_line >= VDP_HEIGHT) return 0;

    // The picture's lines only, with the display on (§3, §10), and none at all
    // in legacy Text (§9). VMODE's Text geometry has them.
    const uint8_t *reg = v->render_reg;
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(reg, &legacy);
    const unsigned line = vdp_display_line_of(v->render_screen_line, g);
    if (line >= g->lines || !(reg[VDP_REG_MODE1] & VDP_MODE1_DISP) || legacy == VDP_LEGACY_TEXT) return 0;
    const uint8_t control = reg[VDP_REG_SPRCTRL];
    if (!(control & VDP_SPRCTRL_ENABLE)) return 0;

    const bool pinned = legacy != VDP_LEGACY_NONE;
    const bool terminates = pinned || (control & VDP_SPRCTRL_TERMINATOR);
    // §5: SPRCOUNT bounds the table and SPRLIMIT the line, each against its
    // ceiling. SPRLIMIT 0 draws none and overflows on the first sprite covering
    // the line, which the comparison in the walk does as it stands.
    const unsigned slots = reg[VDP_REG_SPRCOUNT] < SPRITE_SLOTS ? reg[VDP_REG_SPRCOUNT] : SPRITE_SLOTS;
    const unsigned limit = reg[VDP_REG_SPRLIMIT] < VDP_SPRITES_PER_LINE ? reg[VDP_REG_SPRLIMIT] : VDP_SPRITES_PER_LINE;
    if (pinned) return evaluate(v, line, g->width, slots, limit, sprite_size(reg), sprite_shift(reg), true, true);
    return evaluate(v, line, g->width, slots, limit, sprite_size(reg), sprite_shift(reg), terminates, false);
}

// ---- drawing, a word at a time (Phase 8) ----
//
// Each side of a line keeps a bitmap of the pixels a sprite has covered — a
// solid pixel, visible or not — and, where legacy colour 0 sprites can cover
// without painting, a second of the pixels one has painted. A sprite collides
// wherever its solid pixels meet the first; it draws only the pixels it newly
// takes in the second, four to a word. That is §10's rule — a pixel belongs to
// the lowest slot that paints it, and collides if two slots cover it — decided
// in words, and any division of the columns still gives the same line.

#define CLAIM_WORDS VDP_CLAIM_WORDS

// A sprite's row as it lands on the line: up to 32 pixels' indices, from its
// left edge, and a bit for each solid one, bit 0 leftmost.
typedef struct sprite_row {
    uint8_t index[32 + 4];
    uint32_t solid;
} sprite_row_t;

// The pattern row a sprite draws on this line: 8 or 16 pixels, as indices in
// words and a solid bit each, before any flip. §10: the pattern index counts 8 x
// 8 patterns at every depth, and a 16 x 16's quadrants N to N + 3 are top left,
// bottom left, top right, bottom right. Pixels are MSB or high nibble first.
static inline __attribute__((always_inline)) void decode_quadrant(const uint8_t *row, unsigned depth, bool pinned,
                                                                  uint8_t group, uint32_t *w0, uint32_t *w1,
                                                                  unsigned *solid) {
    switch (depth) {
    case VDP_DEPTH_1BPP: {
        // Legacy: b3:0 is the index itself (§9). Otherwise value 1 in the
        // group: (group × 2 + 1) & $FF.
        const uint32_t index = (uint32_t)(pinned ? group : (uint8_t)(group << 1 | 1)) * VDP_ONES;
        *w0 = index;
        *w1 = index;
        *solid = vdp_reverse8[row[0]];
        break;
    }
    case VDP_DEPTH_2BPP: {
        const uint32_t v0 = vdp_values2[row[0]], v1 = vdp_values2[row[1]];
        const uint32_t base = (uint32_t)(uint8_t)(group << 2) * VDP_ONES;
        *w0 = base | v0;
        *w1 = base | v1;
        *solid = vdp_byte_bits(vdp_nonzero_bytes(v0)) | vdp_byte_bits(vdp_nonzero_bytes(v1)) << 4;
        break;
    }
    case VDP_DEPTH_4BPP: {
        // (group × 16 + value) & $FF: SPRPAL drops out, and the table is the mapping.
        const uint16_t *pairs = vdp_unpack4[group & 0x0f];
        const uint32_t bytes = vdp_load32(row);
        *w0 = pairs[bytes & 0xff] | (uint32_t)pairs[bytes >> 8 & 0xff] << 16;
        *w1 = pairs[bytes >> 16 & 0xff] | (uint32_t)pairs[bytes >> 24] << 16;
        *solid = vdp_byte_bits(vdp_nonzero_bytes(*w0 & 0x0f0f0f0fu)) |
                 vdp_byte_bits(vdp_nonzero_bytes(*w1 & 0x0f0f0f0fu)) << 4;
        break;
    }
    default:
        *w0 = vdp_load32(row);
        *w1 = vdp_load32(row + 4);
        *solid = vdp_byte_bits(vdp_nonzero_bytes(*w0)) | vdp_byte_bits(vdp_nonzero_bytes(*w1)) << 4;
        break;
    }
}

static inline __attribute__((always_inline)) void sprite_row(const uint8_t *vram, const vdp_sprite_t *s,
                                                             unsigned size, unsigned shift, unsigned depth,
                                                             bool pinned, uint16_t table, uint8_t palette_high,
                                                             sprite_row_t *out) {
    const unsigned row_bytes = 1u << depth, pattern_bytes = 8u << depth;
    const uint8_t group = (uint8_t)(pinned ? (s->attributes & ATTR_COLOUR) : (palette_high | (s->attributes & ATTR_COLOUR)));
    const unsigned half = size == 16 ? s->row >> 3 : 0;
    const uint16_t row_offset = (uint16_t)((s->row & 7) * row_bytes);
    uint32_t w[4] = {0};
    unsigned solid, right;
    decode_quadrant(vram + (uint16_t)(table + (s->pattern + half) * pattern_bytes + row_offset), depth, pinned,
                    group, &w[0], &w[1], &solid);
    if (size == 16) {
        decode_quadrant(vram + (uint16_t)(table + (s->pattern + 2 + half) * pattern_bytes + row_offset), depth,
                        pinned, group, &w[2], &w[3], &right);
        solid |= right << 8;
    }
    // §10: a horizontal flip is the whole sprite's, quadrants included. The
    // legacy submode ignores it (§9).
    if (!pinned && (s->attributes & ATTR_FLIP_X)) {
        if (size == 16) {
            const uint32_t t0 = w[0], t1 = w[1];
            w[0] = vdp_swap32(w[3]);
            w[1] = vdp_swap32(w[2]);
            w[2] = vdp_swap32(t1);
            w[3] = vdp_swap32(t0);
            solid = (unsigned)vdp_reverse8[solid & 0xff] << 8 | vdp_reverse8[solid >> 8];
        } else {
            const uint32_t t0 = w[0];
            w[0] = vdp_swap32(w[1]);
            w[1] = vdp_swap32(t0);
            solid = vdp_reverse8[solid];
        }
    }
    const unsigned words = size >> 2;
    if (shift) {
        // Magnified: every pixel twice, a word of four becoming two.
        for (unsigned i = 0; i < words; i++) {
            const uint32_t x = w[i];
            vdp_store32(out->index + 8 * i, (x & 0xff) * 0x0101u | ((x >> 8 & 0xff) * 0x0101u) << 16);
            vdp_store32(out->index + 8 * i + 4, (x >> 16 & 0xff) * 0x0101u | ((x >> 24) * 0x0101u) << 16);
        }
        out->solid = vdp_spread8[solid & 0xff] | (uint32_t)vdp_spread8[solid >> 8] << 16;
    } else {
        for (unsigned i = 0; i < words; i++) vdp_store32(out->index + 4 * i, w[i]);
        out->solid = solid;
    }
}

// Bits [b, b + n) of a claim bitmap, n <= 32, as one word.
static inline uint32_t window(const uint32_t *map, unsigned at) {
    const unsigned k = at >> 5, b = at & 31;
    return b ? (map[k] >> b | map[k + 1] << (32 - b)) : map[k];
}

static inline void claim(uint32_t *map, unsigned at, uint32_t bits) {
    const unsigned k = at >> 5, b = at & 31;
    map[k] |= bits << b;
    if (b) map[k + 1] |= bits >> (32 - b);
}

// Picture columns [x0, x1) of the line's sprites, drawn straight into a half's
// `picture` where each pixel's level beats the layers' in `levels` (§12).
// Reads only the render side and the list; the collisions found are left in
// the half.
static inline __attribute__((always_inline)) void columns(const vdp_t *v, vdp_half_t *s, int x0, int x1,
                                                          uint8_t *picture, const uint8_t *levels,
                                                          const unsigned depth, const bool pinned,
                                                          const bool detailed) {
    const uint8_t *reg = v->render_reg;
    const uint8_t control = reg[VDP_REG_SPRCTRL];
    const bool collision = (control & VDP_SPRCTRL_COLLISION) != 0;
    const unsigned size = sprite_size(reg), shift = sprite_shift(reg);
    const int span = (int)(size << shift);
    const uint16_t table = (uint16_t)(reg[VDP_REG_SPRPAT] << 11);  // §5: x $800
    const uint8_t palette_high = pinned ? 0 : (uint8_t)((reg[VDP_REG_SPRPAL] & 0x0f) << 4);

    uint32_t covered[CLAIM_WORDS] = {0}, painted[CLAIM_WORDS] = {0}, hit[CLAIM_WORDS];
    if (detailed) {
        memset(hit, 0, sizeof hit);
        memset(s->owners, 0, sizeof s->owners);
    }
    sprite_row_t row;

    for (unsigned i = 0; i < v->sprite_count; i++) {
        const vdp_sprite_t *sprite = &v->sprite[i];
        const int left = sprite->left;
        const int from = left > x0 ? left : x0;
        const int to = left + span < x1 ? left + span : x1;
        if (from >= to) continue;  // counted, and drawn by the other side if anywhere

        sprite_row(v->render_vram, sprite, size, shift, depth, pinned, table, palette_high, &row);
        const unsigned count = (unsigned)(to - from);
        uint32_t solid = row.solid >> (from - left);
        if (count < 32) solid &= (1u << count) - 1;
        if (!solid) continue;

        // Collision is on coverage, before any priority (§10, §12): a sprite
        // hidden behind another, or behind a layer, still collides. The lowest
        // slot on a pixel is paired with each one after it, so every sprite on
        // a pixel two share is named.
        const uint32_t before = window(covered, (unsigned)from);
        claim(covered, (unsigned)from, solid);
        const uint32_t met = solid & before;
        if (met && collision) {
            s->collided = true;
            if (detailed) {
                s->collisions |= UINT64_C(1) << sprite->slot;
                claim(hit, (unsigned)from, met);
            }
        }
        if (detailed) {
            // Which slot first covered which pixels, word by word.
            const uint32_t first = solid & ~before;
            const unsigned k = (unsigned)from >> 5, b = (unsigned)from & 31;
            const uint32_t low = first << b, high = b ? first >> (32 - b) : 0;
            if (low) {
                const unsigned n = s->owners[k]++;
                s->owner_slot[k][n] = sprite->slot;
                s->owner_bits[k][n] = low;
            }
            if (high) {
                const unsigned n = s->owners[k + 1]++;
                s->owner_slot[k + 1][n] = sprite->slot;
                s->owner_bits[k + 1][n] = high;
            }
        }

        // §9: legacy colour 0 is invisible but still collides.
        if (pinned && (sprite->attributes & ATTR_COLOUR) == 0) continue;

        // The lowest slot to paint a pixel owns it (§10), even where a layer
        // then outranks it: a sprite behind does not show through.
        uint32_t fresh;
        if (pinned) {
            fresh = solid & ~window(painted, (unsigned)from);
            claim(painted, (unsigned)from, solid);
        } else {
            fresh = solid & ~before;
        }
        if (!fresh) continue;

        // §12: b6 lifts the sprite above layer 1; legacy sprites stay at level 2.
        const unsigned level = (!pinned && (sprite->attributes & ATTR_PRIORITY)) ? VDP_LEVEL_SPRITE_FRONT : VDP_LEVEL_SPRITE;
        // Only the nibbles holding a pixel it newly takes, four pixels to a word.
        const uint8_t *src = row.index + (from - left);
        uint8_t *out = picture + from;
        const uint8_t *lv = levels + from;
        while (fresh) {
            const unsigned c = (unsigned)__builtin_ctz(fresh) & ~3u;
            const uint32_t mask = vdp_nibble_lsb[(fresh >> c) & 15] & vdp_beaten(vdp_load32(lv + c), level);
            fresh &= ~(15u << c);
            vdp_store32(out + c, (vdp_load32(out + c) & ~mask) | (vdp_load32(src + c) & mask));
        }
    }

    // An owner collided exactly where a later sprite met a pixel it owns.
    if (detailed && s->collided) {
        for (unsigned k = 0; k < CLAIM_WORDS; k++) {
            if (!hit[k]) continue;
            for (unsigned n = 0; n < s->owners[k]; n++) {
                if (s->owner_bits[k][n] & hit[k]) s->collisions |= UINT64_C(1) << s->owner_slot[k][n];
            }
        }
    }
}

static void VDP_HOT(columns_at_depth)(const vdp_t *v, vdp_half_t *s, int x0, int x1, uint8_t *picture,
                                      const uint8_t *levels, unsigned depth, bool pinned, bool detailed) {
    if (pinned) {
        columns(v, s, x0, x1, picture, levels, VDP_DEPTH_1BPP, true, detailed);
        return;
    }
    switch (depth) {
    case VDP_DEPTH_1BPP: columns(v, s, x0, x1, picture, levels, VDP_DEPTH_1BPP, false, detailed); break;
    case VDP_DEPTH_2BPP: columns(v, s, x0, x1, picture, levels, VDP_DEPTH_2BPP, false, detailed); break;
    case VDP_DEPTH_4BPP: columns(v, s, x0, x1, picture, levels, VDP_DEPTH_4BPP, false, detailed); break;
    default: columns(v, s, x0, x1, picture, levels, VDP_DEPTH_8BPP, false, detailed); break;
    }
}

// Clips [x0, x1) to the picture and the list, and draws.
void VDP_HOT(vdp_draw_sprites)(const vdp_t *v, vdp_half_t *h, int x0, int x1, uint8_t *picture, const uint8_t *levels) {
    const uint8_t *reg = v->render_reg;
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(reg, &legacy);
    const int width = g->width;
    if (x0 < 0) x0 = 0;
    if (x1 > width) x1 = width;
    h->collided = false;
    h->collisions = 0;
    if (!v->sprite_count || x1 <= x0) return;

    // §9: the legacy submode pins sprites to 1bpp and ignores SPRPAL.
    const uint8_t control = reg[VDP_REG_SPRCTRL];
    const bool pinned = legacy != VDP_LEGACY_NONE;
    const unsigned depth = pinned ? VDP_DEPTH_1BPP : (control & VDP_SPRCTRL_DEPTH) >> 4;
    // §10: b3 does nothing while b1 is clear.
    const bool detailed = (control & VDP_SPRCTRL_COLLISION) && (control & VDP_SPRCTRL_DETAILED);
    columns_at_depth(v, h, x0, x1, picture, levels, depth, pinned, detailed);
}

// §6, §10, §14: COL, the frame's collision interrupt, and with SPRCTRL b3 the
// map. Every one of them only ever sets, so the order the halves of a line are
// published in, or publishing one twice, changes nothing.
void VDP_HOT(vdp_publish_collisions)(vdp_t *v, uint64_t collisions) {
    v->stat0 |= VDP_STAT0_COL;
    vdp_frame_event(v, VDP_IRQ_COLLISION);
    // Bit s mod 8 of STAT(8 + s/8): the map is the 64 bits little-endian.
    uint64_t map;
    memcpy(&map, v->collision_map, sizeof map);
    map |= collisions;
    memcpy(v->collision_map, &map, sizeof map);
}

// PLAN.md section 3: the picture column the cores divide a row at, on an
// 8-pixel boundary: core 0 builds the columns left of it, core 1 the rest. The
// row's cost-weighted mean column — both layers and the expansion, the same for
// every column, and each sprite at its centre — in one pass and one division.
// The costs are Phase 8's, in cycles on the RP2350 (docs/results/phase-08.md);
// the firmware moves the column after each row toward the core that finished
// first. 0, for one core, on a row with no picture.
int VDP_HOT(vdp_split_choose)(const vdp_t *v) {
    if (v->render_screen_line >= VDP_HEIGHT) return 0;
    const uint8_t *reg = v->render_reg;
    const vdp_geometry_t *g = vdp_geometry(reg, NULL);
    if (vdp_display_line_of(v->render_screen_line, g) >= g->lines || !(reg[VDP_REG_MODE1] & VDP_MODE1_DISP)) return 0;
    const unsigned width = g->width;

    uint32_t per_column = 6;
    if (reg[VDP_REG_L0CTRL] & VDP_LXCTRL_ENABLE) per_column += 11;
    if (reg[VDP_REG_L1CTRL] & VDP_LXCTRL_ENABLE) per_column += 17;
    uint32_t total = per_column * width;
    uint32_t moment = total * (width / 2);
    const uint8_t control = reg[VDP_REG_SPRCTRL];
    const bool detailed = (control & VDP_SPRCTRL_COLLISION) && (control & VDP_SPRCTRL_DETAILED);
    const int span = (int)(sprite_size(reg) << sprite_shift(reg));
    const uint32_t per_sprite = (detailed ? 290 : 200) + 8 * (uint32_t)span;
    for (unsigned i = 0; i < v->sprite_count; i++) {
        int centre = v->sprite[i].left + span / 2;
        if (centre < 0) centre = 0;
        if (centre >= (int)width) centre = (int)width - 1;
        moment += per_sprite * (uint32_t)centre;
        total += per_sprite;
    }
    unsigned split = ((moment / total) + 4) & ~7u;
    if (split < 8) split = 8;
    if (split > width) split = width;
    return (int)split;
}
