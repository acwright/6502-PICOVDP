// §3's line numbering, §6's status registers and §14's interrupts, at the core's
// own interface: the platform names each screen line as it begins, and nothing
// here counts time.
//
// Overflow and collision have no source until Phase 6's sprites, so their
// once-a-frame guards are driven through vdp_frame_event, the call the sprites
// will make, and their flags are set in vdp_t directly.

#include <string.h>

#include "card.h"
#include "test.h"
#include "vdp_internal.h"

enum { VMODE = 0x0d, IRQEN = 0x0a, IRQLINE = 0x0b, STATSEL_B = 0x0e, STATSEL_A = 0x0f };

// Begin screen lines until `screen` has begun, wrapping through the frame.
static void run_to(vdp_t *v, uint16_t screen) {
    do {
        vdp_line_start(v, (uint16_t)((v->screen_line + 1) % VDP_SCREEN_LINES));
    } while (v->screen_line != screen);
}

// A status register through a port, as a program reads it.
static uint8_t status(vdp_t *v, unsigned pair, unsigned select) {
    set_reg(v, pair ? STATSEL_B : STATSEL_A, (uint8_t)select);
    return vdp_read(v, 2 * pair + 1);
}

// Display on, in a 192-line geometry by default; VMODE picks another.
static vdp_t *card_in(uint8_t vmode) {
    vdp_t *v = new_card();
    set_reg(v, 0x01, 0x40);
    set_reg(v, VMODE, vmode);
    return v;
}

TEST(display_line_is_the_screen_line_less_the_top_border) {
    static const struct { uint8_t vmode; uint16_t top, lines; } modes[] = {
        {0x0, 24, 192}, {0x1, 24, 192}, {0x2, 24, 192}, {0x3, 0, 240}, {0x4, 0, 240}, {0xf, 24, 192},
    };
    for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        vdp_t *v = card_in(modes[m].vmode);
        set_reg(v, STATSEL_A, 2);
        set_reg(v, STATSEL_B, 3);
        for (unsigned n = 0; n < 2 * VDP_SCREEN_LINES; n++) {
            uint16_t screen = (uint16_t)(n % VDP_SCREEN_LINES);
            vdp_line_start(v, screen);
            uint16_t expected = (uint16_t)((screen + VDP_SCREEN_LINES - modes[m].top) % VDP_SCREEN_LINES);
            CHECK_EQ(expected, vdp_debug_display_line(v));
            CHECK_EQ(expected & 0xff, vdp_read(v, 1));  // 256-261 alias to 0-5
            CHECK_EQ(expected >= modes[m].lines ? 1 : 0, vdp_read(v, 3) & 0x01);
        }
        free(v);
    }
}

TEST(hblank_is_the_platforms) {
    vdp_t *v = new_card();
    set_reg(v, STATSEL_A, 3);
    vdp_set_hblank(v, true);
    CHECK_EQ(0x02, vdp_read(v, 1) & 0x02);
    CHECK_EQ(0x02, vdp_debug_status(v, 3) & 0x02);
    vdp_set_hblank(v, false);
    CHECK_EQ(0x00, vdp_read(v, 1) & 0x02);
    free(v);
}

TEST(a_change_of_height_renumbers_at_the_next_line_start) {
    vdp_t *v = card_in(0x0);
    run_to(v, 124);
    CHECK_EQ(100, vdp_debug_display_line(v));

    set_reg(v, VMODE, 0x03);  // Graphics: no top border
    CHECK_EQ(100, vdp_debug_display_line(v));  // not at the write
    set_reg(v, STATSEL_A, 2);
    CHECK_EQ(100, vdp_read(v, 1));
    // STAT3 b0 judges the old number against the new height at once.
    CHECK_EQ(0, vdp_debug_status(v, 3) & 0x01);
    run_to(v, 125);
    CHECK_EQ(125, vdp_debug_display_line(v));  // at the next line start, 24 on

    set_reg(v, VMODE, 0x01);  // Text: back to 192
    run_to(v, 126);
    CHECK_EQ(102, vdp_debug_display_line(v));
    free(v);
}

TEST(vertical_blank_at_the_end_of_the_picture) {
    static const struct { uint8_t vmode; uint16_t end; } modes[] = {{0x0, 216}, {0x2, 216}, {0x3, 240}, {0x4, 240}};
    for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        vdp_t *v = card_in(modes[m].vmode);
        set_reg(v, IRQEN, 0x01);
        run_to(v, (uint16_t)(modes[m].end - 1));
        CHECK_EQ(0, v->stat0);
        CHECK(!vdp_int_asserted(v));
        run_to(v, modes[m].end);
        CHECK_EQ(0x80, v->stat0);
        CHECK_EQ(0x01, vdp_debug_status(v, 1));
        CHECK(vdp_int_asserted(v));
        free(v);
    }
}

TEST(f_sets_whatever_irqen_and_the_display_say) {
    vdp_t *v = new_card();  // display off, nothing enabled
    run_to(v, 216);
    CHECK_EQ(0x80, v->stat0);
    CHECK_EQ(0, v->irq_latch);  // no trace in STAT1 of a source not enabled
    CHECK(!vdp_int_asserted(v));
    set_reg(v, IRQEN, 0x01);   // enabling it later raises nothing
    CHECK(!vdp_int_asserted(v));
    free(v);
}

TEST(vertical_blank_once_a_frame_across_changes_of_height) {
    // Shrinking past the new end raises it as the next line begins.
    vdp_t *v = card_in(0x4);
    set_reg(v, IRQEN, 0x01);
    run_to(v, 220);
    CHECK_EQ(0, v->stat0);
    set_reg(v, VMODE, 0x02);
    run_to(v, 221);
    CHECK_EQ(197, vdp_debug_display_line(v));
    CHECK_EQ(0x80, v->stat0);
    CHECK_EQ(0x01, status(v, 1, 1));
    run_to(v, 261);
    CHECK_EQ(0, v->irq_latch);  // once in that frame
    run_to(v, 216);
    CHECK_EQ(0x01, v->irq_latch);  // and once in the next, at its own end
    free(v);

    // Growing after it has fired does not raise it again.
    v = card_in(0x0);
    set_reg(v, IRQEN, 0x01);
    run_to(v, 217);
    CHECK_EQ(0x01, status(v, 1, 1));
    CHECK_EQ(0x80, status(v, 0, 0));
    set_reg(v, VMODE, 0x03);
    run_to(v, 261);
    CHECK_EQ(0, v->irq_latch);
    CHECK_EQ(0, v->stat0);
    run_to(v, 239);
    CHECK_EQ(0, v->irq_latch);
    run_to(v, 240);
    CHECK_EQ(0x01, v->irq_latch);  // the next frame's, at 240
    free(v);

    // Growing across the start of a frame skips display line 0; the picture
    // still ends once.
    v = card_in(0x0);
    set_reg(v, IRQEN, 0x01);
    run_to(v, 216);
    CHECK_EQ(0x01, status(v, 1, 1));
    run_to(v, 5);
    set_reg(v, VMODE, 0x03);
    run_to(v, 6);
    CHECK_EQ(6, vdp_debug_display_line(v));
    run_to(v, 239);
    CHECK_EQ(0, v->irq_latch);
    run_to(v, 240);
    CHECK_EQ(0x01, v->irq_latch);
    free(v);
}

TEST(scanline_compare_on_every_match) {
    vdp_t *v = card_in(0x0);
    set_reg(v, IRQLINE, 100);
    set_reg(v, IRQEN, 0x02);
    run_to(v, 123);  // display line 99
    CHECK(!vdp_int_asserted(v));
    run_to(v, 124);  // 100
    CHECK(vdp_int_asserted(v));
    CHECK_EQ(100, status(v, 1, 2));  // STAT2 reads IRQLINE in the handler
    CHECK_EQ(0x02, status(v, 1, 1));
    CHECK(!vdp_int_asserted(v));

    // A handler that reprograms IRQLINE gets another in the same frame.
    set_reg(v, IRQLINE, 108);
    run_to(v, 132);
    CHECK_EQ(0x02, v->irq_latch);
    CHECK_EQ(0x02, status(v, 1, 1));

    // A line a change of height repeats compares again.
    set_reg(v, VMODE, 0x03);
    set_reg(v, IRQLINE, 140);
    run_to(v, 140);
    CHECK_EQ(0x02, status(v, 1, 1));
    set_reg(v, VMODE, 0x01);  // the next line is numbered 24 less
    run_to(v, 164);
    CHECK_EQ(140, vdp_debug_display_line(v));
    CHECK_EQ(0x02, status(v, 1, 1));

    // One it skips matches nothing.
    set_reg(v, IRQLINE, 170);
    run_to(v, 190);  // display 166
    set_reg(v, VMODE, 0x04);
    run_to(v, 191);  // display 191: 167-190 skipped
    run_to(v, 215);
    CHECK_EQ(0, v->irq_latch & 0x02);

    // IRQLINE is eight bits: display lines 256-261 match nothing.
    set_reg(v, IRQLINE, 0);
    run_to(v, 255);
    CHECK_EQ(0, status(v, 1, 1));
    run_to(v, 261);
    CHECK_EQ(0, v->irq_latch);
    run_to(v, 0);  // display line 0 in Full
    CHECK_EQ(0x02, v->irq_latch);

    // Disabled, it leaves no trace.
    CHECK_EQ(0x02, status(v, 1, 1));
    set_reg(v, IRQEN, 0x00);
    run_to(v, 0);
    CHECK_EQ(0, v->irq_latch);
    free(v);
}

TEST(vertical_blank_and_irqline_together) {
    vdp_t *v = card_in(0x0);
    set_reg(v, IRQLINE, 192);
    set_reg(v, IRQEN, 0x03);
    run_to(v, 216);
    CHECK_EQ(0x03, vdp_debug_status(v, 1));
    free(v);
}

// Every flag and every latch up, as a frame with sprites would leave them.
static void raise_everything(vdp_t *v) {
    set_reg(v, IRQEN, 0x0f);
    set_reg(v, IRQLINE, 192);
    run_to(v, 216);  // F, vertical blank, and the compare at display line 192
    CHECK(vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK(vdp_frame_event(v, VDP_IRQ_COLLISION));
    v->stat0 |= VDP_STAT0_OVF | VDP_STAT0_COL | 0x15;
    v->overflow_sprite = 0x35;
    for (unsigned i = 0; i < 8; i++) v->collision_map[i] = (uint8_t)(0x81 << (i & 1));
}

TEST(stat1_acknowledges_the_latches_and_nothing_else) {
    vdp_t *v = card_in(0x0);
    raise_everything(v);
    CHECK_EQ(0x0f, status(v, 1, 1));
    CHECK(!vdp_int_asserted(v));
    CHECK_EQ(0, vdp_debug_status(v, 1));
    CHECK_EQ(0xf5, vdp_debug_status(v, 0));  // F and the rest still there for port A
    CHECK_EQ(0x35, vdp_debug_status(v, 7));
    CHECK_EQ(0x81, vdp_debug_status(v, 8));
    CHECK_EQ(0x02, vdp_debug_status(v, 15));
    free(v);
}

TEST(stat0_acknowledges_its_flags_and_their_interrupts) {
    vdp_t *v = card_in(0x0);
    raise_everything(v);
    // Read the detail first, which STAT0 clears (§6); reading it acknowledges nothing.
    CHECK_EQ(0x35, status(v, 0, 7));
    for (unsigned s = 8; s < 16; s++) CHECK_EQ((uint8_t)(0x81 << (s & 1)), status(v, 0, s));
    CHECK_EQ(0x0f, vdp_debug_status(v, 1));

    CHECK_EQ(0xf5, status(v, 0, 0));
    CHECK_EQ(0, vdp_debug_status(v, 0));
    CHECK_EQ(0, vdp_debug_status(v, 7));
    for (unsigned s = 8; s < 16; s++) CHECK_EQ(0, vdp_debug_status(v, s));
    // The compare has no STAT0 flag; only STAT1 acknowledges it.
    CHECK_EQ(0x02, vdp_debug_status(v, 1));
    CHECK(vdp_int_asserted(v));
    CHECK_EQ(0x02, status(v, 1, 1));
    CHECK(!vdp_int_asserted(v));
    free(v);
}

TEST(int_is_the_latches_still_enabled) {
    vdp_t *v = card_in(0x0);
    set_reg(v, 0x01, 0x60);  // MODE1 b5: IRQEN b0 by its legacy name
    run_to(v, 216);
    CHECK(vdp_int_asserted(v));

    set_reg(v, 0x01, 0x40);  // IE off, nothing read
    CHECK(!vdp_int_asserted(v));
    CHECK_EQ(0, vdp_debug_status(v, 1));
    CHECK_EQ(0x01, v->irq_latch);  // released, not acknowledged
    set_reg(v, IRQEN, 0xf1);   // back on, through IRQEN; b7:4 are ignored
    CHECK(vdp_int_asserted(v));
    CHECK_EQ(0x01, vdp_debug_status(v, 1));
    free(v);
}

TEST(overflow_and_collision_once_a_frame) {
    vdp_t *v = card_in(0x3);
    set_reg(v, IRQEN, 0x0c);
    run_to(v, 100);
    CHECK(vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK_EQ(0x04, status(v, 1, 1));
    // Acknowledged at once, and the next overflowing line of the frame is not
    // raised again.
    CHECK(!vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK_EQ(0, v->irq_latch);
    CHECK(vdp_frame_event(v, VDP_IRQ_COLLISION));  // the other guard is its own

    // They belong to the frame whose line is being built: the line built as
    // screen line 261 begins is the next frame's display line 0.
    run_to(v, 260);
    CHECK(!vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK(!vdp_frame_event(v, VDP_IRQ_COLLISION));
    run_to(v, 261);
    CHECK(vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK(vdp_frame_event(v, VDP_IRQ_COLLISION));
    CHECK_EQ(0x0c, v->irq_latch);
    run_to(v, 0);  // a frame's start re-arms vertical blank only
    CHECK(!vdp_frame_event(v, VDP_IRQ_OVERFLOW));

    // Spent while disabled, it stays spent: enabling it later in the frame
    // latches nothing.
    CHECK_EQ(0x0c, status(v, 1, 1));
    run_to(v, 261);
    set_reg(v, IRQEN, 0x00);
    CHECK(vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    set_reg(v, IRQEN, 0x04);
    CHECK(!vdp_frame_event(v, VDP_IRQ_OVERFLOW));
    CHECK_EQ(0, v->irq_latch & 0x04);
    free(v);
}

TEST(each_port_reads_through_its_own_selector) {
    vdp_t *v = card_in(0x0);
    run_to(v, 216);
    set_reg(v, STATSEL_A, 4);
    set_reg(v, STATSEL_B, 0);
    CHECK_EQ(0xac, vdp_read(v, 1));
    CHECK_EQ(0xac, vdp_read(v, 1));  // no acknowledgement from STAT4
    CHECK_EQ(0x80, vdp_read(v, 3) & 0x80);
    CHECK_EQ(0, vdp_debug_status(v, 0));  // port B's read cleared it for both
    set_reg(v, STATSEL_A, 0x14);        // b7:4 ignored
    CHECK_EQ(0xac, vdp_read(v, 1));
    free(v);
}

TEST(rst_clears_status_and_leaves_the_raster) {
    vdp_t *v = card_in(0x3);
    raise_everything(v);  // at screen line 216, Graphics: no vertical blank yet
    run_to(v, 240);
    CHECK_EQ(0x0d, v->irq_latch & 0x0d);
    CHECK(vdp_int_asserted(v));
    uint8_t spent = v->frame_events;

    vdp_reset(v, false);
    CHECK(!vdp_int_asserted(v));
    for (unsigned s = 0; s < 16; s++) {
        if (s == 2 || s == 3 || (s >= 4 && s <= 6)) continue;
        CHECK_EQ(0, vdp_debug_status(v, s));
    }
    CHECK_EQ(0, v->irq_latch);
    CHECK_EQ(spent, v->frame_events);
    CHECK_EQ(240, vdp_debug_display_line(v));  // the line being scanned carries on
    CHECK_EQ(240, v->screen_line);

    // Numbered for the reset geometry, 192 lines, from the next line start; the
    // frame's vertical blank is not raised a second time.
    set_reg(v, IRQEN, 0x01);
    run_to(v, 241);
    CHECK_EQ(217, vdp_debug_display_line(v));
    CHECK_EQ(0, v->stat0);
    CHECK_EQ(0, v->irq_latch);
    run_to(v, 216);
    CHECK_EQ(0x80, v->stat0);  // the next frame's
    free(v);

    // Power-on: nothing has happened in the frame.
    v = card_in(0x0);
    run_to(v, 216);
    vdp_reset(v, true);
    CHECK_EQ(0, v->frame_events);
    CHECK_EQ(0, v->stat0);
    free(v);
}

TEST(a_snapshot_carries_status_and_the_line_as_numbered) {
    vdp_t *v = card_in(0x0);
    raise_everything(v);
    run_to(v, 130);
    set_reg(v, VMODE, 0x04);  // renumbers only at the next line start
    CHECK_EQ(106, vdp_debug_display_line(v));

    vdp_snapshot_t s;
    static uint8_t vram[VDP_VRAM_SIZE];
    vdp_debug_save(v, &s, vram);
    vdp_t *w = new_card();
    vdp_debug_restore(w, &s, vram);
    CHECK_EQ(106, vdp_debug_display_line(w));
    CHECK_EQ(130, w->screen_line);
    for (unsigned sel = 0; sel < 16; sel++) CHECK_EQ(vdp_debug_status(v, sel), vdp_debug_status(w, sel));
    CHECK_EQ(v->frame_events, w->frame_events);
    CHECK_EQ(v->irq_latch, w->irq_latch);
    CHECK(vdp_int_asserted(w));
    run_to(v, 131);
    run_to(w, 131);
    CHECK_EQ(131, vdp_debug_display_line(w));
    free(v);
    free(w);
}

int main(void) {
    RUN(display_line_is_the_screen_line_less_the_top_border);
    RUN(hblank_is_the_platforms);
    RUN(a_change_of_height_renumbers_at_the_next_line_start);
    RUN(vertical_blank_at_the_end_of_the_picture);
    RUN(f_sets_whatever_irqen_and_the_display_say);
    RUN(vertical_blank_once_a_frame_across_changes_of_height);
    RUN(scanline_compare_on_every_match);
    RUN(vertical_blank_and_irqline_together);
    RUN(stat1_acknowledges_the_latches_and_nothing_else);
    RUN(stat0_acknowledges_its_flags_and_their_interrupts);
    RUN(int_is_the_latches_still_enabled);
    RUN(overflow_and_collision_once_a_frame);
    RUN(each_port_reads_through_its_own_selector);
    RUN(rst_clears_status_and_leaves_the_raster);
    RUN(a_snapshot_carries_status_and_the_line_as_numbered);
    return TEST_RESULT();
}
