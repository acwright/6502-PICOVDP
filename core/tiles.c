// The tile engine (§8): one layer, one display line.
//
// One engine for both layers. The layer number picks a register block (§5) and
// nothing else in here knows which of the two it is drawing. Every mode is a
// name table mapping cells to patterns, a pattern table of pixels and a source
// of colour: Graphics I is the per-pattern-group source, Text is no attribute
// fetch at all (§9).
//
// Reads only the render side (PLAN.md section 3): render_reg and render_vram,
// as they stood at the latch.
//
// Phase 5: 1bpp, with all four attribute sources, COLOR, LxPAL, transparency
// and the legacy submode's pins. The cell walk takes the scroll registers
// (§13) from the start, because the walk is the same loop either way. 2, 4 and
// 8bpp draw nothing until Phase 7, which adds them, the attribute byte and the
// compositor that the byte's priority bit needs.

#include "vdp_internal.h"

// §8: a pattern is 8 rows; at 1bpp, one byte to a row.
#define CELL_HEIGHT 8
#define PATTERN_BYTES_1BPP 8

// §5: a layer's seven registers, by offset from its block.
#define LREG_NAME 0
#define LREG_ATTR 1
#define LREG_PAT 2
#define LREG_SCRX 3
#define LREG_SCRY 4
#define LREG_CTRL 5
#define LREG_PAL 6

static const uint8_t layer_block[2] = {VDP_REG_L0NAME, VDP_REG_L1NAME};

void VDP_HOT(vdp_draw_layer)(const vdp_t *v, unsigned layer, uint16_t line, const vdp_geometry_t *g,
                             vdp_legacy_mode_t legacy, uint8_t *pixels) {
    const uint8_t *reg = v->render_reg + layer_block[layer];
    const uint8_t *vram = v->render_vram;
    uint8_t control = reg[LREG_CTRL];
    // §9 pins layer 0's depth and attribute source in the legacy submode, and
    // ignores its index 0 opaque bit: TMS9918 colour 0 is transparent. Layer 1
    // is unaffected, so the caller passes VDP_LEGACY_NONE for it.
    bool pinned = legacy != VDP_LEGACY_NONE;

    unsigned depth = pinned ? VDP_DEPTH_1BPP : control & VDP_LXCTRL_DEPTH;
    if (depth != VDP_DEPTH_1BPP) return;  // Phase 7

    unsigned source = pinned ? (legacy == VDP_LEGACY_TEXT ? VDP_ATTR_NONE : VDP_ATTR_PER_GROUP)
                             : (control & VDP_LXCTRL_ATTR_SOURCE) >> 2;
    bool opaque = !pinned && (control & VDP_LXCTRL_INDEX0_OPAQUE);
    uint8_t palette_high = (uint8_t)((reg[LREG_PAL] & 0x0f) << 4);
    uint8_t colour = v->render_reg[VDP_REG_COLOR];

    // §5: 1 KB granules for the name and attribute tables, 2 KB for patterns,
    // but ×$40 for layer 0's attribute table in the legacy submode (§9). Every
    // table address wraps at 64 KB (§7), which uint16_t arithmetic does.
    uint16_t name_base = (uint16_t)(reg[LREG_NAME] << 10);
    uint16_t attr_base = (uint16_t)(reg[LREG_ATTR] << (pinned ? 6 : 10));
    uint16_t pattern_base = (uint16_t)(reg[LREG_PAT] << 11);

    // §13: the map is the picture, and wraps onto itself. X is nine bits, the
    // register and LxCTRL b6.
    unsigned width = g->width;
    unsigned scroll_x = ((control & VDP_LXCTRL_SCRX_BIT8 ? 0x100u : 0) | reg[LREG_SCRX]) % width;
    unsigned map_y = line + reg[LREG_SCRY] % g->lines;
    if (map_y >= g->lines) map_y -= g->lines;

    unsigned row = map_y & (CELL_HEIGHT - 1);
    uint16_t cell_base = (uint16_t)((map_y / CELL_HEIGHT) * g->cols);
    uint16_t name_row = (uint16_t)(name_base + cell_base);

    // Cells, not columns: with a scroll that is not a multiple of the cell
    // width the first and last cells are partial, so the walk starts wherever
    // column 0 lands in the map and takes as much of each cell as fits.
    unsigned cell_width = g->cell_width;
    unsigned col = scroll_x / cell_width;
    unsigned first = scroll_x - col * cell_width;
    unsigned x = 0;
    while (x < width) {
        unsigned count = cell_width - first;
        if (count > width - x) count = width - x;

        uint8_t pattern = vram[(uint16_t)(name_row + col)];

        // §8's attribute sources. "None" at 1bpp is COLOR, which is today's text
        // mode.
        uint8_t attribute;
        switch (source) {
        case VDP_ATTR_PER_CELL:
            attribute = vram[(uint16_t)(attr_base + cell_base + col)];
            break;
        case VDP_ATTR_PER_GROUP:
            attribute = vram[(uint16_t)(attr_base + (pattern >> 3))];
            break;
        case VDP_ATTR_PER_ROW:
            attribute = vram[(uint16_t)(attr_base + pattern * CELL_HEIGHT + row)];
            break;
        default:
            attribute = colour;
            break;
        }

        // §8: foreground b7:4, background b3:0, each a 4-bit index into the
        // sixteen colours LxPAL names. A nibble of 0 is transparent, either
        // one, unless index 0 is opaque.
        unsigned foreground = attribute >> 4;
        unsigned background = attribute & 0x0f;
        bool fg_opaque = foreground != 0 || opaque;
        bool bg_opaque = background != 0 || opaque;
        uint8_t fg = (uint8_t)(palette_high | foreground);
        uint8_t bg = (uint8_t)(palette_high | background);

        // Pattern bits leftmost first, from b7 (§8); a partial cell starts
        // partway into the byte. In Text only the top six bits are reached.
        unsigned bits = (unsigned)(vram[(uint16_t)(pattern_base + pattern * PATTERN_BYTES_1BPP + row)] << first);
        uint8_t *out = pixels + x;

        if (fg_opaque && bg_opaque) {
            // Every pixel written: the BIOS console's loop, with no branch.
            for (unsigned i = 0; i < count; i++, bits <<= 1) out[i] = (bits & 0x80) ? fg : bg;
        } else {
            // A transparent pixel is one not written, which leaves what is
            // behind it: the backdrop, or layer 0 under layer 1 (§12).
            for (unsigned i = 0; i < count; i++, bits <<= 1) {
                bool on = (bits & 0x80) != 0;
                if (on ? fg_opaque : bg_opaque) out[i] = on ? fg : bg;
            }
        }

        x += count;
        first = 0;
        if (++col >= g->cols) col = 0;
    }
}
