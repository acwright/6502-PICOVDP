// The render side: building a line from the latched card, and colouring it.
//
// Reads only render_reg, render_vram, render_screen_line, the palette cache and
// the sprites the latch evaluated (PLAN.md section 3).
//
// Phase 5: the backdrop and border (§3, §11), display off, and both layers
// through the tile engine (tiles.c).
// Phase 6: the levels the layers leave for the sprites (sprites.c), and the
// whole line built the way the two cores divide it.
// Phase 7: levels kept for the layers' own contest, where layer 0 can carry b6.
// Phase 8: a row in two halves, one a core, each with a line of its own.

#include "vdp_internal.h"

#include <string.h>

// §11: the backdrop is palette entry L0PAL x 16 + COLOR b3:0, taken for each
// line as §3 builds it, border included.
static inline uint8_t backdrop(const vdp_t *v) {
    return (uint8_t)(((v->render_reg[VDP_REG_L0PAL] & 0x0f) << 4) | (v->render_reg[VDP_REG_COLOR] & 0x0f));
}

// §3: picture columns [x0, x1) of the frame row render_screen_line, into a
// half's line — its picture at its origin, the backdrop around it — with no
// status written. Screen lines 240-261 are blanking and have no row; they build
// as the backdrop, which nothing shows. Either core may build a half, at once:
// both read only the render side.
void VDP_HOT(vdp_build_half)(const vdp_t *v, vdp_half_t *h, int x0, int x1) {
    const uint8_t back = backdrop(v);
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(v->render_reg, &legacy);
    const int width = g->width;
    if (x0 < 0) x0 = 0;
    if (x1 > width) x1 = width;
    if (x1 < x0) x1 = x0;
    h->x0 = (int16_t)x0;
    h->x1 = (int16_t)x1;
    h->collided = false;
    h->collisions = 0;
    // The backdrop over the half's own frame columns; the rest of its line is
    // not its, and is never read.
    const unsigned from = x0 <= 0 ? 0 : g->origin_x + (unsigned)x0;
    const unsigned to = x1 >= width ? VDP_WIDTH : g->origin_x + (unsigned)x1;
    if (x0 == x1) return;
    memset(h->line + VDP_LINE_SLACK + from, back, to - from);
    if (v->render_screen_line >= VDP_HEIGHT) return;

    // The geometry the latch took, which numbers this row's display line: the
    // same one the bus side numbered the line start by (§3).
    uint16_t line = vdp_display_line_of(v->render_screen_line, g);
    if (line >= g->lines) return;  // the border above or below the picture

    // Display off draws the backdrop over the picture (§3).
    if (!(v->render_reg[VDP_REG_MODE1] & VDP_MODE1_DISP)) return;

    // Layer 0, then layer 1 over it (§12), each pixel at its level. Levels are
    // kept only on a line that needs them: one with sprites, which are drawn
    // against them, or one where a layer 0 cell's b6 can lift it over layer 1.
    // Anywhere else layer 1 simply draws over layer 0, which is levels 1 and 3.
    uint8_t *picture = h->line + VDP_LINE_SLACK + g->origin_x;
    bool layer1 = (v->render_reg[VDP_REG_L1CTRL] & VDP_LXCTRL_ENABLE) != 0;
    uint8_t *levels = NULL;
    if (v->sprite_count || (layer1 && vdp_layer0_has_priority(v->render_reg, legacy))) {
        // Levels outside [x0, x1) are left from rows before: 0-6, as the word
        // comparisons need, and never judged for a pixel of this half.
        levels = h->level + VDP_LINE_SLACK + g->origin_x;
        memset(levels + x0, VDP_LEVEL_BACKDROP, (size_t)(x1 - x0));
    }
    if (v->render_reg[VDP_REG_L0CTRL] & VDP_LXCTRL_ENABLE) {
        vdp_draw_layer(v, 0, line, g, legacy, picture, levels, x0, x1);
    }
    if (layer1) {
        vdp_draw_layer(v, 1, line, g, VDP_LEGACY_NONE, picture, levels, x0, x1);
    }
    // Cells are drawn whole: the border either side of the picture again,
    // where it is the half's.
    if (x0 == 0) memset(picture - VDP_LINE_SLACK, back, VDP_LINE_SLACK);
    if (x1 == width) memset(picture + width, back, VDP_LINE_SLACK);

    if (v->sprite_count) vdp_draw_sprites(v, h, x0, x1, picture, levels);
}

// A half's frame columns (vdp.h).
static void frame_columns(const vdp_t *v, const vdp_half_t *h, unsigned *from, unsigned *to) {
    const vdp_geometry_t *g = vdp_geometry(v->render_reg, NULL);
    if (h->x1 <= h->x0) {
        *from = *to = 0;
        return;
    }
    *from = h->x0 <= 0 ? 0 : g->origin_x + (unsigned)h->x0;
    *to = h->x1 >= g->width ? VDP_WIDTH : g->origin_x + (unsigned)h->x1;
}

void vdp_copy_half(const vdp_t *v, const vdp_half_t *h, uint8_t *indices) {
    unsigned from, to;
    frame_columns(v, h, &from, &to);
    memcpy(indices + from, h->line + VDP_LINE_SLACK + from, to - from);
}

// A whole line on one thread, divided as the firmware divides it between its
// two cores (PLAN.md section 3): the half left of the split, the half right of
// it, then the status. Any split gives the same row and the same status
// (tests/unit/test_sprites, test_render).
void vdp_build_line(vdp_t *v, uint8_t *indices) {
    static vdp_half_t core0_half;  // the host's: one thread
    const int split = vdp_split_choose(v);
    vdp_build_half(v, &core0_half, 0, split);
    vdp_build_half(v, &v->half, split, VDP_WIDTH);
    vdp_publish(v, &core0_half, &v->half);
    vdp_copy_half(v, &core0_half, indices);
    vdp_copy_half(v, &v->half, indices);
}

// §3: frame columns [x0, x1) to their colours, twice across. Each cache entry
// holds the pair, stored as one word.
static void VDP_HOT(expand)(const vdp_t *v, const uint8_t *indices, uint16_t *rgb, unsigned x0, unsigned x1) {
    const uint32_t *palette = v->palette;
    uint8_t *out = (uint8_t *)rgb + 4 * x0;
    unsigned x = x0;
    for (; x + 4 <= x1; x += 4, out += 16) {
        const uint32_t p0 = palette[indices[x]], p1 = palette[indices[x + 1]];
        const uint32_t p2 = palette[indices[x + 2]], p3 = palette[indices[x + 3]];
        memcpy(out, &p0, 4);
        memcpy(out + 4, &p1, 4);
        memcpy(out + 8, &p2, 4);
        memcpy(out + 12, &p3, 4);
    }
    for (; x < x1; x++, out += 4) memcpy(out, &palette[indices[x]], 4);
}

void VDP_HOT(vdp_expand_line)(const vdp_t *v, const uint8_t *indices, uint16_t *rgb) {
    expand(v, indices, rgb, 0, VDP_WIDTH);
}

// A half's frame columns, into the words of one buffer the other half's
// columns do not touch.
void VDP_HOT(vdp_expand_half)(const vdp_t *v, const vdp_half_t *h, uint16_t *rgb) {
    unsigned from, to;
    frame_columns(v, h, &from, &to);
    expand(v, h->line + VDP_LINE_SLACK, rgb, from, to);
}
