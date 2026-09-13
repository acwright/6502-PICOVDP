// 6502-PICOVDP core: all of SPEC.md's behaviour, in portable C11.
//
// PLAN.md section 3. One state structure and no globals, so the host can run
// many instances; nothing here includes a pico-sdk header or touches hardware
// (ground rule 5). The same interface is built for the RP2350 and for macOS.
//
// Phase 2: the interface only. Every function exists and does nothing useful,
// so the N-API adapter (host/node) and the test harnesses have a core to link
// against. Phases 3-7 fill it in.

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

// A sprite line built on core 0 for core 1 to merge (section 3). Its layout is
// Phase 6's.
typedef struct vdp_sprline {
    int x0, x1;
} vdp_sprline_t;

// The whole card. Callers allocate it; Phase 3 gives it its bus side and
// Phase 5 its render side.
typedef struct vdp {
    uint16_t screen_line; // the screen line last begun, §3
    bool hblank;          // STAT3 b1, from the platform, §6
} vdp_t;

#ifdef __cplusplus
extern "C" {
#endif

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
