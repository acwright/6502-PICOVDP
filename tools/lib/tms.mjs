// The stock TMS9918A interface, as Phase 9 drives it through the Nano harness
// (docs/BENCH.md). Only what the bench needs: VRAM, registers and status.
//
// The PICO9918 PRO's stock firmware is a TMS9918A, so it decodes MODE alone and
// ignores MODE1. Port B ($9C02/$9C03) and everything else in SPEC §4 arrives
// with this repo's own firmware in Phase 11.

import { BLOCK_MAX, PORT } from './nano.mjs'

/** Point the chip at `address`, for reading or for writing. */
export async function setAddress(nano, address, write) {
  await nano.write(PORT.ADDRESS, address & 0xff)
  await nano.write(PORT.ADDRESS, ((address >> 8) & 0x3f) | (write ? 0x40 : 0x00))
}

/** Write one of the eight registers. */
export async function setRegister(nano, register, value) {
  await nano.write(PORT.ADDRESS, value & 0xff)
  await nano.write(PORT.ADDRESS, 0x80 | (register & 0x07))
}

export async function status(nano) {
  return nano.read(PORT.ADDRESS)
}

/** Write bytes to VRAM from `address`, using the chip's auto-increment. */
export async function writeVram(nano, address, bytes) {
  await setAddress(nano, address, true)
  const body = Buffer.from(bytes)
  for (let at = 0; at < body.length; at += BLOCK_MAX) {
    await nano.writeBlock(PORT.DATA, body.subarray(at, at + BLOCK_MAX))
  }
}

/** Read `count` bytes of VRAM from `address`. */
export async function readVram(nano, address, count) {
  await setAddress(nano, address, false)
  const out = Buffer.alloc(count)
  for (let at = 0; at < count; at += BLOCK_MAX) {
    const n = Math.min(BLOCK_MAX, count - at)
    ;(await nano.readBlock(PORT.DATA, n)).copy(out, at)
  }
  return out
}

/** The stock firmware is a TMS9918A: 16 KB of VRAM, and the address wraps. */
export const VRAM_SIZE = 0x4000

/** Text mode (Mode 1): 40 x 24 characters, 6 pixels wide, no sprites. */
export const TEXT = {
  name: 0x0000,       // 960 bytes
  pattern: 0x0800,    // 2048 bytes: 256 glyphs of 8 rows
  columns: 40,
  rows: 24,
}

/**
 * Put the chip into text mode. `interrupts` enables the vblank interrupt (R1
 * bit 5), `colour` is foreground in the high nibble, backdrop in the low.
 */
export async function textMode(nano, { interrupts = false, colour = 0xf1 } = {}) {
  await setRegister(nano, 0, 0x00)                                   // M3 = 0
  await setRegister(nano, 1, 0xd0 | (interrupts ? 0x20 : 0x00))      // 16K, display on, M1 = 1
  await setRegister(nano, 2, TEXT.name >> 10)
  await setRegister(nano, 4, TEXT.pattern >> 11)
  await setRegister(nano, 7, colour)
}

/** A solid 6x8 block in the CP437 font: the registration frame's ink. */
export const BLOCK = 0xdb

const LINES = [
  '6502-PICOVDP BENCH - PHASE 9',
  'TEXT THROUGH THE NANO BUS HARNESS',
  '',
  'THE QUICK BROWN FOX JUMPS OVER THE',
  'LAZY DOG.  0123456789',
  '!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~',
  '',
  'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
  'abcdefghijklmnopqrstuvwxyz',
]

/**
 * The Phase 9 test screen: a solid border of BLOCK all the way round, with text
 * inside. The border is what makes the capture measurable -- it puts ink in the
 * outermost row and column, so the picture's bounding box is exactly the 40 x 24
 * text area and the comparison needs no assumption about where the PRO places
 * the active picture inside a 640 x 480 frame.
 */
export function testScreen() {
  const cells = new Uint8Array(TEXT.columns * TEXT.rows).fill(0x20)
  for (let x = 0; x < TEXT.columns; x++) {
    cells[x] = BLOCK
    cells[(TEXT.rows - 1) * TEXT.columns + x] = BLOCK
  }
  for (let y = 1; y < TEXT.rows - 1; y++) {
    cells[y * TEXT.columns] = BLOCK
    cells[y * TEXT.columns + TEXT.columns - 1] = BLOCK
  }
  LINES.forEach((line, i) => {
    const row = 2 + i
    for (let x = 0; x < line.length && x < TEXT.columns - 4; x++) {
      cells[row * TEXT.columns + 2 + x] = line.charCodeAt(x) & 0xff
    }
  })
  return cells
}

/**
 * Render a name table through a pattern table, as text mode puts it on screen:
 * 40 x 24 cells of 6 x 8, so 240 x 192 pixels, one byte per pixel, 0 or 1.
 * Text mode uses the leftmost six bits of each pattern byte.
 */
export function renderText(cells, pattern) {
  const width = TEXT.columns * 6
  const height = TEXT.rows * 8
  const bits = new Uint8Array(width * height)
  for (let row = 0; row < TEXT.rows; row++) {
    for (let column = 0; column < TEXT.columns; column++) {
      const glyph = cells[row * TEXT.columns + column]
      for (let y = 0; y < 8; y++) {
        const byte = pattern[glyph * 8 + y]
        for (let x = 0; x < 6; x++) {
          bits[(row * 8 + y) * width + column * 6 + x] = (byte >> (7 - x)) & 1
        }
      }
    }
  }
  return { width, height, bits }
}
