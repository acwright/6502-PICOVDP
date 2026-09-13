// Inspection and snapshots — see vdp_debug.h.

#include "vdp_debug.h"

#include "vdp_internal.h"

#include <string.h>

uint8_t vdp_debug_register(const vdp_t *v, unsigned index) {
    return v->reg[vdp_register_home(index)];
}

void vdp_debug_set_register(vdp_t *v, unsigned index, uint8_t value) {
    vdp_register_write(v, index, value);
}

uint8_t vdp_debug_vram(const vdp_t *v, uint16_t address) {
    return v->vram[address];
}

void vdp_debug_set_vram(vdp_t *v, uint16_t address, uint8_t value) {
    vdp_poke(v, address, value);
}

vdp_port_t vdp_debug_port(const vdp_t *v, unsigned pair) {
    return v->port[pair & 1];
}

uint16_t vdp_debug_palette(const vdp_t *v, unsigned entry) {
    return vdp_palette_decode(v->vram, vdp_palette_base(v->reg), entry & 0xff);
}

vdp_debug_mode_t vdp_debug_mode(const vdp_t *v) {
    vdp_legacy_mode_t legacy;
    const vdp_geometry_t *g = vdp_geometry(v->reg, &legacy);
    return (vdp_debug_mode_t){
        .vmode = v->reg[VDP_REG_VMODE] & 0x0f,
        .legacy = (uint8_t)legacy,
        .geometry = (uint8_t)g->id,
        .cols = g->cols,
        .rows = g->rows,
        .cell_width = g->cell_width,
        .width = g->width,
        .lines = g->lines,
        .origin_x = g->origin_x,
        .origin_y = g->origin_y,
        .display = (v->reg[VDP_REG_MODE1] & VDP_MODE1_DISP) != 0,
    };
}

vdp_debug_stats_t vdp_debug_stats(const vdp_t *v) {
    return (vdp_debug_stats_t){.journal_overflows = v->journal_overflows};
}

void vdp_debug_save(const vdp_t *v, vdp_snapshot_t *s, uint8_t *vram) {
    memcpy(s->registers, v->reg, sizeof s->registers);
    memcpy(s->port, v->port, sizeof s->port);
    s->screen_line = v->screen_line;
    memcpy(vram, v->vram, VDP_VRAM_SIZE);
}

void vdp_debug_restore(vdp_t *v, const vdp_snapshot_t *s, const uint8_t *vram) {
    memcpy(v->reg, s->registers, sizeof v->reg);
    // IRQEN b0 is the home of the vblank enable (§14); a snapshot whose two
    // copies disagree is settled in its favour.
    vdp_register_write(v, VDP_REG_IRQEN, v->reg[VDP_REG_IRQEN]);
    memcpy(v->port, s->port, sizeof v->port);
    v->screen_line = s->screen_line % VDP_SCREEN_LINES;
    memcpy(v->vram, vram, VDP_VRAM_SIZE);

    memcpy(v->render_vram, v->vram, sizeof v->render_vram);
    memcpy(v->render_reg, v->reg, sizeof v->render_reg);
    v->journal_count = 0;
    v->dirty_pages = 0;
    vdp_palette_reload(v);
}
