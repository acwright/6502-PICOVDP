#!/usr/bin/env node

// The card's built-in font, fonts/cp437-6x8.bin (SPEC.md §7, "The built-in font").
//
//   node tools/font.mjs extract <Chars.asm | -> [out.bin]   derive the font from 6502-BIOS's Chars.asm
//   node tools/font.mjs --check                             check the pinned file (CTest font_pinned)
//   node tools/font.mjs show [first[-last]]                 draw glyphs as 6 × 8 text, for review
//
// The font is font $00: CP437, 256 glyphs of 8 rows, glyph n at offset n × 8,
// bit 7 the leftmost pixel. Its bytes are exactly the character set 6502-BIOS
// v1.6 uploads to VRAM $0800, and its SHA-256 is normative. `extract` is how
// the file was made, kept runnable:
//
//   git -C ../../Assembly/6502-BIOS show v1.6:Chars.asm | node tools/font.mjs extract -
//
// --check needs no BIOS checkout: size, SHA-256, and bits 1:0 clear in every
// row, so each glyph fits a 6-pixel cell. If a 6502-BIOS checkout is found
// (PICOVDP_BIOS, or ../../Assembly/6502-BIOS from this repository), it also
// checks that v1.6:Chars.asm still derives the file and that v1.6:BIOS.bin holds
// it at $B800.
//
// `first` and `last` take $41, 0x41 or 65. With neither, every glyph.

import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const FONT = join(REPO, 'fonts', 'cp437-6x8.bin')
const BIOS = resolve(process.env.PICOVDP_BIOS ?? join(REPO, '..', '..', 'Assembly', '6502-BIOS'))

/** §7: 256 glyphs × 8 rows. */
const GLYPHS = 256
const ROWS = 8
const FONT_BYTES = GLYPHS * ROWS
/** Normative (§7). */
const FONT_SHA256 = 'b2adc19efd10870196bad05d84eae51500599935c80f13d608a4f62278260577'
/** The BIOS the font is taken from, and where its ROM image holds it: $B800 in a ROM at $8000. */
const BIOS_TAG = 'v1.6'
const BIOS_BIN_OFFSET = 0x3800

function main() {
  const [command, ...args] = process.argv.slice(2)
  if (command === '--check' && args.length === 0) return check()
  if (command === 'extract' && (args.length === 1 || args.length === 2)) return extract(args[0], args[1])
  if (command === 'show' && args.length <= 1) return show(args[0])
  fail('usage: font.mjs extract <Chars.asm | -> [out.bin] | --check | show [first[-last]]')
}

/** Chars.asm's `.byte` rows, in order: 256 of them, 8 bytes each. */
function parseCharsAsm(source) {
  const bytes = []
  source.split(/\r?\n/).forEach((line, index) => {
    const code = line.replace(/;.*$/, '').trim()
    if (!/^\.byte\b/i.test(code)) return
    const values = code.slice('.byte'.length).split(',').map((value) => value.trim())
    if (values.length !== ROWS) throw new Error(`line ${index + 1}: ${values.length} bytes, not ${ROWS}`)
    for (const value of values) {
      const match = /^\$([0-9a-f]{1,2})$/i.exec(value)
      if (!match) throw new Error(`line ${index + 1}: '${value}' is not a $hex byte`)
      bytes.push(parseInt(match[1], 16))
    }
  })
  if (bytes.length !== FONT_BYTES) throw new Error(`${bytes.length / ROWS} glyphs, not ${GLYPHS}`)
  return Buffer.from(bytes)
}

function extract(input, output = FONT) {
  let font
  try {
    font = parseCharsAsm(readFileSync(input === '-' ? 0 : input, 'utf8'))
  } catch (error) {
    fail(`${input === '-' ? 'standard input' : input}: ${error.message}`)
  }
  mkdirSync(dirname(resolve(output)), { recursive: true })
  writeFileSync(output, font)
  console.log(`Wrote ${relative(process.cwd(), resolve(output)) || output}: ${font.length} bytes, SHA-256 ${sha256(font)}.`)
  if (sha256(font) !== FONT_SHA256) console.log(`That is not font $00 (SHA-256 ${FONT_SHA256}).`)
}

function check() {
  if (!existsSync(FONT)) fail(`no ${relative(REPO, FONT)}`)
  const font = readFileSync(FONT)
  const problems = []
  if (font.length !== FONT_BYTES) problems.push(`it is ${font.length} bytes, not ${FONT_BYTES}`)
  if (sha256(font) !== FONT_SHA256) problems.push(`its SHA-256 is ${sha256(font)}, not ${FONT_SHA256}`)
  const wide = [...font.keys()].filter((offset) => font[offset] & 0x03)
  if (wide.length) {
    const at = wide.slice(0, 8).map((offset) => `glyph ${hex(offset >> 3)} row ${offset & 7}`)
    problems.push(`${wide.length} row(s) set bit 1 or bit 0, outside the 6-pixel cell: ${at.join(', ')}`)
  }

  let derivation = `no 6502-BIOS checkout at ${BIOS} (set PICOVDP_BIOS), so its derivation was not checked`
  if (existsSync(join(BIOS, '.git'))) {
    try {
      const commit = biosGit('rev-parse', `${BIOS_TAG}^{commit}`).toString().trim()
      const derived = parseCharsAsm(biosGit('show', `${BIOS_TAG}:Chars.asm`).toString('utf8'))
      if (!derived.equals(font)) problems.push(`6502-BIOS ${BIOS_TAG}:Chars.asm derives different bytes`)
      const rom = biosGit('show', `${BIOS_TAG}:BIOS.bin`)
      if (!rom.subarray(BIOS_BIN_OFFSET, BIOS_BIN_OFFSET + FONT_BYTES).equals(font)) {
        problems.push(`6502-BIOS ${BIOS_TAG}:BIOS.bin does not hold it at offset $${BIOS_BIN_OFFSET.toString(16).toUpperCase()}`)
      }
      derivation = `derived from 6502-BIOS ${BIOS_TAG} (${commit.slice(0, 7)}) Chars.asm, and in its BIOS.bin at $B800`
    } catch (error) {
      problems.push(`6502-BIOS at ${BIOS}: ${error.message.split('\n')[0]}`)
    }
  }

  if (problems.length) fail(`${relative(REPO, FONT)} is not font $00:\n  ${problems.join('\n  ')}`)
  console.log(`${relative(REPO, FONT)}: font $00, ${FONT_BYTES} bytes, SHA-256 ${FONT_SHA256.slice(0, 12)}…; ${derivation}.`)
}

function show(range) {
  if (!existsSync(FONT)) fail(`no ${relative(REPO, FONT)}`)
  const font = readFileSync(FONT)
  let first = 0
  let last = GLYPHS - 1
  if (range !== undefined) {
    const [from, to = from] = range.split('-')
    first = code(from)
    last = code(to)
    if (last < first) fail(`${range} is an empty range`)
  }

  // Eight glyphs to a band, each drawn from bits 7:2 of its rows.
  const bands = []
  for (let start = first; start <= last; start += 8) {
    const codes = []
    for (let n = start; n <= Math.min(last, start + 7); n++) codes.push(n)
    const lines = [codes.map((n) => hex(n).padEnd(6)).join('  ').trimEnd()]
    for (let row = 0; row < ROWS; row++) {
      lines.push(
        codes
          .map((n) => {
            let pixels = ''
            for (let bit = 7; bit >= 2; bit--) pixels += font[n * ROWS + row] & (1 << bit) ? '#' : '.'
            return pixels
          })
          .join('  ')
      )
    }
    bands.push(lines.join('\n'))
  }
  console.log(bands.join('\n\n'))
}

function code(text) {
  const match = /^(?:\$|0x)([0-9a-f]+)$/i.exec(text)
  const value = match ? parseInt(match[1], 16) : /^\d+$/.test(text) ? Number(text) : NaN
  if (!(value >= 0 && value < GLYPHS)) fail(`'${text}' is not a character code ($00–$FF)`)
  return value
}

function biosGit(...args) {
  return execFileSync('git', ['-C', BIOS, ...args], { stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 1 << 20 })
}

function hex(value) {
  return `$${value.toString(16).toUpperCase().padStart(2, '0')}`
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}

function fail(message) {
  console.error(`font: ${message}`)
  process.exit(1)
}

main()
