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

uint8_t vdp_debug_status(const vdp_t *v, unsigned select) {
    return vdp_status_peek(v, select);
}

uint16_t vdp_debug_display_line(const vdp_t *v) {
    return v->display_line;
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
    return (vdp_debug_stats_t){.journal_overflows = v->journal_overflows, .latches_merged = v->latches_merged};
}

void vdp_debug_save(const vdp_t *v, vdp_snapshot_t *s, uint8_t *vram) {
    memcpy(s->registers, v->reg, sizeof s->registers);
    memcpy(s->port, v->port, sizeof s->port);
    s->screen_line = v->screen_line;
    s->display_line = v->display_line;
    s->stat0 = v->stat0;
    s->irq_latch = v->irq_latch;
    s->frame_events = v->frame_events;
    s->overflow_sprite = v->overflow_sprite;
    memcpy(s->collision_map, v->collision_map, sizeof s->collision_map);
    s->font_pending = v->font_pending;
    memcpy(s->font_id, v->font_id, sizeof s->font_id);
    memcpy(s->font_base, v->font_base, sizeof s->font_base);
    memcpy(vram, v->vram, VDP_VRAM_SIZE);
}

void vdp_debug_restore(vdp_t *v, const vdp_snapshot_t *s, const uint8_t *vram) {
    memcpy(v->reg, s->registers, sizeof v->reg);
    v->geometry = vdp_geometry(v->reg, NULL);
    // IRQEN b0 is the home of the vblank enable (§14); a snapshot whose two
    // copies disagree is settled in its favour.
    vdp_register_write(v, VDP_REG_IRQEN, v->reg[VDP_REG_IRQEN]);
    memcpy(v->port, s->port, sizeof v->port);
    v->screen_line = s->screen_line % VDP_SCREEN_LINES;
    v->display_line = s->display_line % VDP_SCREEN_LINES;
    v->stat0 = s->stat0;
    v->irq_latch = s->irq_latch & VDP_IRQ_SOURCES;
    v->frame_events = s->frame_events & VDP_IRQ_SOURCES;
    v->overflow_sprite = s->overflow_sprite & 0x3f;
    memcpy(v->collision_map, s->collision_map, sizeof v->collision_map);
    // Only font $00 at a whole pattern table is a load (vdp_font_command).
    v->font_pending = 0;
    for (unsigned layer = 0; layer < 2; layer++) {
        if (!(s->font_pending & (1u << layer)) || s->font_id[layer] != VDP_FONT_CP437) continue;
        v->font_id[layer] = VDP_FONT_CP437;
        v->font_base[layer] = (uint16_t)(s->font_base[layer] & 0xf800);
        v->font_pending |= (uint8_t)(1u << layer);
    }
    memcpy(v->vram, vram, VDP_VRAM_SIZE);
    v->copies = 0;
    v->copy_pending = 0;

    memcpy(v->render_vram, v->vram, VDP_VRAM_SIZE);
    vdp_render_guard(v);
    memcpy(v->render_reg, v->reg, sizeof v->render_reg);
    v->reg_journal_head = v->reg_journal_tail = 0;
    v->reg_whole = false;
    v->render_screen_line = (uint16_t)((v->screen_line + 1) % VDP_SCREEN_LINES);
    v->journal_head = v->journal_tail = 0;
    v->dirty_pages = 0;
    v->bulk_head = v->bulk_tail = 0;
    v->bulk_pages = 0;
    v->latch_head = v->latch_tail = 0;
    vdp_palette_reload(v);
    // The sprites of the line it was saved building. Their overflow, if any,
    // was reported before the save, and is in stat0 already.
    vdp_sprites_evaluate(v);
    v->render_overflow = 0;
    v->half.collided = false;
}
