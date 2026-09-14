// 6502-PICOVDP core: all of SPEC.md's behaviour, in portable C11.
//
// PLAN.md section 3. One state structure and no globals, so the host can run
// many instances; nothing here includes a pico-sdk header or touches hardware
// (ground rule 5). The same interface is built for the RP2350 and for macOS.
//
// The card is split into a bus side and a render side. The bus side — the two
// port pairs, the register file, VRAM — is what vdp_read and vdp_write touch,
// at once, as the CPU does. The render side is a copy of the registers, VRAM
// and the palette that the build functions read, and nothing else writes: it
// catches up with the bus side at vdp_line_start, which is §3's latch. So a
// line is built from the card as it stood when the line before it began, however
// the bus interrupts the build (PLAN.md section 3, "Why the split exists").
//
// Phase 3: the bus side (§4, §5, §7), the palette (§11) and reset (§15).
// Phase 4: the raster's line numbering (§3), status (§6) and interrupts (§14).
// Phase 5: the tile engine at 1bpp (§8), the geometries and the legacy submode
// (§9), for both layers.
// Phase 6: sprites (§10) and their place among the layers (§12).
// Phase 7: 2, 4 and 8bpp layers, the attribute byte and §12's seven levels (§8,
// §12, §13): the whole of SPEC.md's picture.
// Phase 8: the latch in two halves, for the firmware's two contexts. Its bus
// side, vdp_latch, is taken at the line's start, in core 1's interrupt; its
// render side, vdp_catch_up, by the renderer when it is ready for the line,
// which after a late line is later (§18). A latch records the register file and
// where the journal stood, so a line is still built from the card as it stood
// at its own latch. Status is published by vdp_publish, which the firmware
// holds interrupts off around. vdp_line_start is all three at once. The layers
// and sprites are drawn a word at a time, and a row is built in two halves, one
// a core, each into a line of its own (vdp_build_half).

#pragma once

#include <stdbool.h>
#include <stdint.h>

// §3: the virtual frame, and the raster it is scanned by.
#define VDP_WIDTH 320
#define VDP_HEIGHT 240
#define VDP_SCREEN_LINES 262

// §4: A1:A0.
#define VDP_PORT_DATA_A 0
#define VDP_PORT_STATUS_A 1
#define VDP_PORT_DATA_B 2
#define VDP_PORT_STATUS_B 3

// §5, §7, §11.
#define VDP_REGISTERS 128
#define VDP_VRAM_SIZE 0x10000
#define VDP_PALETTE_ENTRIES 256

// VRAM writes the journal holds for the render side before a write falls back
// to marking its 1 KB page for a whole copy (PLAN.md section 3). No 6502 comes
// near it: at 2 MHz back-to-back `sta` is 2 µs (§4), 32 writes in a 63.56 µs
// line. A power of two: the journal is a ring.
#define VDP_JOURNAL_ENTRIES 1024
#define VDP_VRAM_PAGE_SHIFT 10

// Latches the render side can fall behind by (§18's late lines). A latch that
// finds them all waiting is merged into the newest, whose line is then never
// built. A power of two.
#define VDP_LATCHES 8

// §10: the most sprites one line draws, SPRLIMIT's ceiling.
#define VDP_SPRITES_PER_LINE 32

// A line is built in a half (below), with slack either side of its 320 bytes
// for whole cells and whole words (Phase 8), and VRAM's render copy carries its
// first bytes again past $FFFF, so a pattern row is read whole where it wraps.
#define VDP_LINE_SLACK 8
#define VDP_LINE_BYTES (VDP_LINE_SLACK + VDP_WIDTH + 2 * VDP_LINE_SLACK)
#define VDP_VRAM_GUARD 8

// Sprite claims, a bit a picture column, in words: one spare for a sprite
// running past the last.
#define VDP_CLAIM_WORDS ((VDP_WIDTH + 31) / 32 + 1)

// One sprite the latch found on the line about to be built (§10), with what the
// build needs of its slot already read from the render copy.
typedef struct vdp_sprite {
    int16_t left;        // its left edge, as a picture column: -128 to 383, or legacy's -32 to 255
    uint8_t slot;        // 0-63: priority among sprites, and its bit in the collision map
    uint8_t row;         // the pattern row this line draws, 0 to size - 1, the vertical flip applied
    uint8_t pattern;
    uint8_t attributes;
} vdp_sprite_t;

// One core's part of a line (PLAN.md section 3): picture columns [x0, x1),
// built into a line of its own, so the two cores never write a byte the other
// reads. A half's frame columns are its picture columns and the border beside
// them: the left border goes with the half that starts at column 0, the right
// with the one that ends at the picture's edge; an empty half has none. Holds
// no status: the collisions found in it are published by vdp_publish.
typedef struct vdp_half {
    int16_t x0, x1;
    bool collided;               // two sprite pixels met in [x0, x1), and collision is enabled (§10)
    uint64_t collisions;         // with SPRCTRL b3, the sprites that did: bit s for slot s (§6)
    uint8_t line[VDP_LINE_BYTES];   // palette indices: frame column x at VDP_LINE_SLACK + x
    uint8_t level[VDP_LINE_BYTES];  // §12's level of the pixel each column holds, the same way
    // Detailed collision's scratch: for each word of sprite claims, the slots
    // that first covered its pixels, and which.
    uint8_t owners[VDP_CLAIM_WORDS];
    uint8_t owner_slot[VDP_CLAIM_WORDS][VDP_SPRITES_PER_LINE];
    uint32_t owner_bits[VDP_CLAIM_WORDS][VDP_SPRITES_PER_LINE];
} vdp_half_t;

// One port pair (§4). $9C02/$9C03 are a complete second copy of $9C00/$9C01.
typedef struct vdp_port {
    uint16_t pointer;  // the full 16-bit VRAM pointer
    uint8_t prefetch;  // the byte the next data-port read returns
    uint8_t payload;   // the first byte of a command pair
    bool read_mode;    // the direction latch: the last address set was a read
    bool second;       // the flip-flop: the next command-port write completes a pair
} vdp_port_t;

// What a latch leaves for the render side (§3): the card as it stood, less
// VRAM, which the journal carries.
typedef struct vdp_latch_record {
    uint8_t reg[VDP_REGISTERS];
    uint64_t dirty_pages;    // pages written past a full journal before the latch
    uint32_t journal_end;    // the journal's tail at the latch: the writes before it
    uint32_t tag;            // the platform's, handed back with the render side (vdp_latch)
    uint16_t screen_line;
} vdp_latch_record_t;

// The whole card. Callers allocate it — it is about 141 KB, so statically or on
// the heap — and hand it to vdp_init. Its fields belong to the core.
typedef struct vdp {
    // ---- the bus side ----
    vdp_port_t port[2];                     // §4: A and B
    uint8_t reg[VDP_REGISTERS];             // §5, with $02-$06 stored at their aliases
    uint8_t vram[VDP_VRAM_SIZE];            // §7
    uint8_t version;                        // STAT5, BCD (§6)

    // VRAM writes the render side has not taken, in a ring: entries run from
    // journal_head, the render side's, to journal_tail, the bus side's, both
    // counting up forever and indexed modulo VDP_JOURNAL_ENTRIES. Each side
    // writes only its own, so a write can interrupt a catch-up.
    uint16_t journal_address[VDP_JOURNAL_ENTRIES];
    uint8_t journal_value[VDP_JOURNAL_ENTRIES];
    uint32_t journal_head;
    uint32_t journal_tail;
    uint64_t dirty_pages;                   // pages written after the journal filled, since the last latch
    uint32_t journal_overflows;             // catch-ups that fell back to page copies

    // Latches the render side has not taken, a ring the same way: latch_tail
    // is vdp_latch's, latch_head vdp_catch_up's.
    vdp_latch_record_t latch[VDP_LATCHES];
    uint32_t latch_head;
    uint32_t latch_tail;
    uint32_t latches_merged;                // latches merged into a full ring's newest

    uint16_t screen_line;                   // the screen line last begun, §3
    uint16_t display_line;                  // its number from the picture's first line, as it began, §3
    bool hblank;                            // STAT3 b1, from the platform, §6

    // §6, §14: flags, latches and the once-a-frame guards. Set at the latch,
    // and from Phase 6 by the sprites; cleared by status reads and reset.
    uint8_t stat0;                          // b7 F, b6 OVF, b5 COL, b4:0 the first sprite dropped
    uint8_t irq_latch;                      // STAT1's sources, as IRQEN bits, latched while enabled
    uint8_t frame_events;                   // IRQEN bits of the once-a-frame events spent this frame
    uint8_t overflow_sprite;                // STAT7
    uint8_t collision_map[8];               // STAT8-STAT15

    // ---- the render side: the card as it stood at the last latch ----
    uint8_t render_reg[VDP_REGISTERS];
    uint8_t render_vram[VDP_VRAM_SIZE + VDP_VRAM_GUARD];
    uint16_t render_screen_line;            // the screen line the build makes: the one after the latch's, §3
    uint32_t render_tag;                    // the tag of the latch the render side reached
    uint8_t render_overflow;                // 1 + the slot the evaluation dropped, 0 for none: for vdp_publish
    // §11's cache: each entry's 12-bit 0x0BGR twice, x * 0x10001, so one load
    // expands a pixel to two (§18).
    uint32_t palette[VDP_PALETTE_ENTRIES];
    // §10: the sprites that line draws, in table order, as the latch evaluated
    // them. Only those reaching into the picture; a sprite off its left or
    // right counted toward SPRLIMIT and is not here.
    uint8_t sprite_count;
    vdp_sprite_t sprite[VDP_SPRITES_PER_LINE];

    // ---- the build's scratch ----
    vdp_half_t half;                        // core 1's half of the line, or all of it
} vdp_t;

#ifdef __cplusplus
extern "C" {
#endif

void    vdp_init(vdp_t *v, uint8_t version);                // a card that has been made has been reset: power-on, STAT5 = version
void    vdp_reset(vdp_t *v, bool power_on);                 // §15; RST leaves the raster running
uint8_t vdp_read(vdp_t *v, unsigned port);                  // port = A1:A0, §4
void    vdp_write(vdp_t *v, unsigned port, uint8_t value);  // §4
void    vdp_line_start(vdp_t *v, uint16_t screen_line);     // a screen line begins, §3: latch, catch up, publish
void    vdp_latch(vdp_t *v, uint16_t screen_line, uint32_t tag); // its bus side, now; tag comes back in render_tag
bool    vdp_catch_up(vdp_t *v);                             // the render side to the oldest latch it has not taken; false if none
void    vdp_set_hblank(vdp_t *v, bool hblank);              // STAT3 b1, from the platform, §6
int     vdp_split_choose(const vdp_t *v);                   // the picture column the cores divide this row at; 0: one core builds it
void    vdp_build_half(const vdp_t *v, vdp_half_t *h, int x0, int x1); // picture columns [x0, x1): backdrop, layers, sprites; no status
void    vdp_expand_half(const vdp_t *v, const vdp_half_t *h, uint16_t *rgb);   // its frame columns, 12-bit 0x0BGR, x2
void    vdp_copy_half(const vdp_t *v, const vdp_half_t *h, uint8_t *indices);  // its frame columns into a row of 320
void    vdp_publish(vdp_t *v, const vdp_half_t *a, const vdp_half_t *b); // the row's overflow, and each half's collisions if not NULL
void    vdp_build_line(vdp_t *v, uint8_t *indices);         // both halves on one thread, published, into 320 indices (host)
void    vdp_expand_line(const vdp_t *v, const uint8_t *indices, uint16_t *rgb); // 12-bit 0x0BGR, x2
bool    vdp_int_asserted(const vdp_t *v);                   // §14

#ifdef __cplusplus
}
#endif
