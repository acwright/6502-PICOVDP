// The render side: building a line from the latched card, and colouring it.
//
// Reads only render_reg, render_vram, render_screen_line and the palette cache
// (PLAN.md section 3).
//
// Phase 5: the backdrop and border (§3, §11), display off, and both layers
// through the tile engine (tiles.c). The sprites (Phase 6) draw over them.

#include "vdp_internal.h"

#include <string.h>

// §11: the backdrop is palette entry L0PAL x 16 + COLOR b3:0, taken for each
// line as §3 builds it, border included.
static inline uint8_t backdrop(const vdp_t *v) {
    return (uint8_t)(((v->render_reg[VDP_REG_L0PAL] & 0x0f) << 4) | (v->render_reg[VDP_REG_COLOR] & 0x0f));
}

int vdp_split_choose(const vdp_t *v) {
    (void)v;
    return VDP_WIDTH;
}

// §3: the frame row render_screen_line, whole — the picture at its origin and
// the backdrop around it. Screen lines 240-261 are blanking and have no row;
// they build as the backdrop, which nothing shows.
void VDP_HOT(vdp_build_layers)(vdp_t *v, uint8_t *indices) {
    memset(indices, backdrop(v), VDP_WIDTH);
    if (v->render_screen_line >= VDP_HEIGHT) return;

    // The geometry the latch took, which numbers this row's display line: the
    // same one the bus side numbered the line start by (§3).
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(v->render_reg, &legacy);
    uint16_t line = vdp_display_line_of(v->render_screen_line, g);
    if (line >= g->lines) return;  // the border above or below the picture

    // Display off draws the backdrop over the picture (§3).
    if (!(v->render_reg[VDP_REG_MODE1] & VDP_MODE1_DISP)) return;

    // Layer 0, then layer 1 over it (§12). At 1bpp neither carries a priority
    // bit, so levels 1 and 3 are this order and nothing more.
    uint8_t *picture = indices + g->origin_x;
    if (v->render_reg[VDP_REG_L0CTRL] & VDP_LXCTRL_ENABLE) vdp_draw_layer(v, 0, line, g, legacy, picture);
    if (v->render_reg[VDP_REG_L1CTRL] & VDP_LXCTRL_ENABLE) vdp_draw_layer(v, 1, line, g, VDP_LEGACY_NONE, picture);
}

void vdp_build_sprites(const vdp_t *v, vdp_sprline_t *s, int x0, int x1) {
    (void)v;
    s->x0 = x0;
    s->x1 = x1;
}

void vdp_draw_sprites(vdp_t *v, uint8_t *indices, int x0, int x1) {
    (void)v;
    (void)indices;
    (void)x0;
    (void)x1;
}

void vdp_merge_sprites(vdp_t *v, uint8_t *indices, const vdp_sprline_t *s) {
    (void)v;
    (void)indices;
    (void)s;
}

void vdp_build_line(vdp_t *v, uint8_t *indices) {
    vdp_build_layers(v, indices);
}

// §3: each index to its colour, twice across. Each cache entry holds the pair.
void VDP_HOT(vdp_expand_line)(const vdp_t *v, const uint8_t *indices, uint16_t *rgb) {
    for (unsigned x = 0; x < VDP_WIDTH; x++) {
        uint32_t pair = v->palette[indices[x]];
        rgb[2 * x] = (uint16_t)pair;
        rgb[2 * x + 1] = (uint16_t)(pair >> 16);
    }
}
