// §7, §14, §15: the built-in font, at reset and by FONT at vertical blank.
//
//   test_font <fonts/cp437-6x8.bin>
//
// The core's bytes are generated from the file at build time; they are checked
// against it here, as test_palette checks the palette against SPEC.md. The
// latch's two halves are looked at inside the struct, which Jest cannot see.

#include <stdio.h>
#include <string.h>

#include "card.h"
#include "test.h"
#include "vdp_internal.h"

enum { L0PAT = 0x12, L1PAT = 0x1a, IRQEN = 0x0a, FONT = 0x30 };

// The reset geometry is Compact: its vertical blank fires as screen line 216 begins.
enum { VBLANK_LINE = 216 };

static uint8_t font[VDP_FONT_BYTES];
static const char *font_path;

static bool holds_font(const uint8_t *vram, uint16_t base) {
    return memcmp(vram + base, font, VDP_FONT_BYTES) == 0;
}

static bool holds(const uint8_t *vram, uint16_t base, uint8_t value) {
    for (unsigned i = 0; i < VDP_FONT_BYTES; i++) {
        if (vram[base + i] != value) return false;
    }
    return true;
}

static void fill(vdp_t *v, uint16_t base, uint8_t value) {
    uint8_t bytes[VDP_FONT_BYTES];
    memset(bytes, value, sizeof bytes);
    store(v, base, bytes, sizeof bytes);
}

// Every line start from `from` to `to`, inclusive, the host's way.
static void lines(vdp_t *v, unsigned from, unsigned to) {
    for (unsigned line = from; line <= to; line++) vdp_line_start(v, (uint16_t)line);
}

// A card mid-frame, with the destination filled and taken by the render side.
static vdp_t *card_with(uint16_t base, uint8_t value) {
    vdp_t *v = new_card();
    lines(v, 0, 9);
    fill(v, base, value);
    vdp_line_start(v, 10);  // 2 KB is past the journal: pages, and an overflow
    v->journal_overflows = 0;
    return v;
}

TEST(the_generated_bytes_are_the_file) {
    FILE *f = fopen(font_path, "rb");
    CHECK(f != NULL);
    if (f == NULL) return;
    CHECK_EQ(VDP_FONT_BYTES, fread(font, 1, sizeof font + 1, f));
    fclose(f);
    CHECK(memcmp(vdp_font_cp437, font, sizeof font) == 0);
    for (unsigned i = 0; i < VDP_FONT_BYTES; i++) CHECK_EQ(0, font[i] & 0x03);
}

// §15: a card that has been made holds the font at $0800, on both sides.
TEST(power_on) {
    vdp_t *v = new_card();
    CHECK(holds_font(v->vram, 0x0800));
    CHECK(holds_font(v->render_vram, 0x0800));
    CHECK_EQ(0, vdp_debug_register(v, FONT));
    CHECK_EQ(0, vdp_debug_register(v, L0PAT));
    CHECK(holds(v->vram, 0x0000, 0x00));   // L0PAT x $800: nothing loaded there
    CHECK(holds(v->vram, 0x1000, 0x00));
    CHECK_EQ(0, vdp_read(v, 1) & 0x80);    // no vertical blank in its wake
    free(v);
}

// §15: RST writes it again, and leaves the rest of VRAM; the bus side at once,
// the render side at the next latch, and neither through the journal.
TEST(rst) {
    vdp_t *v = card_with(0x0800, 0xff);
    uint8_t byte = 0xab;
    store(v, 0x1000, &byte, 1);
    vdp_line_start(v, 11);
    uint32_t tail = v->journal_tail;

    vdp_reset(v, false);
    CHECK(holds_font(v->vram, 0x0800));
    CHECK_EQ(0xab, vdp_debug_vram(v, 0x1000));
    CHECK_EQ(tail, v->journal_tail);       // no journal entries: not the palette's 512 either
    CHECK(holds(v->render_vram, 0x0800, 0xff));

    vdp_line_start(v, 12);
    CHECK(holds_font(v->render_vram, 0x0800));
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    CHECK_EQ(0x0fffu * 0x10001u, v->palette[15]);
    CHECK_EQ(0, vdp_debug_stats(v).journal_overflows);

    // And from power-on again.
    fill(v, 0x0800, 0xff);
    vdp_reset(v, true);
    CHECK(holds_font(v->vram, 0x0800));
    CHECK_EQ(0x00, vdp_debug_vram(v, 0x1000));
    free(v);
}

// §7, §14: nothing before the line start where vertical blank fires; all of it
// there, before F, on the bus side and on the render side from that latch.
TEST(completes_at_the_vertical_blank_line_start) {
    vdp_t *v = card_with(0x1000, 0xff);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    CHECK_EQ(0x01, v->font_pending);

    lines(v, 11, VBLANK_LINE - 1);
    CHECK(holds(v->vram, 0x1000, 0xff));
    CHECK(holds(v->render_vram, 0x1000, 0xff));
    CHECK_EQ(0, vdp_debug_status(v, 0) & 0x80);

    // The latch's bus side: the copy and F together.
    vdp_latch(v, VBLANK_LINE, 0);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK_EQ(0x80, vdp_debug_status(v, 0) & 0x80);
    CHECK_EQ(0, v->font_pending);
    CHECK(holds(v->render_vram, 0x1000, 0xff));
    // Its render side takes the pages from that same latch.
    CHECK(vdp_catch_up(v));
    CHECK(holds_font(v->render_vram, 0x1000));
    vdp_publish(v, NULL, NULL);
    CHECK_EQ(0, vdp_debug_stats(v).journal_overflows);

    // Once: the next vertical blank copies nothing.
    fill(v, 0x1000, 0x5a);
    lines(v, VBLANK_LINE + 1, 261);
    lines(v, 0, VBLANK_LINE);
    CHECK(holds(v->vram, 0x1000, 0x5a));
    free(v);
}

// §14: when vertical blank's interrupt is seen, the copy is complete.
TEST(before_the_interrupt) {
    vdp_t *v = card_with(0x1000, 0xff);
    set_reg(v, IRQEN, 0x01);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    unsigned line = 11;
    while (!vdp_int_asserted(v)) {
        CHECK(holds(v->vram, 0x1000, 0xff));
        vdp_line_start(v, (uint16_t)line++);
    }
    CHECK_EQ(VBLANK_LINE + 1, line);
    CHECK(holds_font(v->vram, 0x1000));
    free(v);
}

// §7: until it lands, reads see the old bytes and writes are overwritten.
TEST(the_old_contents_until_it_lands) {
    vdp_t *v = card_with(0x1000, 0x5a);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    point_at(v, 0, 0x1008, false);
    CHECK_EQ(0x5a, vdp_read(v, 0));
    uint8_t byte = 0x00;
    store(v, 0x1010, &byte, 1);
    CHECK_EQ(0x00, vdp_debug_vram(v, 0x1010));
    lines(v, 11, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->render_vram, 0x1000));
    free(v);
}

// §7: layer 1's table with b7, both together, layer 0's landing first.
TEST(both_destinations) {
    vdp_t *v = card_with(0x1000, 0xff);
    fill(v, 0x2000, 0xff);
    lines(v, 11, 11);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, L1PAT, 0x04);
    set_reg(v, FONT, 0x80);
    CHECK_EQ(0x80, vdp_debug_register(v, FONT));
    set_reg(v, FONT, 0x00);
    CHECK_EQ(0x03, v->font_pending);
    lines(v, 12, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->vram, 0x2000));
    CHECK(holds_font(v->render_vram, 0x2000));

    // Layer 1 alone leaves layer 0's table.
    fill(v, 0x1000, 0xee);
    fill(v, 0x2000, 0xee);
    lines(v, VBLANK_LINE + 1, 261);
    set_reg(v, FONT, 0x80);
    lines(v, 0, VBLANK_LINE);
    CHECK(holds(v->vram, 0x1000, 0xee));
    CHECK(holds_font(v->vram, 0x2000));
    free(v);
}

// §7: the destination is sampled at the write; a second load for it replaces
// the first; a reserved ID is stored and neither loads nor cancels.
TEST(sampled_replaced_and_reserved) {
    vdp_t *v = card_with(0x1000, 0xff);
    fill(v, 0x1800, 0xff);
    fill(v, 0x2000, 0xff);
    lines(v, 11, 11);

    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    set_reg(v, L0PAT, 0x03);        // does not move it
    set_reg(v, FONT, 0x7f);         // reserved: does not cancel it
    CHECK_EQ(0x7f, vdp_debug_register(v, FONT));
    lines(v, 12, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds(v->vram, 0x1800, 0xff));

    fill(v, 0x1000, 0xff);
    lines(v, VBLANK_LINE + 1, 261);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    set_reg(v, L0PAT, 0x04);
    set_reg(v, FONT, 0x00);         // replaces it: $2000, not $1000
    lines(v, 0, VBLANK_LINE);
    CHECK(holds(v->vram, 0x1000, 0xff));
    CHECK(holds(v->vram, 0x1800, 0xff));
    CHECK(holds_font(v->vram, 0x2000));

    lines(v, VBLANK_LINE + 1, 261);
    fill(v, 0x2000, 0xff);
    set_reg(v, FONT, 0x01);         // reserved, alone: nothing
    CHECK_EQ(0, v->font_pending);
    lines(v, 0, VBLANK_LINE);
    CHECK(holds(v->vram, 0x2000, 0xff));
    free(v);
}

// §7: a register write in every other respect: no port's pointer or prefetch.
TEST(ports_untouched) {
    vdp_t *v = new_card();
    set_reg(v, L0PAT, 0x02);
    point_at(v, 0, 0x3000, false);
    point_at(v, 1, 0x1008, true);
    vdp_port_t a = vdp_debug_port(v, 0), b = vdp_debug_port(v, 1);
    set_reg(v, FONT, 0x00);
    lines(v, 30, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(memcmp(&a, &v->port[0], sizeof a) == 0);
    CHECK(memcmp(&b, &v->port[1], sizeof b) == 0);
    free(v);
}

// §7, §11: at $F800 it covers the palette window, and the cache takes it; it
// does not wrap to $0000.
TEST(over_the_palette_window) {
    vdp_t *v = card_with(0x0000, 0xee);
    set_reg(v, L0PAT, 0x1f);
    set_reg(v, FONT, 0x00);
    lines(v, 11, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0xf800));
    CHECK(holds_font(v->render_vram, 0xf800));
    CHECK(holds(v->vram, 0x0000, 0xee));
    uint16_t stored = (uint16_t)(((font[0x41e] & 0x0f) << 8) | font[0x41f]);
    CHECK(stored != 0xfff);
    CHECK_EQ(stored, vdp_debug_palette(v, 15));
    uint32_t bgr = (uint32_t)((stored >> 8) & 0xf) | (stored & 0x0f0u) | ((uint32_t)(stored & 0xf) << 8);
    CHECK_EQ(bgr * 0x10001u, v->palette[15]);
    CHECK_EQ(0, vdp_debug_stats(v).journal_overflows);
    free(v);
}

// §15: a reset cancels a pending load.
TEST(cancelled_by_reset) {
    vdp_t *v = card_with(0x1000, 0xff);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, FONT, 0x00);
    vdp_reset(v, false);
    CHECK_EQ(0, v->font_pending);
    lines(v, 11, VBLANK_LINE);
    CHECK(holds(v->vram, 0x1000, 0xff));
    free(v);
}

// Phase 13: the latch's own part leaves the bus copy's bytes for its steps, of
// VDP_COPY_STEP at most, layer 0's first. The render side has both from the
// latch, and the two copies agree once the steps are done.
static vdp_t *begun_with_both(void) {
    vdp_t *v = card_with(0x1000, 0xff);
    fill(v, 0x2000, 0xff);
    lines(v, 11, 11);
    set_reg(v, L0PAT, 0x02);
    set_reg(v, L1PAT, 0x04);
    set_reg(v, FONT, 0x00);
    set_reg(v, FONT, 0x80);
    lines(v, 12, VBLANK_LINE - 1);
    vdp_latch_begin(v, VBLANK_LINE, 0);
    return v;
}

TEST(copied_after_the_latch_a_step_at_a_time) {
    vdp_t *v = begun_with_both();
    CHECK_EQ(0x80, vdp_debug_status(v, 0) & 0x80);
    CHECK_EQ(0, v->font_pending);
    CHECK_EQ(2, v->copies);
    CHECK_EQ(~UINT64_C(0), v->copy_pending);
    CHECK(holds(v->vram, 0x1000, 0xff));
    CHECK(holds(v->vram, 0x2000, 0xff));

    unsigned steps = 1;
    CHECK(vdp_latch_copy(v));
    CHECK(memcmp(v->vram + 0x1000, font, VDP_COPY_STEP) == 0);
    CHECK_EQ(0xff, v->vram[0x1000 + VDP_COPY_STEP]);
    while (vdp_latch_copy(v)) steps++;
    CHECK_EQ(2 * VDP_FONT_BYTES / VDP_COPY_STEP, steps + 1);
    CHECK_EQ(0, v->copies);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->vram, 0x2000));

    CHECK(vdp_catch_up(v));
    vdp_publish(v, NULL, NULL);
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    free(v);
}

// An access that reaches a chunk not yet copied copies that chunk first, and
// no more: a read finds the font, and a write stays written, after every step,
// on both sides.
TEST(an_access_that_reaches_it_copies_its_chunk) {
    vdp_t *v = begun_with_both();
    CHECK(vdp_latch_copy(v));
    point_at(v, 1, 0x1100, false);      // chunk 4 of layer 0's
    CHECK_EQ(2, v->copies);
    CHECK_EQ(~UINT64_C(0) & ~UINT64_C(0x11), v->copy_pending);
    CHECK(memcmp(v->vram + 0x1100, font + 0x100, VDP_COPY_STEP) == 0);
    CHECK_EQ(0xff, v->vram[0x1100 + VDP_COPY_STEP]);
    CHECK_EQ(font[0x100], vdp_read(v, 2));
    CHECK_EQ(font[0x101], vdp_read(v, 2));
    vdp_copies_finish(v);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->vram, 0x2000));
    free(v);

    v = begun_with_both();
    uint8_t byte = (uint8_t)~font[0x10];
    store(v, 0x2010, &byte, 1);         // chunk 0 of layer 1's
    CHECK_EQ(~UINT64_C(0) & ~(UINT64_C(1) << VDP_COPY_CHUNKS), v->copy_pending);
    while (vdp_latch_copy(v)) {
    }
    CHECK_EQ(0, v->copies);
    CHECK_EQ(byte, vdp_debug_vram(v, 0x2010));
    lines(v, VBLANK_LINE + 1, VBLANK_LINE + 2);
    CHECK_EQ(byte, v->render_vram[0x2010]);
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    free(v);
}

// One elsewhere does not wait for it, and neither does status.
TEST(an_access_elsewhere_does_not) {
    vdp_t *v = begun_with_both();
    CHECK(vdp_latch_copy(v));
    uint8_t byte = 0x42;
    store(v, 0x3000, &byte, 1);
    point_at(v, 1, 0x3000, false);
    CHECK_EQ(0x42, vdp_read(v, 2));
    CHECK_EQ(0x80, vdp_read(v, 1) & 0x80);
    CHECK_EQ(2, v->copies);
    CHECK_EQ(~UINT64_C(0) & ~UINT64_C(1), v->copy_pending);
    vdp_copies_finish(v);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->vram, 0x2000));
    free(v);
}

// A load the latch has begun came before a reset that follows it: RST finishes
// it rather than cancelling it (§15 cancels loads still pending).
TEST(a_reset_after_the_latch_finishes_it) {
    vdp_t *v = begun_with_both();
    vdp_reset(v, false);
    CHECK_EQ(0, v->copies);
    CHECK(holds_font(v->vram, 0x1000));
    CHECK(holds_font(v->vram, 0x2000));
    lines(v, VBLANK_LINE + 1, VBLANK_LINE + 2);
    CHECK(memcmp(v->render_vram, v->vram, sizeof v->vram) == 0);
    free(v);
}

// Video.ts's setRegister is a command too, so a debugger's write loads.
TEST(a_debuggers_write) {
    vdp_t *v = card_with(0x1000, 0xff);
    vdp_debug_set_register(v, L0PAT, 0x02);
    vdp_debug_set_register(v, FONT, 0x00);
    lines(v, 11, VBLANK_LINE);
    CHECK(holds_font(v->vram, 0x1000));
    free(v);
}

// A snapshot carries the pending loads; a restored card lands them at its own
// vertical blank.
TEST(in_a_snapshot) {
    vdp_t *v = card_with(0x2000, 0xff);
    fill(v, 0x2800, 0xff);
    lines(v, 11, 11);
    set_reg(v, L1PAT, 0x04);
    set_reg(v, FONT, 0x80);
    set_reg(v, L1PAT, 0x05);

    vdp_snapshot_t s;
    static uint8_t vram[VDP_VRAM_SIZE];
    vdp_debug_save(v, &s, vram);
    CHECK_EQ(0x02, s.font_pending);
    CHECK_EQ(0x2000, s.font_base[1]);
    vdp_t *w = new_card();
    vdp_debug_restore(w, &s, vram);
    CHECK_EQ(0x02, w->font_pending);
    lines(w, 12, VBLANK_LINE);
    CHECK(holds_font(w->vram, 0x2000));
    CHECK(holds(w->vram, 0x2800, 0xff));
    CHECK(memcmp(w->render_vram, w->vram, sizeof w->vram) == 0);
    free(w);
    free(v);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_font <fonts/cp437-6x8.bin>\n");
        return 2;
    }
    font_path = argv[1];
    RUN(the_generated_bytes_are_the_file);
    RUN(power_on);
    RUN(rst);
    RUN(completes_at_the_vertical_blank_line_start);
    RUN(before_the_interrupt);
    RUN(the_old_contents_until_it_lands);
    RUN(both_destinations);
    RUN(sampled_replaced_and_reserved);
    RUN(ports_untouched);
    RUN(over_the_palette_window);
    RUN(cancelled_by_reset);
    RUN(copied_after_the_latch_a_step_at_a_time);
    RUN(an_access_that_reaches_it_copies_its_chunk);
    RUN(an_access_elsewhere_does_not);
    RUN(a_reset_after_the_latch_finishes_it);
    RUN(a_debuggers_write);
    RUN(in_a_snapshot);
    return TEST_RESULT();
}
