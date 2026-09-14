// Phase 1's worst-case scenes, as a program would make them: port writes into
// the card (PLAN.md Phase 8). Portable C11 on vdp.h's port numbers alone, so
// the firmware runs them and host/scene draws the same frames on the host.
//
// Each is the worst a line can be at its settings (docs/results/phase-01.md):
// both layers on with per-cell attributes, scrolled, the scroll moving every
// frame; layer 0 opaque, layer 1 half transparent; a quarter of the cells with
// the priority bit, random flips and bit 8; all 64 sprite slots covering the
// band at display lines 100 up, SPRLIMIT of them drawn, every sprite pixel
// solid and each overlapping the next.
//
// The program's frame: the scene is set up at a line start of screen line 250,
// in vertical blank. At each later screen line 250 it reads status (below),
// then scrolls both layers for the next frame.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct scene {
    char name[32];          // geometry-depth-sprites[-det][-limN], as Phase 1 named them
    uint8_t vmode;          // 3 Graphics, 4 Full (§9)
    uint8_t depth;          // LxCTRL b1:0 for both layers, SPRCTRL b5:4 for the sprites
    bool magnified;         // 16 x 16 sprites, x2
    bool detailed;          // SPRCTRL b3
    uint8_t limit;          // SPRLIMIT
} scene_t;

// Where a scene's operations go.
typedef struct scene_port {
    void (*write)(void *context, unsigned port, uint8_t value);
    uint8_t (*read)(void *context, unsigned port);
    void *context;
} scene_port_t;

// Phase 1's split suite: every geometry, depth and sprite setting at SPRLIMIT
// 32, then Full mode at 4bpp at SPRLIMIT 16.
unsigned scene_count(void);
const scene_t *scene_at(unsigned index);
int scene_find(const char *name);

// Everything from a power-on reset, frame 0's scroll included.
void scene_setup(const scene_t *s, const scene_port_t *port);

// The scroll for scene frame n.
void scene_frame(const scene_t *s, unsigned n, const scene_port_t *port);

// What the program reads each frame, in this order: STAT1, STAT7, STAT8-15,
// then STAT0, which acknowledges the rest (§6).
#define SCENE_READS 11
void scene_reads(const scene_port_t *port, uint8_t reads[SCENE_READS]);

// The screen line the program's frame runs at.
#define SCENE_LINE 250
