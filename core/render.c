// The render side: building a line from the latched card, and colouring it.
//
// Reads only render_reg, render_vram and the palette cache (PLAN.md section 3).
//
// Phase 3: every line is built as display off builds it (§3) — the backdrop,
// across the whole frame row. The tile engine (Phase 5) and the sprites (Phase
// 6) draw over it.

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

void VDP_HOT(vdp_build_layers)(vdp_t *v, uint8_t *indices) {
    memset(indices, backdrop(v), VDP_WIDTH);
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
