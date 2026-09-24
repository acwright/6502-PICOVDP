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
    vdp_palette_install(v->reset_palette);
    vdp_reset(v, true);
}

void vdp_reset(vdp_t *v, bool power_on) {
    // A FONT load a latch has begun is part of what came before the reset (§7).
    vdp_copies_finish(v);
    reset_registers(v->reg);
    v->geometry = vdp_geometry(v->reg, NULL);
    v->reg_whole = true;  // the next latch copies the file: no journal entry each
    vdp_status_reset(v, power_on);
    // Both port pairs: pointer 0, direction read, prefetch 0, flip-flop
    // cleared. STATSEL is in the register file, and 0 already.
    for (unsigned pair = 0; pair < 2; pair++) {
        v->port[pair] = (vdp_port_t){.read_mode = true};
    }

    // §7: no load waiting for vertical blank survives a reset.
    v->font_pending = 0;

    uint16_t base = vdp_palette_base(v->reg);
    if (power_on) {
        // §15 leaves VRAM undefined but for the palette and the font; power-on
        // zeroes the rest, as the emulator's cold start does, so VRAM goldens
        // compare exactly (PLAN.md section 4). Nothing is being built at
        // power-on, so the render side is loaded directly.
        memset(v->vram, 0, sizeof v->vram);
        memcpy(v->vram + base, v->reset_palette, VDP_PALETTE_BYTES);
        memcpy(v->vram + VDP_FONT_RESET_BASE, vdp_font_cp437, VDP_FONT_BYTES);
        memcpy(v->render_vram, v->vram, VDP_VRAM_SIZE);
        vdp_render_guard(v);
        memcpy(v->render_reg, v->reg, sizeof v->render_reg);
        v->reg_journal_head = v->reg_journal_tail = 0;
        v->reg_whole = false;
        v->journal_head = v->journal_tail = 0;
        v->dirty_pages = 0;
        v->bulk_head = v->bulk_tail = 0;
        v->bulk_pages = 0;
        v->latch_head = v->latch_tail = 0;
        vdp_palette_reload(v);
        v->render_screen_line = (uint16_t)((v->screen_line + 1) % VDP_SCREEN_LINES);
        v->render_overflow = 0;
        v->half.collided = false;
        vdp_sprites_evaluate(v);
        v->hblank = false;
        return;
    }

    // RST keeps VRAM but for the palette window and the font, which reset
    // clobbers (§11, §15), in that order. The raster runs on, and a line may be
    // building from the render side, so both go to the bus side and reach the
    // render side at the next latch: 2.5 KB, as two bulk writes.
    vdp_bulk_write(v, base, v->reset_palette, VDP_PALETTE_BYTES);
    vdp_bulk_write(v, VDP_FONT_RESET_BASE, vdp_font_cp437, VDP_FONT_BYTES);
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
// — where the journals' writes end, VRAM's and the registers'. When the ring
// is full, the render side is VDP_LATCHES lines behind: the newest record takes
// this latch instead, and the line it held is never built.
void vdp_latch(vdp_t *v, uint16_t screen_line, uint32_t tag) {
    vdp_latch_begin(v, screen_line, tag);
    vdp_copies_finish(v);
}

// The latch less the bus copy's part of a FONT load, which vdp_latch_copy
// moves after it (Phase 13).
void vdp_latch_begin(vdp_t *v, uint16_t screen_line, uint32_t tag) {
    vdp_latch_take_t t;
    vdp_latch_take(v, screen_line, &t);
    vdp_latch_record(v, &t, tag);
}

// The latch's one moment: the line's events, judged with the card as it stands
// now, and where the journals end — the writes before the latch, which its
// line is built from (§3). On the RP2350 no access interleaves with it, and it
// is short: an access waits for it (Phase 13). The register file is copied only
// when its journal cannot say, which is after a reset or an overflow, or when
// a full ring merges into a record that holds a whole copy already, which
// must be this latch's. A FONT load landing here is queued for the bus copy,
// where an access that reaches it first finishes it.
void VDP_BUS(vdp_latch_take)(vdp_t *v, uint16_t screen_line, vdp_latch_take_t *t) {
    v->screen_line = screen_line < VDP_SCREEN_LINES ? screen_line : screen_line % VDP_SCREEN_LINES;
    const uint8_t fonts = vdp_raster_line_start(v, v->screen_line);
    t->screen_line = v->screen_line;
    t->reg_end = v->reg_journal_tail;
    t->journal_end = v->journal_tail;
    t->bulk_end = v->bulk_tail;
    t->pages = v->dirty_pages | v->bulk_pages;
    t->overflowed = v->dirty_pages != 0;
    v->dirty_pages = 0;
    v->bulk_pages = 0;
    const uint32_t tail = v->latch_tail;
    t->reg_whole = v->reg_whole ||
                   (tail - v->latch_head == VDP_LATCHES && v->latch[(tail - 1) & (VDP_LATCHES - 1)].reg_whole);
    if (t->reg_whole) memcpy(v->reg_taken, v->reg, sizeof v->reg_taken);
    v->reg_whole = false;
    t->fonts = fonts;
    t->font_base[0] = v->font_base[0];
    t->font_base[1] = v->font_base[1];
    // A frame's copies are long done by the next vertical blank; if not, done
    // now, before this one's are queued behind them.
    if (fonts && v->copy_pending) vdp_copies_finish(v);
    for (unsigned layer = 0; layer < 2; layer++) {
        if (!(fonts & (1u << layer))) continue;
        // Font $00 is the only one there is; vdp_font_command records no other.
        const unsigned i = v->copies++;
        v->copy[i] = (vdp_bulk_t){.bytes = vdp_font_cp437, .address = v->font_base[layer], .length = VDP_FONT_BYTES};
        v->copy_pending |= UINT64_C(0xffffffff) << (i * VDP_COPY_CHUNKS);
    }
}

// The record, from what the latch took. It is the latch's own until the ring's
// tail moves, so accesses may come between: nothing here reads what they write.
// When the ring is full, the render side is VDP_LATCHES lines behind: the
// newest record takes this latch instead, and the line it held is never built.
void VDP_HOT(vdp_latch_record)(vdp_t *v, const vdp_latch_take_t *t, uint32_t tag) {
    uint32_t tail = v->latch_tail;
    bool full = tail - v->latch_head == VDP_LATCHES;
    vdp_latch_record_t *r = &v->latch[(full ? tail - 1 : tail) & (VDP_LATCHES - 1)];
    r->bulk_end = t->bulk_end;
    if (full) {
        r->dirty_pages |= t->pages;
        r->overflowed |= t->overflowed;
        v->latches_merged++;
    } else {
        r->dirty_pages = t->pages;
        r->overflowed = t->overflowed;
    }
    // The registers by where their journal ends, or whole.
    if (t->reg_whole) memcpy(r->reg, v->reg_taken, sizeof r->reg);
    if (t->reg_whole || !full) r->reg_whole = t->reg_whole;
    r->reg_end = t->reg_end;
    r->journal_end = t->journal_end;
    r->tag = tag;
    r->screen_line = t->screen_line;
    // The fonts landing now, at their place among the writes: a merged record
    // may hold an earlier latch's, and replays past them. One that already
    // holds a vertical blank's — the render side a frame behind — takes this
    // one's as pages, from the bus copy as the catch-up finds it.
    if (!full) r->fonts = 0;
    if (t->fonts && full && r->fonts) {
        for (unsigned layer = 0; layer < 2; layer++) {
            if (t->fonts & (1u << layer)) r->dirty_pages |= UINT64_C(0x3) << (t->font_base[layer] >> VDP_VRAM_PAGE_SHIFT);
        }
    } else if (t->fonts) {
        r->fonts = t->fonts;
        r->font_at = t->journal_end;
        r->font_base[0] = t->font_base[0];
        r->font_base[1] = t->font_base[1];
    }
    // The record is whole before the render side can see it.
    if (!full) v->latch_tail = tail + 1;
}

// A bulk write reaches the render copy, and the cache the entries of its
// window it covers (§11).
static void VDP_HOT(render_bulk)(vdp_t *v, const vdp_bulk_t *b, uint16_t base) {
    memcpy(v->render_vram + b->address, b->bytes, b->length);
    if (b->address < VDP_VRAM_GUARD) vdp_render_guard(v);
    unsigned from = b->address > base ? b->address : base;
    unsigned to = b->address + b->length < base + VDP_PALETTE_BYTES ? b->address + b->length : base + VDP_PALETTE_BYTES;
    for (unsigned address = from & ~1u; address < to; address += 2) vdp_palette_cache_entry(v, (address - base) >> 1);
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
    // Bulk writes are taken at their place in that order.
    uint16_t base = vdp_palette_base(v->render_reg);
    uint32_t bulk = v->bulk_head;
    for (uint32_t i = v->journal_head;; i++) {
        for (; bulk != r->bulk_end && v->bulk[bulk & (VDP_BULK_ENTRIES - 1)].at == i; bulk++) {
            render_bulk(v, &v->bulk[bulk & (VDP_BULK_ENTRIES - 1)], base);
        }
        // FONT loads at their latch, layer 0's first (§7).
        if (r->fonts && i == r->font_at) {
            for (unsigned layer = 0; layer < 2; layer++) {
                if (!(r->fonts & (1u << layer))) continue;
                const vdp_bulk_t font = {.bytes = vdp_font_cp437, .address = r->font_base[layer], .length = VDP_FONT_BYTES};
                render_bulk(v, &font, base);
            }
        }
        if (i == r->journal_end) break;
        uint16_t address = v->journal_address[i & (VDP_JOURNAL_ENTRIES - 1)];
        v->render_vram[address] = v->journal_value[i & (VDP_JOURNAL_ENTRIES - 1)];
        if (address < VDP_VRAM_GUARD) v->render_vram[VDP_VRAM_SIZE + address] = v->render_vram[address];
        uint16_t offset = (uint16_t)(address - base);
        if (offset < VDP_PALETTE_BYTES) vdp_palette_cache_entry(v, offset >> 1);
    }
    v->journal_head = r->journal_end;
    v->bulk_head = bulk;

    // Writes past the journal's end, and bulk writes: whole pages, which hold
    // the final bytes of every write to them, journaled or not. Taken from the
    // bus copy as it is now, so a page written again since the latch arrives
    // with that write too; the host catches up at the latch, where now is the
    // latch. Only a full journal counts as an overflow.
    bool reload = false;
    if (r->overflowed) v->journal_overflows++;
    if (r->dirty_pages) {
        for (unsigned page = 0; page < 64; page++) {
            if (!(r->dirty_pages & (UINT64_C(1) << page))) continue;
            unsigned from = page << VDP_VRAM_PAGE_SHIFT;
            memcpy(v->render_vram + from, v->vram + from, 1u << VDP_VRAM_PAGE_SHIFT);
            if (page == 0) vdp_render_guard(v);
            // The window lies inside one page (vdp_palette_base).
            if (page == (unsigned)(base >> VDP_VRAM_PAGE_SHIFT)) reload = true;
        }
    }

    // The registers: their journal's writes, in order, or the file whole. A
    // PALBASE that moved re-reads the window (§11), from the render copy just
    // brought up to date.
    if (r->reg_whole) {
        memcpy(v->render_reg, r->reg, sizeof v->render_reg);
    } else {
        for (uint32_t i = v->reg_journal_head; i != r->reg_end; i++) {
            v->render_reg[v->reg_journal_index[i & (VDP_REGISTER_JOURNAL - 1)]] =
                v->reg_journal_value[i & (VDP_REGISTER_JOURNAL - 1)];
        }
    }
    v->reg_journal_head = r->reg_end;
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
    const bool in_a = a && a->collided, in_b = b && b->collided;
    if (in_a || in_b) vdp_publish_collisions(v, (in_a ? a->collisions : 0) | (in_b ? b->collisions : 0));
}

// Whether either port's answer moved with it: only a port whose STATSEL names
// STAT3 reads the bit, so the bus need not restage for it otherwise.
bool VDP_HOT(vdp_set_hblank)(vdp_t *v, bool hblank) {
    if (v->hblank == hblank) return false;
    v->hblank = hblank;
    return (v->reg[VDP_REG_STATSEL_A] & VDP_STATSEL_MASK) == 3 || (v->reg[VDP_REG_STATSEL_B] & VDP_STATSEL_MASK) == 3;
}
