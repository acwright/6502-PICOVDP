// Inspecting the card without disturbing it, and putting a card back.
//
// For the host adapter (host/node), the unit tests and, from Phase 8, the debug
// link. Nothing here is a bus operation: none of it moves a pointer, refills a
// prefetch or acknowledges a status. Writes that change the card go through the
// same paths the bus takes, so a debugger's poke and a program's store reach the
// screen the same way. A release firmware does not link it (ground rule 8).

#pragma once

#include "vdp.h"

#ifdef __cplusplus
extern "C" {
#endif

// §5: a register, through the $02-$06 aliases, as the bus side holds it.
uint8_t vdp_debug_register(const vdp_t *v, unsigned index);
// As a command-port register write, without the command port's flip-flop.
void    vdp_debug_set_register(vdp_t *v, unsigned index, uint8_t value);

// §7: a VRAM byte, as the bus side holds it.
uint8_t vdp_debug_vram(const vdp_t *v, uint16_t address);
// As a data-port store, without a pointer: journaled and snooped.
void    vdp_debug_set_vram(vdp_t *v, uint16_t address, uint8_t value);

// §4: one port pair, 0 for A and 1 for B.
vdp_port_t vdp_debug_port(const vdp_t *v, unsigned pair);

// §11: an entry as 12-bit $RGB, as the next line built draws it — from the bus
// side's window at the bus side's PALBASE. The render side's cache reaches the
// same value at the next vdp_line_start; tests/unit holds it to that.
uint16_t vdp_debug_palette(const vdp_t *v, unsigned entry);

// §6: a status register as a port selecting it would read it, without what
// reading it does — no acknowledgement of STAT0 or STAT1, no flip-flop reset.
// STAT3 b1 is the platform's last vdp_set_hblank.
uint8_t vdp_debug_status(const vdp_t *v, unsigned select);

// §3: the display line being scanned, 0-261, as it was numbered when it began.
uint16_t vdp_debug_display_line(const vdp_t *v);

// §9: what the register file selects.
typedef struct vdp_debug_mode {
    uint8_t vmode;          // VMODE b3:0, as written
    uint8_t legacy;         // 0 none, 1 Text, 2 Graphics I, 3 Graphics II, 4 Multicolor
    uint8_t geometry;       // 0 Text, 1 Compact, 2 Graphics, 3 Full
    uint8_t cols, rows, cell_width;
    uint16_t width, lines, origin_x, origin_y;
    bool display;           // MODE1 b6
} vdp_debug_mode_t;
vdp_debug_mode_t vdp_debug_mode(const vdp_t *v);

// The statistics the debug link reports (PLAN.md section 3's STATS).
typedef struct vdp_debug_stats {
    uint32_t journal_overflows;  // catch-ups that copied pages because the journal filled
    uint32_t latches_merged;     // latches merged into a full ring's newest: lines never built (§18)
} vdp_debug_stats_t;
vdp_debug_stats_t vdp_debug_stats(const vdp_t *v);

// A card's state, less VRAM, which travels beside it. What a snapshot restores
// is a discontinuity, not a bus operation: both sides are loaded at once, with
// nothing pending.
typedef struct vdp_snapshot {
    uint8_t registers[VDP_REGISTERS];  // raw, $02-$06 included
    vdp_port_t port[2];
    uint16_t screen_line;
    uint16_t display_line;  // not always derivable: a mode change renumbers only at the next line start
    uint8_t stat0;
    uint8_t irq_latch;
    uint8_t frame_events;
    uint8_t overflow_sprite;
    uint8_t collision_map[8];
    // §7: the FONT loads waiting for vertical blank, as vdp_t holds them.
    uint8_t font_pending;   // bit n: a load for layer n
    uint8_t font_id[2];
    uint16_t font_base[2];
} vdp_snapshot_t;
void vdp_debug_save(const vdp_t *v, vdp_snapshot_t *s, uint8_t *vram);
void vdp_debug_restore(vdp_t *v, const vdp_snapshot_t *s, const uint8_t *vram);

#ifdef __cplusplus
}
#endif
