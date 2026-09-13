'use strict'

/**
 * The core, presented as 6502-EMULATOR's `Video` (PLAN.md section 4).
 *
 * `jest.picovdp.cjs` in the emulator maps `src/core/IO/Video` here and runs
 * `Video.test.ts` unchanged, so this class has that module's public surface and
 * nothing more. The card's behaviour is the C core's, through the Node-API
 * addon; what is JavaScript here is what the core leaves to its platform:
 *
 * - **The raster.** `Video.ts` advances a line every `f / 60 / 262` cycles by an
 *   accumulator, and so does `tick` here, calling `vdp_line_start` with the
 *   screen line that begins. The core is told the line; it never counts time.
 * - **Horizontal blanking**, `STAT3` b1 (§6), from the same accumulator, handed
 *   to the core before every read.
 * - **The frame.** Row S of the 320 × 240 frame is built as screen line S − 1
 *   begins (§3), into a back buffer that is presented when row 239 is built, as
 *   `Video.ts` does. The picture comes from `vdp_build_line` and its colour from
 *   `vdp_expand_line` — the firmware's own 12-bit path, one pixel in two.
 *
 * Phase 2: over the empty core. Every method `Video.test.ts` calls exists; the
 * ones that inspect the card need accessors the core does not have yet, and
 * throw naming the phase that adds them.
 *
 *   PICOVDP_NODE_BINARY   the addon, if not build/host/host/node/picovdp.node
 */

const { join } = require('node:path')

const binary =
  process.env.PICOVDP_NODE_BINARY ?? join(__dirname, '..', '..', 'build', 'host', 'host', 'node', 'picovdp.node')
const core = require(binary)

const DISPLAY_WIDTH = 320
const DISPLAY_HEIGHT = 240
const VIDEO_PALETTE_ENTRIES = 256
const VIDEO_STATUS_COUNT = 16
const VIDEO_REGISTER_COUNT = 128
const VRAM_SIZE = 0x10000

/** §3: 262 screen lines, 60 frames a second, as `Video.ts` times them. */
const SCREEN_LINES = 262
const FRAMES_PER_SECOND = 60

/**
 * Where a cold start puts the raster: display line 0 of the reset geometry
 * (§15, §18), which is Compact's, 24 screen lines down — where `Video.ts` puts it.
 */
const COLD_START_SCREEN_LINE = 24

/** §6: `STAT3` b1 covers the last fifth of each of a display line's two VGA lines. */
const HBLANK_FRACTION = 640 / 800
const VGA_LINES_PER_DISPLAY_LINE = 2

/** 4-bit channel to 8-bit, as `Video.ts` expands its palette. */
const CHANNEL_EXPAND = 0xff / 0x0f

class NotInCore extends Error {
  constructor(method, phase) {
    super(`picovdp: Video.${method} needs the core's ${phase}; it is not there yet`)
    this.name = 'NotInCore'
  }
}

class Video {
  constructor() {
    this.kind = 'video'
    this.card = core.create()

    /** The presented frame, RGBA, as `Video.buffer`. */
    this.buffer = Buffer.alloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4)
    this.frameReady = false

    this.backBuffer = Buffer.alloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4)
    this.indexBuffer = new Uint8Array(DISPLAY_WIDTH * DISPLAY_HEIGHT)
    this.backIndexBuffer = new Uint8Array(DISPLAY_WIDTH * DISPLAY_HEIGHT)
    this.rgbLine = new Uint16Array(DISPLAY_WIDTH * 2)

    this.cycleAccumulator = 0
    this.cyclesPerScanline = 0
    this.screenLine = COLD_START_SCREEN_LINE
    this.beginLine()
  }

  // ---- the bus (§4) ----

  read(address) {
    core.setHblank(this.card, this.horizontalBlanking())
    return core.read(this.card, address & 3)
  }

  write(address, data) {
    core.write(this.card, address & 3, data & 0xff)
  }

  // ---- the raster (§3) ----

  tick(frequency) {
    this.cyclesPerScanline = frequency / FRAMES_PER_SECOND / SCREEN_LINES
    this.cycleAccumulator++
    while (this.cycleAccumulator >= this.cyclesPerScanline) {
      this.cycleAccumulator -= this.cyclesPerScanline
      this.screenLine = this.screenLine + 1 === SCREEN_LINES ? 0 : this.screenLine + 1
      this.beginLine()
    }
    return core.intAsserted(this.card) ? 0x80 : 0
  }

  reset(coldStart) {
    core.reset(this.card, coldStart)
    this.backIndexBuffer.fill(0)
    this.backBuffer.fill(0)
    if (coldStart) {
      this.cycleAccumulator = 0
      this.screenLine = COLD_START_SCREEN_LINE
      this.beginLine()
    }
  }

  beginLine() {
    core.lineStart(this.card, this.screenLine)
    const row = this.screenLine + 1 === SCREEN_LINES ? 0 : this.screenLine + 1
    if (row < DISPLAY_HEIGHT) this.buildRow(row)
  }

  buildRow(row) {
    const start = row * DISPLAY_WIDTH
    const indices = this.backIndexBuffer.subarray(start, start + DISPLAY_WIDTH)
    core.buildLine(this.card, indices)
    core.expandLine(this.card, indices, this.rgbLine)
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const bgr = this.rgbLine[x * 2]
      const offset = (start + x) * 4
      this.backBuffer[offset] = (bgr & 0x0f) * CHANNEL_EXPAND
      this.backBuffer[offset + 1] = ((bgr >> 4) & 0x0f) * CHANNEL_EXPAND
      this.backBuffer[offset + 2] = ((bgr >> 8) & 0x0f) * CHANNEL_EXPAND
      this.backBuffer[offset + 3] = 0xff
    }
    if (row === DISPLAY_HEIGHT - 1) {
      this.backBuffer.copy(this.buffer)
      this.indexBuffer.set(this.backIndexBuffer)
      this.frameReady = true
    }
  }

  horizontalBlanking() {
    if (this.cyclesPerScanline <= 0) return false
    const vgaLine = this.cyclesPerScanline / VGA_LINES_PER_DISPLAY_LINE
    return this.cycleAccumulator % vgaLine >= vgaLine * HBLANK_FRACTION
  }

  frameIndices() {
    return this.indexBuffer
  }

  /** `/INT` (§14). Not `Video.ts`'s — tools/fuzz.mjs compares it after every operation. */
  interruptAsserted() {
    return core.intAsserted(this.card)
  }

  // ---- inspection: accessors the core adds from Phase 3 ----

  get vramSize() {
    return VRAM_SIZE
  }

  readVRAM() {
    throw new NotInCore('readVRAM', 'VRAM (Phase 3)')
  }

  writeVRAM() {
    throw new NotInCore('writeVRAM', 'VRAM (Phase 3)')
  }

  getVramByte() {
    throw new NotInCore('getVramByte', 'VRAM (Phase 3)')
  }

  setVramByte() {
    throw new NotInCore('setVramByte', 'VRAM (Phase 3)')
  }

  getRegister() {
    throw new NotInCore('getRegister', 'register file (Phase 3)')
  }

  setRegister() {
    throw new NotInCore('setRegister', 'register file (Phase 3)')
  }

  paletteBase() {
    throw new NotInCore('paletteBase', 'palette (Phase 3)')
  }

  paletteEntry() {
    throw new NotInCore('paletteEntry', 'palette (Phase 3)')
  }

  portState() {
    throw new NotInCore('portState', 'ports (Phase 3)')
  }

  getStatus() {
    throw new NotInCore('getStatus', 'status registers (Phase 4)')
  }

  peekStatus() {
    throw new NotInCore('peekStatus', 'status registers (Phase 4)')
  }

  getDisplayLine() {
    throw new NotInCore('getDisplayLine', 'line timing (Phase 4)')
  }

  getMode() {
    throw new NotInCore('getMode', 'geometry table (Phase 5)')
  }

  isDisplayEnabled() {
    throw new NotInCore('isDisplayEnabled', 'geometry table (Phase 5)')
  }

  textGrid() {
    throw new NotInCore('textGrid', 'geometry table (Phase 5)')
  }

  // Snapshots are the emulator's, and not in PLAN.md section 4's surface; three
  // tests use them. Phase 3 decides whether the core carries its state out or
  // those tests are skipped by name.
  serialize() {
    throw new NotInCore('serialize', 'snapshot state (undecided)')
  }

  deserialize() {
    throw new NotInCore('deserialize', 'snapshot state (undecided)')
  }
}

module.exports = {
  Video,
  NotInCore,
  DISPLAY_WIDTH,
  DISPLAY_HEIGHT,
  VIDEO_PALETTE_ENTRIES,
  VIDEO_STATUS_COUNT,
  VIDEO_REGISTER_COUNT
}
