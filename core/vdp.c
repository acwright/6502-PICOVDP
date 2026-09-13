// 6502-PICOVDP core. Phase 2: empty — see vdp.h.
//
// Every function is here so the interface links, and none implements SPEC.md
// yet. Reads return 0, lines build as index 0, and /INT is never asserted. The
// Jest run and the fuzzer are expected to fail against this.

#include "vdp.h"

#include <string.h>

void vdp_reset(vdp_t *v, bool power_on) {
    (void)power_on;
    v->hblank = false;
}

uint8_t vdp_read(vdp_t *v, unsigned port) {
    (void)v;
    (void)port;
    return 0;
}

void vdp_write(vdp_t *v, unsigned port, uint8_t value) {
    (void)v;
    (void)port;
    (void)value;
}

void vdp_line_start(vdp_t *v, uint16_t screen_line) {
    v->screen_line = screen_line % VDP_SCREEN_LINES;
}

void vdp_set_hblank(vdp_t *v, bool hblank) {
    v->hblank = hblank;
}

int vdp_split_choose(const vdp_t *v) {
    (void)v;
    return VDP_WIDTH;
}

void vdp_build_layers(vdp_t *v, uint8_t *indices) {
    (void)v;
    memset(indices, 0, VDP_WIDTH);
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

void vdp_expand_line(const vdp_t *v, const uint8_t *indices, uint16_t *rgb) {
    (void)v;
    (void)indices;
    memset(rgb, 0, 2 * VDP_WIDTH * sizeof *rgb);
}

bool vdp_int_asserted(const vdp_t *v) {
    (void)v;
    return false;
}
