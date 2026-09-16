// The built-in font (§7): FONT's loads, which complete at vertical blank.
//
// The bytes are fonts/cp437-6x8.bin, generated into font_data.c at build time
// (core/CMakeLists.txt); none are typed here. Bus side only: a load is recorded
// as FONT is written, and carried out at the latch where vertical blank fires,
// both in core 1's interrupts of one priority on the RP2350.

#include "vdp_internal.h"

// §7: the destination is sampled now, so a later LxPAT write does not move the
// load. A reserved ID is stored in the register and does nothing more: it
// neither loads nor cancels. A second load for a destination replaces the first.
void VDP_HOT(vdp_font_command)(vdp_t *v, uint8_t value) {
    uint8_t id = value & VDP_FONT_ID_MASK;
    if (id != VDP_FONT_CP437) return;
    unsigned layer = (value & VDP_FONT_LAYER1) ? 1 : 0;
    uint8_t pattern = v->reg[layer ? VDP_REG_L1PAT : VDP_REG_L0PAT];
    v->font_id[layer] = id;
    v->font_base[layer] = (uint16_t)(pattern << 11);  // x $800: at most $F800, so 2 KB never wraps
    v->font_pending |= (uint8_t)(1u << layer);
}

// §7, §14: at the line start where vertical blank fires, before F sets, before
// the latch record is taken, so the render side has the pages from this latch.
// A 2 KB copy is VRAM written in every other respect: the palette cache takes
// what lands in its window, and no port's pointer or prefetch moves.
void VDP_HOT(vdp_font_complete)(vdp_t *v) {
    for (unsigned layer = 0; layer < 2; layer++) {
        if (!(v->font_pending & (1u << layer))) continue;
        // Font $00 is the only one there is; vdp_font_command records no other.
        vdp_bulk_write(v, v->font_base[layer], vdp_font_cp437, VDP_FONT_BYTES);
    }
    v->font_pending = 0;
}
