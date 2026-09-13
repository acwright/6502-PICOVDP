// Phase 1 timing spike: a scanline renderer for SPEC.md's worst cases.
// Disposable (PLAN.md section 6, Phase 1). It implements the rules that cost
// time — §8's tile engine, §10's sprites, §12's priority, §11's expansion —
// for the Graphics and Full geometries, and nothing of the bus, the legacy
// submode or the status registers. Portable C11: built for the RP2350 to be
// timed, and for the host to be checked against a per-pixel reference.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPIKE_WIDTH 320     // the virtual frame, §3
#define SPIKE_HEIGHT 240
#define SPIKE_SLACK_L 8     // a line buffer's slack either side of its 320 pixels:
#define SPIKE_SLACK_R 16    // whole cells are written, starting up to 7 px early
#define SPIKE_LINE_BUF (SPIKE_SLACK_L + SPIKE_WIDTH + SPIKE_SLACK_R)
#define SPIKE_VRAM_GUARD 8  // VRAM $0000–$0007 mirrored past $FFFF: a pattern row is read whole

enum { SPIKE_GRAPHICS, SPIKE_FULL };                     // §9 VMODE $3, $4
enum { SPIKE_1BPP, SPIKE_2BPP, SPIKE_4BPP, SPIKE_8BPP }; // §8 LxCTRL b1:b0

typedef struct {
    bool enabled;
    bool opaque;        // LxCTRL b5
    uint8_t depth;      // SPIKE_1BPP..SPIKE_8BPP
    uint16_t name;      // absolute VRAM addresses (base × granularity, §5)
    uint16_t attr;      // attribute source is always per cell (§8 b3:b2 = 00)
    uint16_t pat;
    uint16_t scrx;      // 9 bits: LxSCRX plus LxCTRL b6
    uint8_t scry;
    uint8_t pal;        // LxPAL b3:0
} spike_layer_t;

typedef struct {
    uint8_t vram[0x10000 + SPIKE_VRAM_GUARD];

    // Geometry, §9
    uint16_t width;     // 256 or 320
    uint8_t cols;       // 32 or 40
    uint8_t left;       // picture's x in the frame: 32 or 0

    spike_layer_t layer[2];

    // Sprites, §10 (VMODE semantics, not legacy)
    bool spr_enabled;   // SPRCTRL b0
    bool collision;     // SPRCTRL b1
    bool detailed;      // SPRCTRL b3
    bool d0term;        // SPRCTRL b2
    bool size16;        // MODE1 b1
    bool mag;           // MODE1 b0
    uint8_t spr_depth;  // SPRCTRL b5:4
    uint8_t sprpal;
    uint8_t sprcount;   // 0..64
    uint8_t sprlimit;   // 0..32
    uint16_t sprattr, sprpat;

    uint8_t backdrop;   // palette index, §11

    // Implementation choice the spike compares
    bool use_tab4;      // 4bpp through §18's 8 KB unpacking table, not by arithmetic

    uint32_t pal2[256]; // 12-bit 0x0BGR, paired: x × $10001 (§11 cache)

    // Status, §6 — sticky
    bool ovf, col;
    uint8_t first_drop;
    uint64_t colmap;
} spike_t;

// Per-pixel priority class (§12). Layers are drawn before sprites, so a pixel
// records only what the sources still to come need to know:
#define SPIKE_PR_BLOCK_SPR  0x01    // level 3, 4 or 6: a normal sprite loses here
#define SPIKE_PR_BLOCK_SPRP 0x02    // level 6: so does a priority sprite
#define SPIKE_PR_BLOCK_L1   0x04    // level 4: so does a normal layer 1 tile
// Layer 0 normal 0, priority 5; layer 1 normal 1, priority 3; backdrop 0.

// A line in progress.
typedef struct {
    uint8_t idx_buf[SPIKE_LINE_BUF];    // palette indices
    uint8_t pr_buf[SPIKE_LINE_BUF];     // priority classes
    uint8_t list[32];                   // sprites drawn on this line, in table order
    uint8_t nlist;
} spike_line_t;

#define SPIKE_IDX(ln) ((ln)->idx_buf + SPIKE_SLACK_L)   // frame x = 0

void spike_init_tables(void);
void spike_set_geometry(spike_t *v, int geom);
void spike_vram_sealed(spike_t *v);     // after writing VRAM $0000–$0007: refresh the guard

// The stages of one line build, in order. Each is timed on its own.
void spike_sprite_eval(spike_t *v, spike_line_t *ln, int line);
void spike_layer0(const spike_t *v, spike_line_t *ln, int line);
void spike_layer1(const spike_t *v, spike_line_t *ln, int line);
void spike_sprites(spike_t *v, spike_line_t *ln, int line);
void spike_finish(const spike_t *v, spike_line_t *ln, uint16_t *rgb);  // border, then 320 → 640

// All of the above.
void spike_build_line(spike_t *v, spike_line_t *ln, int line, uint16_t *rgb);
