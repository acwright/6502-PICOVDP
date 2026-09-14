// Shared inside core/ only: register numbers, bit names, and the helpers more
// than one file needs. The spec section each comes from is beside it (rule 9).

#pragma once

#include <string.h>

#include "vdp.h"

// Hot functions: in RAM on the RP2350 (PLAN.md section 3). The image is
// copy_to_ram already, so this only names the section the SDK's linker script
// keeps there, without including an SDK header (rule 5).
#ifdef PICOVDP_RP2350
#define VDP_HOT(name) __attribute__((section(".time_critical." #name))) name
#else
#define VDP_HOT(name) name
#endif

#define VDP_VRAM_MASK (VDP_VRAM_SIZE - 1)
#define VDP_REGISTER_MASK (VDP_REGISTERS - 1)

// ---- words ----
//
// The renderer works in 32-bit words of byte-wide pixels (PLAN.md section 3,
// Phase 8), which assumes a little-endian machine: byte 0 of a word is the
// leftmost pixel. Both of this core's platforms are.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the renderer's words are little-endian"
#endif

#define VDP_ONES 0x01010101u

static inline uint32_t vdp_load32(const uint8_t *p) {
    uint32_t w;
    memcpy(&w, p, sizeof w);
    return w;
}

static inline void vdp_store32(uint8_t *p, uint32_t w) {
    memcpy(p, &w, sizeof w);
}

// A word's bytes in the other order: pixels mirrored.
static inline uint32_t vdp_swap32(uint32_t w) {
    return w << 24 | (w << 8 & 0xff0000u) | (w >> 8 & 0xff00u) | w >> 24;
}

// $FF in each byte that is not zero.
static inline uint32_t vdp_nonzero_bytes(uint32_t w) {
    return ((((w & 0x7f7f7f7fu) + 0x7f7f7f7fu) | w) >> 7 & VDP_ONES) * 0xff;
}

// Bytes of $00 or $FF, one bit each, byte 0 in bit 0.
static inline unsigned vdp_byte_bits(uint32_t mask) {
    return ((mask & VDP_ONES) * 0x01020408u) >> 24;
}

// $FF in each byte holding a §12 level below `level`: the pixels a source at
// `level` beats. Levels are 0-6, so no byte carries into the next.
static inline uint32_t vdp_beaten(uint32_t levels, unsigned level) {
    return ((((levels + (0x80u - level) * VDP_ONES) >> 7) & VDP_ONES) ^ VDP_ONES) * 0xff;
}

// $FF in each byte where `a`'s level is above `b`'s.
static inline uint32_t vdp_above(uint32_t a, uint32_t b) {
    return ((((a | 0x80808080u) - b - VDP_ONES) >> 7) & VDP_ONES) * 0xff;
}

// tables.c
extern const uint16_t vdp_unpack4[16][256];
extern const uint32_t vdp_values2[256];
extern const uint8_t vdp_reverse8[256];
extern const uint16_t vdp_spread8[256];
extern const uint32_t vdp_nibble_msb[16];
extern const uint32_t vdp_nibble_lsb[16];

// ---- §5: the register file ----

#define VDP_REG_MODE0 0x00
#define VDP_REG_MODE1 0x01
#define VDP_REG_COLOR 0x07
#define VDP_REG_VBANK 0x08
#define VDP_REG_VINC 0x09
#define VDP_REG_IRQEN 0x0a
#define VDP_REG_IRQLINE 0x0b
#define VDP_REG_PALBASE 0x0c
#define VDP_REG_VMODE 0x0d
#define VDP_REG_STATSEL_B 0x0e
#define VDP_REG_STATSEL_A 0x0f
#define VDP_REG_L0NAME 0x10
#define VDP_REG_L0ATTR 0x11
#define VDP_REG_L0PAT 0x12
#define VDP_REG_L0CTRL 0x15
#define VDP_REG_L0PAL 0x16
#define VDP_REG_L1NAME 0x18
#define VDP_REG_L1CTRL 0x1d
#define VDP_REG_SPRATTR 0x20
#define VDP_REG_SPRPAT 0x21
#define VDP_REG_SPRCOUNT 0x22
#define VDP_REG_SPRCTRL 0x23
#define VDP_REG_SPRLIMIT 0x24
#define VDP_REG_SPRPAL 0x25

// MODE0 and MODE1 bits (§5, §9).
#define VDP_MODE0_M3 0x02
#define VDP_MODE1_DISP 0x40
#define VDP_MODE1_IE 0x20
#define VDP_MODE1_M1 0x10
#define VDP_MODE1_M2 0x08
#define VDP_MODE1_SIZE16 0x02    // §10: 16 x 16 sprites
#define VDP_MODE1_MAG 0x01       // §10: every sprite x2

// IRQEN and STAT1 bits (§14).
#define VDP_IRQ_VBLANK 0x01
#define VDP_IRQ_SCANLINE 0x02
#define VDP_IRQ_OVERFLOW 0x04
#define VDP_IRQ_COLLISION 0x08
#define VDP_IRQ_SOURCES 0x0f

// STAT0 bits (§6).
#define VDP_STAT0_F 0x80
#define VDP_STAT0_OVF 0x40
#define VDP_STAT0_COL 0x20
#define VDP_STAT0_SPRITE 0x1f

// Where a register's byte lives: $02-$06 are the same storage as $10-$12, $20
// and $21 (§5), resolved on the way in and on the way out, so the byte has one
// home and no two copies can disagree.
static inline unsigned vdp_register_home(unsigned index) {
    static const uint8_t alias[7] = {
        0x00, 0x01, VDP_REG_L0NAME, VDP_REG_L0ATTR, VDP_REG_L0PAT, VDP_REG_SPRATTR, VDP_REG_SPRPAT,
    };
    index &= VDP_REGISTER_MASK;
    return index < sizeof alias ? alias[index] : index;
}

// ---- §4 ----

#define VDP_CMD_REGISTER 0x80     // %1rrrrrrr
#define VDP_CMD_WRITE 0x40        // %01aaaaaa, against %00aaaaaa for a read
#define VDP_CMD_ADDRESS 0x3f
#define VDP_STATSEL_MASK 0x0f

// ---- §6 ----

#define VDP_STAT_IDENTIFICATION 0xac   // STAT4
#define VDP_STAT_CAPABILITIES 0x3f     // STAT6: two layers, 8bpp, flip, scroll, scanline IRQ, 64 KB

// ---- §11: the palette ----

#define VDP_PALETTE_BYTES 512
#define VDP_PALBASE_MASK 0x3f

// The first byte of the 512-byte window: PALBASE b5:0 x $400. A 1 KB granule
// holding 512 bytes, so the window lies inside one VRAM page and never wraps.
static inline uint16_t vdp_palette_base(const uint8_t *reg) {
    return (uint16_t)((reg[VDP_REG_PALBASE] & VDP_PALBASE_MASK) << VDP_VRAM_PAGE_SHIFT);
}

// §11's table, transcribed: 256 entries of 12-bit $RGB (palette.c).
extern const uint16_t vdp_default_palette[VDP_PALETTE_ENTRIES];

// Write the default palette into a VRAM image at base.
void vdp_palette_install(uint8_t *vram, uint16_t base);

// One entry of the render side's cache, from the render copy of VRAM.
void vdp_palette_cache_entry(vdp_t *v, unsigned entry);

// All 256, as a PALBASE change does.
void vdp_palette_reload(vdp_t *v);

// A VRAM image's two bytes for an entry, as 12-bit $RGB.
static inline uint16_t vdp_palette_decode(const uint8_t *vram, uint16_t base, unsigned entry) {
    uint16_t address = (uint16_t)(base + 2 * entry);
    return (uint16_t)(((vram[address] & 0x0f) << 8) | vram[(uint16_t)(address + 1)]);
}

// ---- §9: geometry ----

typedef enum vdp_geometry_id {
    VDP_GEOMETRY_TEXT,
    VDP_GEOMETRY_COMPACT,
    VDP_GEOMETRY_GRAPHICS,
    VDP_GEOMETRY_FULL,
} vdp_geometry_id_t;

typedef enum vdp_legacy_mode {
    VDP_LEGACY_NONE,         // VMODE names the geometry itself
    VDP_LEGACY_TEXT,
    VDP_LEGACY_GRAPHICS_I,
    VDP_LEGACY_GRAPHICS_II,  // not supported: drawn as Graphics I
    VDP_LEGACY_MULTICOLOR,   // not supported: drawn as Graphics I
} vdp_legacy_mode_t;

typedef struct vdp_geometry {
    vdp_geometry_id_t id;
    uint8_t cols, rows;
    uint8_t cell_width;      // pixels of pattern a cell draws across: 6 in Text
    uint16_t width, lines;   // the picture
    uint16_t origin_x, origin_y;  // where it sits in the 320 x 240 frame (§3)
} vdp_geometry_t;

// The geometry a register file selects, and the legacy mode behind it.
const vdp_geometry_t *vdp_geometry(const uint8_t *reg, vdp_legacy_mode_t *legacy);

// ---- §8: the tile engine (tiles.c) ----

// LxCTRL (§5, §8).
#define VDP_LXCTRL_DEPTH 0x03
#define VDP_LXCTRL_ATTR_SOURCE 0x0c
#define VDP_LXCTRL_ENABLE 0x10
#define VDP_LXCTRL_INDEX0_OPAQUE 0x20
#define VDP_LXCTRL_SCRX_BIT8 0x40

// b1:b0.
#define VDP_DEPTH_1BPP 0
#define VDP_DEPTH_2BPP 1
#define VDP_DEPTH_4BPP 2
#define VDP_DEPTH_8BPP 3

// b3:b2.
#define VDP_ATTR_PER_CELL 0
#define VDP_ATTR_PER_GROUP 1
#define VDP_ATTR_PER_ROW 2
#define VDP_ATTR_NONE 3

// Draw layer 0 or 1's display line `line`, 0 to g->lines - 1, into picture
// columns [x0, x1) of the g->width pixels at `pixels`, over what they hold. `legacy` is the legacy mode
// pinning the layer (§9): layer 0's in the legacy submode, else
// VDP_LEGACY_NONE. A transparent pixel is left as it is. Where `levels` is not
// NULL it holds the §12 level of each picture column, the backdrop's before
// layer 0 is drawn: layer 1 writes only where its level beats the one there,
// and each pixel written has its level recorded, for layer 1 and the sprites
// to be judged against. Where it is NULL, every opaque pixel is written.
//
// Cells are drawn whole: a cell at either end of [x0, x1) may write up to 7
// pixels beyond it, into the slack of a half's line or columns that are not
// the half's (vdp.h).
void vdp_draw_layer(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
                    vdp_legacy_mode_t legacy, uint8_t *pixels, uint8_t *levels, int x0, int x1);

// §8, §12: whether layer 0's cells can carry attribute b6, and so stand at
// level 4 above layer 1's ordinary cells — an attribute byte at 2, 4 or 8bpp,
// outside the legacy submode, which pins 1bpp (§9).
static inline bool vdp_layer0_has_priority(const uint8_t *reg, vdp_legacy_mode_t legacy) {
    uint8_t control = reg[VDP_REG_L0CTRL];
    return legacy == VDP_LEGACY_NONE && (control & VDP_LXCTRL_ENABLE) &&
           (control & VDP_LXCTRL_DEPTH) != VDP_DEPTH_1BPP &&
           (control & VDP_LXCTRL_ATTR_SOURCE) >> 2 != VDP_ATTR_NONE;
}

// ---- §12: priority levels ----

#define VDP_LEVEL_BACKDROP 0
#define VDP_LEVEL_LAYER0 1
#define VDP_LEVEL_SPRITE 2
#define VDP_LEVEL_LAYER1 3
#define VDP_LEVEL_LAYER0_FRONT 4
#define VDP_LEVEL_SPRITE_FRONT 5
#define VDP_LEVEL_LAYER1_FRONT 6

// ---- §10: sprites (sprites.c) ----

// SPRCTRL (§5).
#define VDP_SPRCTRL_ENABLE 0x01
#define VDP_SPRCTRL_COLLISION 0x02
#define VDP_SPRCTRL_TERMINATOR 0x04
#define VDP_SPRCTRL_DETAILED 0x08
#define VDP_SPRCTRL_DEPTH 0x30

// §10: evaluate the sprites for the line the render side is about to build,
// into v->sprite. Writes no status: returns 1 + the slot of the first sprite
// the line drops, or 0, for vdp_publish to report (§6, §14).
uint8_t vdp_sprites_evaluate(vdp_t *v);

// §6, §14: a line dropped `slot` (sprites.c).
void vdp_report_overflow(vdp_t *v, unsigned slot);

// §6, §10, §14: what a half's sprites found colliding (sprites.c).
void vdp_publish_collisions(vdp_t *v, const vdp_half_t *h);

// Sprites over picture columns [x0, x1) of a half's line, after its layers,
// each pixel where its level beats the layers' (§12); the collisions found
// are left in the half (sprites.c).
void vdp_draw_sprites(const vdp_t *v, vdp_half_t *h, int x0, int x1, uint8_t *picture, const uint8_t *levels);

// ---- §3, §6, §14: the raster, status and interrupts (status.c) ----

// The events of a screen line's start: its display line, a frame's start,
// vertical blank, the scanline compare, and the sprite events' new frame.
void vdp_raster_line_start(vdp_t *v, uint16_t screen_line);

// A status register as a read on a port returns it, acknowledging what STAT0
// and STAT1 acknowledge (§6).
uint8_t vdp_status_read(vdp_t *v, unsigned select);

// The same without the acknowledgement: a debugger's look.
uint8_t vdp_status_peek(const vdp_t *v, unsigned select);

// §15: flags, latches, and at power-on the frame's spent events.
void vdp_status_reset(vdp_t *v, bool power_on);

// §3: a screen line's display line in a geometry.
static inline uint16_t vdp_display_line_of(uint16_t screen_line, const vdp_geometry_t *g) {
    return (uint16_t)((screen_line + VDP_SCREEN_LINES - g->origin_y) % VDP_SCREEN_LINES);
}

// §14: latch a source in STAT1 if IRQEN enables it now. A disabled source
// leaves no trace.
static inline void vdp_latch_interrupt(vdp_t *v, uint8_t source) {
    v->irq_latch |= (uint8_t)(v->reg[VDP_REG_IRQEN] & source);
}

// §14: vertical blank, overflow or collision — once a frame. False, and nothing
// latched, if the frame has spent it already, however quickly it was
// acknowledged.
static inline bool vdp_frame_event(vdp_t *v, uint8_t source) {
    if (v->frame_events & source) return false;
    v->frame_events |= source;
    vdp_latch_interrupt(v, source);
    return true;
}

// ---- §7: VRAM writes ----

// The render copy's guard: its first bytes again past $FFFF (vdp.h).
static inline void vdp_render_guard(vdp_t *v) {
    memcpy(v->render_vram + VDP_VRAM_SIZE, v->render_vram, VDP_VRAM_GUARD);
}

// Store a byte in the bus copy and journal it for the render side.
void vdp_poke(vdp_t *v, uint16_t address, uint8_t value);

// A register write, from the command port or a debugger.
void vdp_register_write(vdp_t *v, unsigned index, uint8_t value);
