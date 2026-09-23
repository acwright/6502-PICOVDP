// Bench cards: pictures this repository authors itself, as a program of port
// operations (PLAN.md Phase 10).
//
// The oracle's fixtures are the emulator's programs, and they are what proves
// the card draws what SPEC.md says. A bench card proves something else: what
// reaches the monitor. Its picture is made of large flat areas of known colour,
// which is what an analog path through the DAC, a VGA-to-HDMI dongle and a
// capture card can carry without argument — where the oracle's densest frames
// are detail at every pixel and come back blurred (docs/results/phase-10.md).
//
// A card is drawn once, by the reference (tools/card.mjs), into a trace and a
// golden of the oracle's shape, and thereafter replayed on the board by
// `vdpctl inject` like any other checkpoint.
//
// Ports (§4): port 1 takes the command protocol, port 0 the data.

// §5's registers the cards write.
export const REG = {
  MODE1: 0x01, COLOR: 0x07, VBANK: 0x08, VINC: 0x09, PALBASE: 0x0c, VMODE: 0x0d,
  L0NAME: 0x10, L0ATTR: 0x11, L0PAT: 0x12, L0CTRL: 0x15, L0PAL: 0x16,
  L1NAME: 0x18, L1ATTR: 0x19, L1PAT: 0x1a, L1CTRL: 0x1d, L1PAL: 0x1e,
  SPRCTRL: 0x23,
}

// Full mode (§9): 40 x 30 cells of 8 x 8, the whole 320 x 240 frame.
export const COLS = 40
export const ROWS = 30

// Where a card puts its tables. Layer 0's patterns are 8bpp, 64 bytes each;
// layer 1 draws the built-in font where §7's reset leaves it.
export const VRAM = {
  L0_NAME: 0x0000,  // 1200 bytes
  FONT: 0x0800,     // the built-in font, 2 KB (§7)
  L1_NAME: 0x1000,  // 1200 bytes
  L0_PAT: 0x4000,   // 256 x 64 bytes
  PALETTE: 0xfc00,  // PALBASE's reset value (§11)
}

/** Collects a card's port operations. */
export class Writer {
  constructor() {
    this.ops = []
  }

  write(port, value) {
    this.ops.push([port, value & 0xff])
  }

  /** §4's register command: the value, then $80 | index. */
  reg(index, value) {
    this.write(1, value)
    this.write(1, 0x80 | index)
  }

  /** §4's address command, with VBANK for the bits above 14. */
  at(address) {
    this.reg(REG.VBANK, address >> 14)
    this.write(1, address & 0xff)
    this.write(1, 0x40 | ((address >> 8) & 0x3f))
  }

  data(value) {
    this.write(0, value)
  }

  fill(value, count) {
    for (let i = 0; i < count; i++) this.write(0, value)
  }

  /** CP437 text into a name table, at a cell. */
  text(base, cx, cy, string) {
    this.at(base + cy * COLS + cx)
    for (let i = 0; i < string.length; i++) this.data(string.charCodeAt(i))
  }
}

const HEX = '0123456789ABCDEF'

/** 8bpp patterns 0..count-1, each 64 bytes of its own index: pattern n is solid entry n. */
function solidPatterns(w, count) {
  w.at(VRAM.L0_PAT)
  for (let n = 0; n < count; n++) w.fill(n, 64)
}

/** The common opening: Full mode, layer 0 8bpp with no attribute byte, layer 1 the font. */
function setup(w, { color }) {
  w.reg(REG.VINC, 0x01)
  w.reg(REG.VMODE, 0x04)                 // Full: 40 x 30 (§9)
  w.reg(REG.COLOR, color)                // layer 1's foreground, and the backdrop (§5)
  w.reg(REG.L0NAME, VRAM.L0_NAME >> 10)
  w.reg(REG.L0PAT, VRAM.L0_PAT >> 11)
  w.reg(REG.L0CTRL, 0x3f)                // 8bpp, attribute source none, enabled, index 0 opaque (§8)
  w.reg(REG.L0PAL, 0x00)
  w.reg(REG.L1NAME, VRAM.L1_NAME >> 10)
  w.reg(REG.L1PAT, VRAM.FONT >> 11)      // the built-in font, where §7's reset leaves it
  w.reg(REG.L1CTRL, 0x1c)                // 1bpp, attribute source none, enabled, index 0 transparent
  // §10: SPRCTRL resets to $27, sprites on, with SPRATTR at $0000 — which is
  // layer 0's name table here, so a card that left it alone would draw a
  // sprite out of its own picture. Nothing on these cards is a sprite.
  w.reg(REG.SPRCTRL, 0x26)
  w.reg(REG.MODE1, 0x40)                 // DISP: the picture on, interrupts off (§5)
}

/**
 * The palette test card: every one of §11's 256 entries, 16 to a row, as a
 * swatch of 16 x 8 pixels, with the family down the side and the step across
 * the top. PLAN.md's Still Open 3 — are the hue ramps usable — is judged on
 * this, on a real monitor.
 */
function palette(w) {
  const GRID_X = 4, GRID_Y = 8   // cells; a swatch is two cells wide and one tall
  const PAPER = 0x17             // §11 row 1 step 7: mid grey, so a black swatch shows

  setup(w, { color: 0xf0 })      // foreground entry 15, backdrop entry 0
  w.reg(REG.L1PAL, 0x00)         // layer 1's nibbles index entries $00-$0F: $F is white
  solidPatterns(w, 256)

  // Layer 0: the paper, then the grid of swatches.
  w.at(VRAM.L0_NAME)
  w.fill(PAPER, COLS * ROWS)
  for (let row = 0; row < 16; row++) {
    w.at(VRAM.L0_NAME + (GRID_Y + row) * COLS + GRID_X)
    for (let step = 0; step < 16; step++) {
      w.data(row * 16 + step)
      w.data(row * 16 + step)
    }
  }

  // Layer 1: the labels.
  w.at(VRAM.L1_NAME)
  w.fill(0, COLS * ROWS)
  w.text(VRAM.L1_NAME, GRID_X, 2, '6502-PICOVDP PALETTE TEST CARD')
  w.text(VRAM.L1_NAME, GRID_X, 4, 'SPEC SECTION 11, ALL 256 ENTRIES')
  w.text(VRAM.L1_NAME, GRID_X, 6, 'COLUMN = STEP, ROW = FAMILY')
  for (let step = 0; step < 16; step++) w.text(VRAM.L1_NAME, GRID_X + 2 * step, 7, HEX[step])
  for (let row = 0; row < 16; row++) w.text(VRAM.L1_NAME, 2, GRID_Y + row, HEX[row])
  w.text(VRAM.L1_NAME, GRID_X, 25, 'ENTRY = ROW X 16 + COLUMN')
  w.text(VRAM.L1_NAME, GRID_X, 27, 'PAPER IS ENTRY $17')
}

/**
 * The DAC card: each channel's sixteen levels on its own, as bars of 16 x 24
 * pixels, and bands of one-pixel stripes. Flat colour a capture can measure,
 * and a picture in which a wrong bit order on GPIO 2-13 is plain to the eye:
 * red and blue swapped turn the top bar blue, a reversed nibble makes a ramp
 * that is not a ramp.
 *
 * It writes its own palette, so nothing here depends on §11's table:
 * entries $00-$0F are red 0-15, $10-$1F green, $20-$2F blue, $30-$3F grey,
 * and the rest are black.
 */
function dac(w) {
  const BAR_X = 4, BAR_Y = 5, BAR_H = 3, BAR_GAP = 4  // cells
  const STRIPE_H = 0xf0, STRIPE_V = 0xf1, CHECKER = 0xf2
  const WHITE = 0x3f

  setup(w, { color: 0xf0 })
  w.reg(REG.L1PAL, 0x03)   // layer 1's nibbles index entries $30-$3F: $F is white

  // §11's window: R in b3:0 of the first byte, G in b7:4 and B in b3:0 of the second.
  w.at(VRAM.PALETTE)
  for (let entry = 0; entry < 256; entry++) {
    const level = entry & 0x0f
    const channel = entry >> 4
    const rgb = channel === 0 ? [level, 0, 0]
      : channel === 1 ? [0, level, 0]
        : channel === 2 ? [0, 0, level]
          : channel === 3 ? [level, level, level]
            : [0, 0, 0]
    w.data(rgb[0])
    w.data((rgb[1] << 4) | rgb[2])
  }

  solidPatterns(w, 64)
  // The stripe patterns, at the top of the 8bpp table: a pixel of layer is a
  // pixel of card, so these are what line doubling and the ×2 across have to
  // carry (§3).
  w.at(VRAM.L0_PAT + STRIPE_H * 64)
  for (let row = 0; row < 8; row++) w.fill(row % 2 ? 0 : WHITE, 8)
  w.at(VRAM.L0_PAT + STRIPE_V * 64)
  for (let row = 0; row < 8; row++) for (let x = 0; x < 8; x++) w.data(x % 2 ? 0 : WHITE)
  w.at(VRAM.L0_PAT + CHECKER * 64)
  for (let row = 0; row < 8; row++) for (let x = 0; x < 8; x++) w.data((x + row) % 2 ? 0 : WHITE)

  w.at(VRAM.L0_NAME)
  w.fill(0, COLS * ROWS)
  for (let channel = 0; channel < 4; channel++) {
    for (let line = 0; line < BAR_H; line++) {
      w.at(VRAM.L0_NAME + (BAR_Y + channel * BAR_GAP + line) * COLS + BAR_X)
      for (let level = 0; level < 16; level++) {
        w.data(channel * 16 + level)
        w.data(channel * 16 + level)
      }
    }
  }
  for (let line = 0; line < 2; line++) {
    w.at(VRAM.L0_NAME + (22 + line) * COLS + BAR_X)
    for (let cell = 0; cell < 32; cell++) w.data(cell < 11 ? STRIPE_H : cell < 22 ? STRIPE_V : CHECKER)
  }

  w.at(VRAM.L1_NAME)
  w.fill(0, COLS * ROWS)
  w.text(VRAM.L1_NAME, BAR_X, 1, '6502-PICOVDP DAC CARD')
  w.text(VRAM.L1_NAME, BAR_X, 3, 'LEVEL')
  for (let level = 0; level < 16; level++) w.text(VRAM.L1_NAME, BAR_X + 2 * level, 4, HEX[level])
  const names = ['RED', 'GRN', 'BLU', 'WHT']
  for (let channel = 0; channel < 4; channel++) {
    w.text(VRAM.L1_NAME, 0, BAR_Y + channel * BAR_GAP + 1, names[channel])
  }
  w.text(VRAM.L1_NAME, 0, 20, 'ONE PIXEL STRIPES')
  w.text(VRAM.L1_NAME, 6, 25, 'ACROSS')
  w.text(VRAM.L1_NAME, 18, 25, 'DOWN')
  w.text(VRAM.L1_NAME, 28, 25, 'CHECK')
  w.text(VRAM.L1_NAME, BAR_X, 27, 'PALETTE WRITTEN BY THE CARD')
}

export const CARDS = {
  palette: {
    checkpoint: 'palette',
    title: 'the 256-entry palette test card',
    draw: palette,
  },
  dac: {
    checkpoint: 'dac',
    title: 'the DAC card: each channel’s sixteen levels, and one-pixel stripes',
    draw: dac,
    // Its entries are laid out for dacResponse (tools/lib/screen.mjs).
    response: true,
  },
}
