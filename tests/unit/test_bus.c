// §4, §5, §7: the ports, the register file and VRAM, through the bus.
//
// Video.test.ts's bus block covers the same ground through the adapter; these
// are the cases worth pinning in C too, where the firmware's own build runs them.

#include "card.h"
#include "test.h"

// §4's prefetch table, row by row.
TEST(prefetch_follows_the_table) {
    vdp_t *v = new_card();
    uint8_t bytes[] = {0x11, 0x22, 0x33};
    store(v, 0x0200, bytes, 3);

    // Set a read address: fetch the byte at the pointer into the prefetch; advance.
    point_at(v, 0, 0x0200, false);
    vdp_port_t a = vdp_debug_port(v, 0);
    CHECK_EQ(0x11, a.prefetch);
    CHECK_EQ(0x0201, a.pointer);
    CHECK(a.read_mode);

    // Read VC_DATA: return the prefetch; fetch the byte at the pointer; advance.
    CHECK_EQ(0x11, vdp_read(v, 0));
    CHECK_EQ(0x22, vdp_read(v, 0));
    CHECK_EQ(0x0203, vdp_debug_port(v, 0).pointer);

    // Write VC_DATA: store; load the written byte into the prefetch; advance.
    vdp_write(v, 0, 0x99);
    CHECK_EQ(0x99, vdp_debug_vram(v, 0x0203));
    CHECK_EQ(0x99, vdp_read(v, 0));
    CHECK_EQ(0x0205, vdp_debug_port(v, 0).pointer);

    // Set a write address: nothing more. The prefetch is left as it was.
    point_at(v, 0, 0x0200, true);
    a = vdp_debug_port(v, 0);
    CHECK_EQ(0x0200, a.pointer);
    CHECK(!a.read_mode);
    CHECK_EQ(0x00, a.prefetch);  // the byte at $0204, fetched by the read above

    // The direction latch is read by nothing: a write-pointed port still reads.
    CHECK_EQ(0x00, vdp_read(v, 0));
    CHECK_EQ(0x11, vdp_read(v, 0));
    free(v);
}

TEST(pairs_are_independent) {
    vdp_t *v = new_card();
    vdp_write(v, 1, 0x77);            // A: payload latched
    command(v, 1, 0x42, 0x87);        // B: a whole command
    vdp_read(v, 3);                   // B: status, which resets B's flip-flop only
    vdp_write(v, 1, 0x87);            // A completes its pair
    CHECK_EQ(0x77, vdp_debug_register(v, 7));

    point_at(v, 0, 0x0100, true);
    point_at(v, 1, 0x2000, true);
    vdp_write(v, 0, 0xa0);
    vdp_write(v, 2, 0xb0);
    CHECK_EQ(0xa0, vdp_debug_vram(v, 0x0100));
    CHECK_EQ(0xb0, vdp_debug_vram(v, 0x2000));
    CHECK_EQ(0xb0, vdp_debug_port(v, 1).prefetch);
    CHECK_EQ(0xa0, vdp_debug_port(v, 0).prefetch);
    free(v);
}

// §4: any access to a pair's data port, and any read of its status port, resets
// the flip-flop. A command-port write never does.
TEST(flip_flop_resets) {
    vdp_t *v = new_card();
    vdp_write(v, 1, 0x33);
    vdp_write(v, 0, 0x00);  // data write
    CHECK(!vdp_debug_port(v, 0).second);
    vdp_write(v, 1, 0x33);
    vdp_read(v, 0);         // data read
    CHECK(!vdp_debug_port(v, 0).second);
    vdp_write(v, 1, 0x33);
    vdp_read(v, 1);         // status read
    CHECK(!vdp_debug_port(v, 0).second);
    vdp_write(v, 1, 0x33);
    CHECK(vdp_debug_port(v, 0).second);
    CHECK_EQ(0x33, vdp_debug_port(v, 0).payload);
    free(v);
}

// §4: a register write moves neither port's pointer nor prefetch.
TEST(register_write_leaves_pointers) {
    vdp_t *v = new_card();
    vdp_debug_set_vram(v, 0x1234, 0x5a);
    point_at(v, 0, 0x1234, false);
    point_at(v, 1, 0x0042, true);
    vdp_port_t a = vdp_debug_port(v, 0), b = vdp_debug_port(v, 1);
    set_reg(v, 0x07, 0xf1);
    command(v, 1, 0x10, 0x80 | 0x22);
    CHECK_EQ(a.pointer, vdp_debug_port(v, 0).pointer);
    CHECK_EQ(a.prefetch, vdp_debug_port(v, 0).prefetch);
    CHECK_EQ(b.pointer, vdp_debug_port(v, 1).pointer);
    CHECK_EQ(b.prefetch, vdp_debug_port(v, 1).prefetch);
    free(v);
}

// §4: bits 15:14 from VBANK when the command completes; the carry is the
// pointer's, wrapping at 64 KB both ways; VINC is signed and shared.
TEST(pointer_bank_carry_and_stride) {
    vdp_t *v = new_card();
    set_reg(v, 0x08, 0xfd);  // b1:0 = 1, the rest stored and ignored
    command(v, 0, 0xff, 0x40 | 0x3f);
    CHECK_EQ(0x7fff, vdp_debug_port(v, 0).pointer);
    set_reg(v, 0x08, 0x03);  // does not move a pointer already set
    vdp_write(v, 0, 0x01);
    vdp_write(v, 0, 0x02);
    CHECK_EQ(0x01, vdp_debug_vram(v, 0x7fff));
    CHECK_EQ(0x02, vdp_debug_vram(v, 0x8000));
    CHECK_EQ(0x03, vdp_debug_register(v, 0x08));  // VBANK does not follow a carry

    command(v, 0, 0xff, 0x40 | 0x3f);  // bank 3: $FFFF
    vdp_write(v, 0, 0xee);
    CHECK_EQ(0x0000, vdp_debug_port(v, 0).pointer);

    set_reg(v, 0x09, 0xff);  // -1
    set_reg(v, 0x08, 0x00);
    command(v, 0, 0x00, 0x40);  // $0000
    vdp_write(v, 0, 0xdd);
    CHECK_EQ(0xffff, vdp_debug_port(v, 0).pointer);

    set_reg(v, 0x09, 0x80);  // -128
    command(v, 1, 0x40, 0x00);  // B, read $0040: prefetch, then advance
    CHECK_EQ((0x0040 - 128) & 0xffff, vdp_debug_port(v, 1).pointer);

    set_reg(v, 0x09, 0x00);  // no increment
    command(v, 0, 0x00, 0x42);
    vdp_write(v, 0, 0x01);
    vdp_write(v, 0, 0x02);
    CHECK_EQ(0x0200, vdp_debug_port(v, 0).pointer);
    CHECK_EQ(0x02, vdp_debug_vram(v, 0x0200));
    free(v);
}

// §5: seven bits of register number, the $02-$06 aliases, reserved registers
// stored, MODE1 b5 and IRQEN b0 one bit.
TEST(register_file) {
    vdp_t *v = new_card();
    for (unsigned reg = 0; reg < 128; reg++) set_reg(v, reg, (uint8_t)(reg * 7));
    static const uint8_t home[][2] = {{0x02, 0x10}, {0x03, 0x11}, {0x04, 0x12}, {0x05, 0x20}, {0x06, 0x21}};
    for (unsigned i = 0; i < 5; i++) {
        CHECK_EQ(vdp_debug_register(v, home[i][1]), vdp_debug_register(v, home[i][0]));
        set_reg(v, home[i][0], 0xa5);
        CHECK_EQ(0xa5, vdp_debug_register(v, home[i][1]));
    }
    CHECK_EQ((0x17 * 7) & 0xff, vdp_debug_register(v, 0x17));  // reserved: stored
    CHECK_EQ((0x7f * 7) & 0xff, vdp_debug_register(v, 0x7f));

    set_reg(v, 0x01, 0x20);
    CHECK_EQ(0x01, vdp_debug_register(v, 0x0a) & 0x01);
    set_reg(v, 0x0a, 0x0e);
    CHECK_EQ(0x00, vdp_debug_register(v, 0x01) & 0x20);
    set_reg(v, 0x0a, 0x01);
    CHECK_EQ(0x20, vdp_debug_register(v, 0x01));
    free(v);
}

// §6: STAT4 and STAT6 are constants, STAT5 the version the card was made with;
// each port reads through its own selector.
TEST(constant_status_registers) {
    vdp_t *v = new_card();
    set_reg(v, 0x0f, 0xf4);  // STATSEL_A: b7:4 ignored
    set_reg(v, 0x0e, 0x06);  // STATSEL_B
    CHECK_EQ(0xac, vdp_read(v, 1));
    CHECK_EQ(0xbf, vdp_read(v, 3));  // b7: the built-in font (§7)
    set_reg(v, 0x0f, 0x05);
    CHECK_EQ(0x04, vdp_read(v, 1));
    free(v);
}

int main(void) {
    RUN(prefetch_follows_the_table);
    RUN(pairs_are_independent);
    RUN(flip_flop_resets);
    RUN(register_write_leaves_pointers);
    RUN(pointer_bank_carry_and_stride);
    RUN(register_file);
    RUN(constant_status_registers);
    return TEST_RESULT();
}
