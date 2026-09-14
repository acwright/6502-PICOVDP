// Reset (§15), the latch where the render side catches up with the bus (§3),
// and the status a line publishes.

#include "vdp_internal.h"

#include <string.h>

// §5's reset values; every register not listed resets to $00.
static void reset_registers(uint8_t *reg) {
    memset(reg, 0, VDP_REGISTERS);
    reg[VDP_REG_VINC] = 0x01;      // +1
    reg[VDP_REG_PALBASE] = 0x3f;   // the palette at $FC00
    reg[VDP_REG_L0CTRL] = 0x3c;    // 1bpp, no attribute table, enabled, index 0 opaque
    reg[VDP_REG_L1CTRL] = 0x0c;    // the same, disabled, index 0 transparent
    reg[VDP_REG_SPRCOUNT] = 0x20;  // 32 slots
    reg[VDP_REG_SPRCTRL] = 0x27;   // enabled, collision, $D0 terminator, 4bpp
    reg[VDP_REG_SPRLIMIT] = 0x10;  // 16 a line (§18)
}

void vdp_init(vdp_t *v, uint8_t version) {
    memset(v, 0, sizeof *v);
    v->version = version;
    vdp_reset(v, true);
}

void vdp_reset(vdp_t *v, bool power_on) {
    reset_registers(v->reg);
    vdp_status_reset(v, power_on);
    // Both port pairs: pointer 0, direction read, prefetch 0, flip-flop
    // cleared. STATSEL is in the register file, and 0 already.
    for (unsigned pair = 0; pair < 2; pair++) {
        v->port[pair] = (vdp_port_t){.read_mode = true};
    }

    uint16_t base = vdp_palette_base(v->reg);
    if (power_on) {
        // §15 leaves VRAM undefined; power-on zeroes it, as the emulator's cold
        // start does, so VRAM goldens compare exactly (PLAN.md section 4). Nothing
        // is being built at power-on, so the render side is loaded directly.
        memset(v->vram, 0, sizeof v->vram);
        vdp_palette_install(v->vram, base);
        memcpy(v->render_vram, v->vram, VDP_VRAM_SIZE);
        vdp_render_guard(v);
        memcpy(v->render_reg, v->reg, sizeof v->render_reg);
        v->journal_head = v->journal_tail = 0;
        v->dirty_pages = 0;
        v->latch_head = v->latch_tail = 0;
        vdp_palette_reload(v);
        v->render_screen_line = (uint16_t)((v->screen_line + 1) % VDP_SCREEN_LINES);
        v->render_overflow = 0;
        v->half.collided = false;
        vdp_sprites_evaluate(v);
        v->hblank = false;
        return;
    }

    // RST keeps VRAM but for the palette window, which reset clobbers (§11).
    // The raster runs on, and a line may be building from the render side, so
    // this goes through the bus side like any other write and reaches the render
    // side at the next latch.
    for (unsigned entry = 0; entry < VDP_PALETTE_ENTRIES; entry++) {
        uint16_t rgb = vdp_default_palette[entry];
        vdp_poke(v, (uint16_t)(base + 2 * entry), (uint8_t)((rgb >> 8) & 0x0f));
        vdp_poke(v, (uint16_t)(base + 2 * entry + 1), (uint8_t)rgb);
    }
}

// §3: screen line `screen_line` begins, and the line after it is built from the
// card as it stands now. The render side takes every VRAM write and register
// write since the last latch, so that the build — which the bus interrupts
// throughout on the RP2350 — reads a card that does not change under it. The
// host's latch, whole: the firmware takes the three halves in their own
// contexts.
void vdp_line_start(vdp_t *v, uint16_t screen_line) {
    vdp_latch(v, screen_line, 0);
    while (vdp_catch_up(v)) {
    }
    vdp_publish(v, NULL, NULL);
}

// §3's latch, as the line begins: the line's events (§14), numbered and judged
// with the card as it stands now, and a record of that card for the render side
// — the register file whole, and where the journal's writes end. When the ring
// is full, the render side is VDP_LATCHES lines behind: the newest record takes
// this latch instead, and the line it held is never built.
void VDP_HOT(vdp_latch)(vdp_t *v, uint16_t screen_line, uint32_t tag) {
    v->screen_line = screen_line % VDP_SCREEN_LINES;
    vdp_raster_line_start(v, v->screen_line);

    uint32_t tail = v->latch_tail;
    bool full = tail - v->latch_head == VDP_LATCHES;
    vdp_latch_record_t *r = &v->latch[(full ? tail - 1 : tail) & (VDP_LATCHES - 1)];
    if (full) {
        r->dirty_pages |= v->dirty_pages;
        v->latches_merged++;
    } else {
        r->dirty_pages = v->dirty_pages;
    }
    v->dirty_pages = 0;
    memcpy(r->reg, v->reg, sizeof r->reg);
    r->journal_end = v->journal_tail;
    r->tag = tag;
    r->screen_line = v->screen_line;
    // The record is whole before the render side can see it.
    if (!full) v->latch_tail = tail + 1;
}

// The render side reaches the oldest latch it has not taken. Reads what the
// latch recorded and the journal entries before its end, which no write
// touches again, so the bus may interrupt it anywhere; writes only the render
// side and the two heads. No status: the line's overflow waits for vdp_publish.
bool VDP_HOT(vdp_catch_up)(vdp_t *v) {
    uint32_t head = v->latch_head;
    if (head == v->latch_tail) return false;
    const vdp_latch_record_t *r = &v->latch[head & (VDP_LATCHES - 1)];

    // VRAM, in the order it was written, snooping the palette window as the
    // render side has it placed (§11): the cache takes a write at once.
    uint16_t base = vdp_palette_base(v->render_reg);
    for (uint32_t i = v->journal_head; i != r->journal_end; i++) {
        uint16_t address = v->journal_address[i & (VDP_JOURNAL_ENTRIES - 1)];
        v->render_vram[address] = v->journal_value[i & (VDP_JOURNAL_ENTRIES - 1)];
        if (address < VDP_VRAM_GUARD) v->render_vram[VDP_VRAM_SIZE + address] = v->render_vram[address];
        uint16_t offset = (uint16_t)(address - base);
        if (offset < VDP_PALETTE_BYTES) vdp_palette_cache_entry(v, offset >> 1);
    }
    v->journal_head = r->journal_end;

    // Writes past the journal's end: whole pages, which hold the final bytes of
    // every write to them, journaled or not. Taken from the bus copy as it is
    // now, so a page written again since the latch arrives with that write too;
    // the host catches up at the latch, where now is the latch.
    bool reload = false;
    if (r->dirty_pages) {
        v->journal_overflows++;
        for (unsigned page = 0; page < 64; page++) {
            if (!(r->dirty_pages & (UINT64_C(1) << page))) continue;
            unsigned from = page << VDP_VRAM_PAGE_SHIFT;
            memcpy(v->render_vram + from, v->vram + from, 1u << VDP_VRAM_PAGE_SHIFT);
            if (page == 0) vdp_render_guard(v);
            // The window lies inside one page (vdp_palette_base).
            if (page == (unsigned)(base >> VDP_VRAM_PAGE_SHIFT)) reload = true;
        }
    }

    // The registers, whole. A PALBASE that moved re-reads the window (§11),
    // from the render copy just brought up to date.
    memcpy(v->render_reg, r->reg, sizeof v->render_reg);
    if (reload || vdp_palette_base(v->render_reg) != base) vdp_palette_reload(v);

    // The line built now is the next one: screen line 0 as 261 begins (§3).
    v->render_screen_line = (uint16_t)((r->screen_line + 1) % VDP_SCREEN_LINES);
    v->render_tag = r->tag;

    // Its sprites, from the render side as it now stands.
    v->render_overflow = vdp_sprites_evaluate(v);
    v->latch_head = head + 1;
    return true;
}

// §6, §14: the status a row found. Its overflow, from the evaluation (§10),
// then each half's collisions. Each is published once.
void VDP_HOT(vdp_publish)(vdp_t *v, const vdp_half_t *a, const vdp_half_t *b) {
    if (v->render_overflow) {
        vdp_report_overflow(v, v->render_overflow - 1u);
        v->render_overflow = 0;
    }
    if (a) vdp_publish_collisions(v, a);
    if (b) vdp_publish_collisions(v, b);
}

void vdp_set_hblank(vdp_t *v, bool hblank) {
    v->hblank = hblank;
}
