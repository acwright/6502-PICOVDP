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
 * - **Snapshots.** `Video.ts`'s format, filled from `vdp_debug_save` and read
 *   back through `vdp_debug_restore`.
 * - **`tickCount` and `observer`**, as `Video.ts` has them, so a trace recorder
 *   (tools/lib/trace.mjs) can watch the core as the emulator's watches `Video.ts`.
 *
 * Phase 3: ports, registers, VRAM, palette, and the debugger's view of them.
 * Phase 4: the display line, status and interrupts.
 * Phase 5: the picture, through the tile engine.
 * Phase 6: sprites. With PICOVDP_SPLIT_CHECK set, every row is built a second
 * time with the two cores' division at another column, and a row or status
 * that differs throws (PLAN.md section 3: "the reference check builds each line
 * both ways"). Publishing a line's collisions twice changes nothing.
 *
 *   PICOVDP_NODE_BINARY   the addon, if not build/host/host/node/picovdp.node
 *   PICOVDP_SPLIT_CHECK   build every row at a second split, and compare
 */

const { join } = require('node:path')
const CP437 = require('./cp437.cjs')

const binary =
  process.env.PICOVDP_NODE_BINARY ?? join(__dirname, '..', '..', 'build', 'host', 'host', 'node', 'picovdp.node')
const core = require(binary)

const SPLIT_CHECK = Boolean(process.env.PICOVDP_SPLIT_CHECK)

const DISPLAY_WIDTH = 320
const DISPLAY_HEIGHT = 240
const VIDEO_PALETTE_ENTRIES = 256
const VIDEO_STATUS_COUNT = 16
const VIDEO_REGISTER_COUNT = 128
const VRAM_SIZE = 0x10000
const VRAM_MASK = VRAM_SIZE - 1

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

/**
 * `STAT5` (§6). The firmware reports its own version; the adapter reports the
 * spec revision, `$04`, as `Video.ts` does (PLAN.md section 4's known differences).
 */
const STAT5_SPEC_REVISION = 0x04

/** §5: `PALBASE`, and the register numbers `getMode`'s neighbours read. */
const REG_COLOR = 0x07
const REG_PALBASE = 0x0c
const REG_L0NAME = 0x10
const REG_L0PAL = 0x16

/** `vdp_debug_mode_t`'s codes, as `Video.ts` names them (§9). */
const LEGACY_NAMES = [null, 'text', 'graphics-i', 'graphics-ii', 'multicolor']
const GEOMETRY_NAMES = ['text', 'compact', 'graphics', 'full']

class Video {
  constructor() {
    this.kind = 'video'
    this.card = core.create(STAT5_SPEC_REVISION)

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

    /** Ticks since the card was made or last cold-started: the clock a trace is timed by. */
    this.tickCount = 0
    /** Told of every port access, line start and reset, as `Video.ts`'s `VideoObserver` is. */
    this.observer = undefined

    this.beginLine()
  }

  // ---- the bus (§4) ----

  read(address) {
    core.setHblank(this.card, this.horizontalBlanking())
    const value = core.read(this.card, address & 3)
    if (this.observer) this.observer.read(address & 3, value)
    return value
  }

  write(address, data) {
    core.write(this.card, address & 3, data & 0xff)
    if (this.observer) this.observer.write(address & 3, data)
  }

  // ---- the raster (§3) ----

  tick(frequency) {
    this.cyclesPerScanline = frequency / FRAMES_PER_SECOND / SCREEN_LINES
    this.tickCount++
    this.cycleAccumulator++
    while (this.cycleAccumulator >= this.cyclesPerScanline) {
      this.cycleAccumulator -= this.cyclesPerScanline
      this.screenLine = this.screenLine + 1 === SCREEN_LINES ? 0 : this.screenLine + 1
      this.beginLine()
      if (this.observer) this.observer.lineStart(this.screenLine, core.displayLine(this.card))
    }
    return core.intAsserted(this.card) ? 0x80 : 0
  }

  reset(coldStart) {
    core.reset(this.card, coldStart)
    // `Video.ts` starts the frame in progress from the backdrop as reset leaves it.
    this.fillBackground()
    if (coldStart) {
      this.cycleAccumulator = 0
      this.tickCount = 0
      this.screenLine = COLD_START_SCREEN_LINE
      this.beginLine()
    }
    // A cold start's line start is reported here, not as a lineStart.
    if (this.observer) this.observer.reset(coldStart, this.screenLine)
  }

  beginLine() {
    core.lineStart(this.card, this.screenLine)
    const row = this.screenLine + 1 === SCREEN_LINES ? 0 : this.screenLine + 1
    if (row < DISPLAY_HEIGHT) this.buildRow(row)
  }

  buildRow(row) {
    const start = row * DISPLAY_WIDTH
    const indices = this.backIndexBuffer.subarray(start, start + DISPLAY_WIDTH)
    if (SPLIT_CHECK) this.buildRowTwice(row, indices)
    else core.buildLine(this.card, indices)
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

  /**
   * The row at the split the core chooses, then again at the next of the
   * 32-pixel columns 0-320 in turn; both rows, and STAT0 and the collision map
   * after each, must agree.
   */
  buildRowTwice(row, indices) {
    const chosen = core.splitChoose(this.card)
    core.buildLineAt(this.card, indices, chosen)
    const status = () => [0, 8, 9, 10, 11, 12, 13, 14, 15].map((select) => core.status(this.card, select)).join()
    const first = { row: Uint8Array.from(indices), status: status(), interrupt: core.intAsserted(this.card) }
    this.splitTurn = ((this.splitTurn ?? 0) + 1) % 11
    const other = this.splitTurn * 32
    core.buildLineAt(this.card, indices, other)
    const x = indices.findIndex((index, at) => index !== first.row[at])
    if (x >= 0 || status() !== first.status || core.intAsserted(this.card) !== first.interrupt) {
      throw new Error(
        `picovdp: row ${row} differs split at ${other} from at ${chosen}` +
          (x >= 0 ? `, first at x ${x}: ${indices[x]}, not ${first.row[x]}` : `, in status ${status()}, not ${first.status}`)
      )
    }
  }

  /** The back buffers filled with the backdrop (§11), for a reset or a restore. */
  fillBackground() {
    const index = ((this.getRegister(REG_L0PAL) & 0x0f) << 4) | (this.getRegister(REG_COLOR) & 0x0f)
    const rgb = this.paletteEntry(index)
    const red = ((rgb >> 8) & 0x0f) * CHANNEL_EXPAND
    const green = ((rgb >> 4) & 0x0f) * CHANNEL_EXPAND
    const blue = (rgb & 0x0f) * CHANNEL_EXPAND
    for (let offset = 0; offset < this.backBuffer.length; offset += 4) {
      this.backBuffer[offset] = red
      this.backBuffer[offset + 1] = green
      this.backBuffer[offset + 2] = blue
      this.backBuffer[offset + 3] = 0xff
    }
    this.backIndexBuffer.fill(index)
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

  // ---- inspection (vdp_debug.h) ----

  get vramSize() {
    return VRAM_SIZE
  }

  readVRAM(offset) {
    return core.getVram(this.card, offset & VRAM_MASK)
  }

  writeVRAM(offset, value) {
    core.setVram(this.card, offset & VRAM_MASK, value & 0xff)
  }

  getVramByte(address) {
    return core.getVram(this.card, address & VRAM_MASK)
  }

  setVramByte(address, value) {
    core.setVram(this.card, address & VRAM_MASK, value & 0xff)
  }

  getRegister(index) {
    return core.getRegister(this.card, index & 0x7f)
  }

  setRegister(index, value) {
    core.setRegister(this.card, index & 0x7f, value & 0xff)
  }

  /** The palette window's first byte, `PALBASE` × `$400` (§11). */
  paletteBase() {
    return (this.getRegister(REG_PALBASE) & 0x3f) << 10
  }

  /** An entry as 12-bit `$RGB`, as the next line built draws it (§11). */
  paletteEntry(index) {
    return core.paletteEntry(this.card, index & 0xff)
  }

  portState(which) {
    const { pointer, readMode, readAhead, awaitingCommand, payload } = core.portState(this.card, which === 'a' ? 0 : 1)
    return { pointer, readMode, readAhead, awaitingCommand, payload }
  }

  getMode() {
    const mode = core.mode(this.card)
    return {
      vmode: mode.vmode,
      legacy: LEGACY_NAMES[mode.legacy],
      geometry: GEOMETRY_NAMES[mode.geometry],
      cols: mode.cols,
      rows: mode.rows,
      cellWidth: mode.cellWidth,
      width: mode.width,
      lines: mode.lines,
      originX: mode.originX,
      originY: mode.originY
    }
  }

  isDisplayEnabled() {
    return core.mode(this.card).display
  }

  /** Layer 0's name table as CP437 text, one string per cell row (§9). */
  textGrid() {
    const { cols, rows } = core.mode(this.card)
    const base = (this.getRegister(REG_L0NAME) << 10) & VRAM_MASK
    const lines = []
    for (let row = 0; row < rows; row++) {
      let line = ''
      for (let col = 0; col < cols; col++) line += CP437[core.getVram(this.card, (base + row * cols + col) & VRAM_MASK)]
      lines.push(line)
    }
    return lines
  }

  /** `STAT0`, without acknowledging it (§6). */
  getStatus() {
    return core.status(this.card, 0)
  }

  /** A status register as a program selecting it would read it, without acknowledging (§6). */
  peekStatus(select) {
    core.setHblank(this.card, this.horizontalBlanking())
    return core.status(this.card, select & 0x0f)
  }

  /** The display line being scanned, 0 – 261 (§3). */
  getDisplayLine() {
    return core.displayLine(this.card)
  }

  // ---- snapshots, in Video.ts's format ----

  serialize() {
    const vram = new Uint8Array(VRAM_SIZE)
    const saved = core.save(this.card, vram)
    return {
      kind: this.kind,
      registers: Buffer.from(saved.registers).toString('base64'),
      stat0: saved.stat0,
      irqLatch: saved.irqLatch,
      overflowSprite: saved.overflowSprite,
      collisionMap: Buffer.from(saved.collisionMap).toString('base64'),
      ports: saved.ports.map((port) => ({ kind: 'video-port', ...port })),
      vram: Buffer.from(vram).toString('base64'),
      frameEvents: saved.frameEvents,
      cycleAccumulator: this.cycleAccumulator,
      screenLine: saved.screenLine,
      displayLine: saved.displayLine,
      frameReady: this.frameReady
    }
  }

  deserialize(state) {
    if (state?.kind !== this.kind) throw new Error(`picovdp: expected a '${this.kind}' state, got '${state?.kind}'`)
    const bytes = (name, length) => {
      const decoded = Buffer.from(state[name], 'base64')
      if (decoded.length !== length) throw new Error(`picovdp: ${name} is ${decoded.length} bytes, not ${length}`)
      return new Uint8Array(decoded)
    }
    // Required as `Video.ts` requires them; only `screenLine` may be absent.
    const number = (name) => {
      if (typeof state[name] !== 'number' || !Number.isFinite(state[name])) {
        throw new Error(`picovdp: ${name} is ${JSON.stringify(state[name])}, not a number`)
      }
      return state[name]
    }
    const snapshot = {
      registers: bytes('registers', VIDEO_REGISTER_COUNT),
      stat0: number('stat0') & 0xff,
      irqLatch: number('irqLatch') & 0x0f,
      overflowSprite: number('overflowSprite') & 0x3f,
      frameEvents: number('frameEvents') & 0x0f,
      collisionMap: bytes('collisionMap', 8),
      displayLine: number('displayLine') % SCREEN_LINES,
      ports: state.ports.map((port) => ({
        pointer: port.pointer & VRAM_MASK,
        readMode: Boolean(port.readMode),
        readAhead: port.readAhead & 0xff,
        stage: port.stage & 1,
        payload: port.payload & 0xff
      })),
      screenLine: typeof state.screenLine === 'number' ? state.screenLine : 0
    }
    const vram = bytes('vram', VRAM_SIZE)
    core.restore(this.card, snapshot, vram)
    // A snapshot from before screen lines were counted has only the display
    // line, which the restored geometry's top border turns back into one (§3).
    if (typeof state.screenLine !== 'number') {
      snapshot.screenLine = (snapshot.displayLine + core.mode(this.card).originY) % SCREEN_LINES
      core.restore(this.card, snapshot, vram)
    }
    this.screenLine = snapshot.screenLine % SCREEN_LINES
    this.cycleAccumulator = number('cycleAccumulator')
    this.frameReady = Boolean(state.frameReady)
    this.fillBackground()
  }
}

module.exports = {
  Video,
  DISPLAY_WIDTH,
  DISPLAY_HEIGHT,
  VIDEO_PALETTE_ENTRIES,
  VIDEO_STATUS_COUNT,
  VIDEO_REGISTER_COUNT
}
