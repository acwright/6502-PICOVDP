// The raster's line numbering (§3), the status registers (§6) and interrupts
// (§14).
//
// Bus side only. Everything here runs at a line start or a status read — on the
// RP2350 both are core 1 interrupts of one priority, so neither preempts the
// other (PLAN.md section 3) and nothing here needs a lock.

#include "vdp_internal.h"

#include <string.h>

// §3, §14. The display line is numbered with the geometry in effect as the line
// begins, so a change between a 192- and a 240-line geometry moves it 24 lines
// here and nowhere else.
void VDP_HOT(vdp_raster_line_start)(vdp_t *v, uint16_t screen_line) {
    const vdp_geometry_t *g = vdp_geometry(v->reg, NULL);
    uint16_t line = vdp_display_line_of(screen_line, g);
    v->display_line = line;

    // A frame begins at screen line 0, and its vertical blank is still to come.
    if (screen_line == 0) v->frame_events &= (uint8_t)~VDP_IRQ_VBLANK;

    // The end of the picture: the first line start of the frame at or past it in
    // the geometry of the moment — screen line 216 in Text and Compact, 240 in
    // Graphics and Full. F sets whatever IRQEN and the display say (§6). A
    // picture that shrinks past its new end raises it at once; one that grows
    // after it has fired does not raise it again.
    if (screen_line >= g->origin_y + g->lines && vdp_frame_event(v, VDP_IRQ_VBLANK)) {
        v->stat0 |= VDP_STAT0_F;
    }

    // Every match, so a handler that reprograms IRQLINE gets another. IRQLINE is
    // eight bits: display lines 256-261 match nothing.
    if (line == v->reg[VDP_REG_IRQLINE]) vdp_latch_interrupt(v, VDP_IRQ_SCANLINE);

    // Overflow and collision belong to the frame whose line is being built: the
    // line built as screen line 261 begins is the next frame's first.
    if (screen_line == VDP_SCREEN_LINES - 1) {
        v->frame_events &= (uint8_t)~(VDP_IRQ_OVERFLOW | VDP_IRQ_COLLISION);
    }
}

// §6: a read of STAT0 clears its flags and what details them, and the three
// STAT1 latches those flags stand for, so TMS9918 code that reads status to
// acknowledge its interrupt still does. The scanline latch is left for STAT1.
static void acknowledge_flags(vdp_t *v) {
    v->stat0 = 0;
    v->overflow_sprite = 0;
    memset(v->collision_map, 0, sizeof v->collision_map);
    v->irq_latch &= (uint8_t)~(VDP_IRQ_VBLANK | VDP_IRQ_OVERFLOW | VDP_IRQ_COLLISION);
}

// §14: the latched sources still enabled in IRQEN as it is now — STAT1, and /INT.
static inline uint8_t pending(const vdp_t *v) {
    return (uint8_t)(v->irq_latch & v->reg[VDP_REG_IRQEN] & VDP_IRQ_SOURCES);
}

uint8_t VDP_HOT(vdp_status_peek)(const vdp_t *v, unsigned select) {
    switch (select & VDP_STATSEL_MASK) {
    case 0:
        return v->stat0;
    case 1:
        return pending(v);
    case 2:
        // Low 8 bits: 256-261 alias to 0-5, and STAT3 b0 tells them apart.
        return (uint8_t)v->display_line;
    case 3: {
        // b0 against the picture's height as it is now, whichever geometry
        // numbered the line.
        const vdp_geometry_t *g = vdp_geometry(v->reg, NULL);
        return (uint8_t)((v->display_line >= g->lines ? 0x01 : 0) | (v->hblank ? 0x02 : 0));
    }
    case 4:
        return VDP_STAT_IDENTIFICATION;
    case 5:
        return v->version;
    case 6:
        return VDP_STAT_CAPABILITIES;
    case 7:
        return v->overflow_sprite;
    default:
        // STAT8-STAT15: the collision map, bit s mod 8 of STAT(8 + s/8).
        return v->collision_map[(select & VDP_STATSEL_MASK) - 8];
    }
}

uint8_t VDP_HOT(vdp_status_read)(vdp_t *v, unsigned select) {
    uint8_t value = vdp_status_peek(v, select);
    switch (select & VDP_STATSEL_MASK) {
    case 0:
        acknowledge_flags(v);
        break;
    case 1:
        v->irq_latch = 0;
        break;
    default:
        break;
    }
    return value;
}

void vdp_status_reset(vdp_t *v, bool power_on) {
    // /INT released; every flag, STAT7, the map and every latch clear (§15).
    acknowledge_flags(v);
    v->irq_latch = 0;
    if (!power_on) {
        // The raster runs on: the line being scanned keeps its number until the
        // next line start, and the frame's vertical blank is not raised twice.
        return;
    }
    // Where the raster stands at power-on is undefined (§15); the platform says
    // with its next line start. Until then the line is numbered as the reset
    // geometry would number it, and nothing has happened in its frame.
    v->frame_events = 0;
    v->display_line = vdp_display_line_of(v->screen_line, vdp_geometry(v->reg, NULL));
}

bool VDP_HOT(vdp_int_asserted)(const vdp_t *v) {
    return pending(v) != 0;
}
