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
// Lines build as the backdrop; the tile engine is Phase 5's, sprites Phase 6's.

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

// VRAM writes one line can journal before the latch falls back to copying the
// 1 KB pages written since (PLAN.md section 3). No 6502 comes near it: at 2 MHz
// back-to-back `sta` is 2 µs (§4), 32 writes in a 63.56 µs line.
#define VDP_JOURNAL_ENTRIES 1024
#define VDP_VRAM_PAGE_SHIFT 10

// A sprite line built on core 0 for core 1 to merge (section 3). Its layout is
// Phase 6's.
typedef struct vdp_sprline {
    int x0, x1;
} vdp_sprline_t;

// One port pair (§4). $9C02/$9C03 are a complete second copy of $9C00/$9C01.
typedef struct vdp_port {
    uint16_t pointer;  // the full 16-bit VRAM pointer
    uint8_t prefetch;  // the byte the next data-port read returns
    uint8_t payload;   // the first byte of a command pair
    bool read_mode;    // the direction latch: the last address set was a read
    bool second;       // the flip-flop: the next command-port write completes a pair
} vdp_port_t;

// The whole card. Callers allocate it — it is about 135 KB, so statically or on
// the heap — and hand it to vdp_init. Its fields belong to the core.
typedef struct vdp {
    // ---- the bus side ----
    vdp_port_t port[2];                     // §4: A and B
    uint8_t reg[VDP_REGISTERS];             // §5, with $02-$06 stored at their aliases
    uint8_t vram[VDP_VRAM_SIZE];            // §7
    uint8_t version;                        // STAT5, BCD (§6)

    // VRAM writes since the last latch, for the render side to replay.
    uint16_t journal_address[VDP_JOURNAL_ENTRIES];
    uint8_t journal_value[VDP_JOURNAL_ENTRIES];
    uint16_t journal_count;
    uint64_t dirty_pages;                   // pages written after the journal filled
    uint32_t journal_overflows;             // latches that fell back to page copies

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
    uint8_t render_vram[VDP_VRAM_SIZE];
    // §11's cache: each entry's 12-bit 0x0BGR twice, x * 0x10001, so one load
    // expands a pixel to two (§18).
    uint32_t palette[VDP_PALETTE_ENTRIES];
} vdp_t;

#ifdef __cplusplus
extern "C" {
#endif

void    vdp_init(vdp_t *v, uint8_t version);                // a card that has been made has been reset: power-on, STAT5 = version
void    vdp_reset(vdp_t *v, bool power_on);                 // §15; RST leaves the raster running
uint8_t vdp_read(vdp_t *v, unsigned port);                  // port = A1:A0, §4
void    vdp_write(vdp_t *v, unsigned port, uint8_t value);  // §4
void    vdp_line_start(vdp_t *v, uint16_t screen_line);     // a screen line begins, §3
void    vdp_set_hblank(vdp_t *v, bool hblank);              // STAT3 b1, from the platform, §6
int     vdp_split_choose(const vdp_t *v);                   // the column the cores divide this line at
void    vdp_build_layers(vdp_t *v, uint8_t *indices);       // 320 palette indices, both layers
void    vdp_build_sprites(const vdp_t *v, vdp_sprline_t *s, int x0, int x1); // columns [x0, x1), no status
void    vdp_draw_sprites(vdp_t *v, uint8_t *indices, int x0, int x1);        // straight into the line
void    vdp_merge_sprites(vdp_t *v, uint8_t *indices, const vdp_sprline_t *s); // and publish its collisions
void    vdp_build_line(vdp_t *v, uint8_t *indices);         // all of the above on one core (host)
void    vdp_expand_line(const vdp_t *v, const uint8_t *indices, uint16_t *rgb); // 12-bit 0x0BGR, x2
bool    vdp_int_asserted(const vdp_t *v);                   // §14

#ifdef __cplusplus
}
#endif
