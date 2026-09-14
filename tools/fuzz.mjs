#!/usr/bin/env node

// Fuzz the core against Video.ts (PLAN.md section 4).
//
//   node tools/fuzz.mjs [--seed N] [--ops N] [--check-every N] [--scope bus|status|tiles|all] [--out DIR] [--self]
//
// Loads the emulator's compiled Video and the adapter (host/node/Video.cjs, or
// PICOVDP_ADDON) into one process, feeds both the same seeded stream of port
// operations and ticks — biased toward the registers, addresses and values that
// mean something — and compares them. The first divergence prints the seed, is
// minimised to the shortest operation list that still diverges, and is written
// to DIR (build/fuzz) as that list and, when a trace can hold it, as a
// docs/TRACE.md trace recorded from Video.ts, ready to become a unit test.
//
// What is compared is the scope, which grows with the core:
//
//   bus   Phase 3: §4, §5, §7, §11. Every data-port read, and every status read
//         of the constant STAT4-STAT6; every N operations, all 128 registers,
//         both port pairs, all 256 palette entries and all 64 KB of VRAM. The
//         stream adds a debugger's register writes and VRAM pokes. Status
//         values, /INT and frames are not compared: they are Phases 4 and 5's.
//   status
//         Phase 4: the bus scope's stream and state, plus §3, §6, §14. Every
//         read, status included; /INT after every operation and every tick;
//         and every N operations all sixteen status registers, peeked, and the
//         display line. Frames are not compared. From Phase 6 sprites run, so
//         overflow, collision, STAT7 and the collision map are compared too.
//   tiles Phase 5: the status scope, plus §8 at 1bpp, §9 and the backdrop,
//         and from Phase 6 §10's sprites and §12 against 1bpp layers. Every
//         frame either card presents: both must present it after the same
//         tick, and its 76,800 indices must agree. Both layers are held at
//         1bpp — whenever an operation leaves Video.ts's L0CTRL or L1CTRL b1:0
//         non-zero, both cards have them cleared — because 2, 4 and 8bpp are
//         Phase 7's. The legacy submode ignores L0CTRL's depth, so it runs
//         unheld. Sprites run at every depth.
//   all   every read, /INT after every operation and every tick, and the frame
//         every N operations.
//
// --self fuzzes Video.ts against itself: the harness's own check, which must
// never diverge.

import { mkdirSync, writeFileSync } from 'node:fs'
import { join, relative } from 'node:path'
import { REPO, loadAdapter, loadVideo } from './lib/emulator.mjs'
import { TraceRecorder, writeTrace } from './lib/trace.mjs'

const FREQUENCY = 1_000_000
const TICKS_PER_FRAME = FREQUENCY / 60

function main() {
  const options = parseArgs(process.argv.slice(2))
  const Reference = loadVideo()
  const Candidate = options.self ? Reference : loadAdapter()
  const make = () => ({ a: new Reference(), b: new Candidate() })

  const ops = generate(options.seed, options.ops, options.scope)
  console.log(
    `fuzz: seed ${options.seed}, ${ops.length} operations, scope ${options.scope}, Video.ts against ${options.self ? 'itself' : 'the core'}`
  )
  console.log(`stream: ${composition(ops)}`)
  const started = Date.now()
  const found = run(make, ops, options)
  if (options.scope === 'status' || options.scope === 'tiles') {
    console.log(`sprites: Video.ts's OVF set ${held.overflow} time(s), COL ${held.collision}, the collision map grew ${held.map}`)
  }
  if (options.scope === 'tiles') console.log(`layers held at 1bpp ${held.depth} time(s); ${held.frames} frames compared, ${held.drawn} of them more than one colour`)
  if (!found) {
    console.log(`no divergence in ${((Date.now() - started) / 1000).toFixed(1)}s`)
    return
  }

  console.log(`DIVERGED at operation ${found.index}: ${found.what}`)
  const minimal = minimise(make, ops.slice(0, found.index + 1), options)
  const last = run(make, minimal, options)
  console.log(`minimised to ${minimal.length} operation(s): ${last.what}`)

  mkdirSync(options.out, { recursive: true })
  const base = join(options.out, `fuzz-${options.seed}`)
  writeFileSync(
    `${base}.json`,
    JSON.stringify({ seed: options.seed, scope: options.scope, diverged: last.what, ops: minimal }, null, 1) + '\n'
  )
  held.depth = 0
  run(make, minimal, options)
  if (minimal.some((op) => op.x !== undefined || op.p !== undefined || op.g !== undefined) || held.depth) {
    console.log(`wrote ${relative(REPO, base)}.json (it resets, pokes or holds a depth, which a version 1 trace cannot hold)`)
  } else {
    writeFileSync(`${base}.vdpt.gz`, record(Reference, minimal, `fuzz-${options.seed}`))
    console.log(`wrote ${relative(REPO, base)}.json and .vdpt.gz`)
  }
  process.exitCode = 1
}

function parseArgs(args) {
  const options = { seed: 1, ops: 100_000, checkEvery: 5_000, scope: 'all', out: join(REPO, 'build', 'fuzz'), self: false }
  for (let i = 0; i < args.length; i++) {
    const value = () => args[++i] ?? usage()
    switch (args[i]) {
      case '--seed': options.seed = Number(value()) >>> 0; break
      case '--ops': options.ops = Number(value()); break
      case '--check-every': options.checkEvery = Number(value()); break
      case '--scope': options.scope = value(); if (!SCOPES.includes(options.scope)) usage(); break
      case '--out': options.out = value(); break
      case '--self': options.self = true; break
      default: usage()
    }
  }
  return options
}

function usage() {
  console.error('usage: fuzz.mjs [--seed N] [--ops N] [--check-every N] [--scope bus|status|tiles|all] [--out DIR] [--self]')
  process.exit(2)
}

const SCOPES = ['bus', 'status', 'tiles', 'all']

// ---- the stream ----

/** mulberry32: small, seeded, and the same in every Node. */
function random(seed) {
  let state = seed >>> 0
  return () => {
    state = (state + 0x6d2b79f5) >>> 0
    let t = state
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

/** Registers worth hitting often (§5): the legacy core, access and interrupts, layers, sprites. */
const REGISTERS = [0, 1, 2, 3, 4, 5, 6, 7, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18, 0x1b, 0x1c, 0x1d, 0x1e, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25]
const VALUES = [0x00, 0x01, 0x02, 0x03, 0x04, 0x0f, 0x10, 0x20, 0x3f, 0x40, 0x7f, 0x80, 0xc0, 0xd0, 0xe0, 0xff]
/** Addresses worth pointing at (§7): the reset tables, the palette window, the ends of VRAM. */
const ADDRESSES = [0x0000, 0x0400, 0x0800, 0x1000, 0x1b00, 0x3800, 0x3ffe, 0x4000, 0x7fff, 0xfbfe, 0xfc00, 0xfdff, 0xfffe]

/**
 * Primitive operations: `{ w: port, v }`, `{ r: port }`, `{ t: ticks }`,
 * `{ x: cold }`, and in the bus scope a debugger's `{ p: address, v }` (VRAM) and
 * `{ g: register, v }`. Each kind of thing a program does becomes a few of them.
 */
function generate(seed, count, scope) {
  const next = random(seed)
  const pick = (list) => list[Math.floor(next() * list.length)]
  const byte = () => (next() < 0.6 ? pick(VALUES) : Math.floor(next() * 256))
  const port = () => (next() < 0.8 ? 0 : 2) // port A mostly, B too
  const ops = []
  // The status and tiles scopes add sprite scenes to the stream. The bus and
  // all scopes draw no extra numbers, so their streams are as they were.
  const scenes = scope === 'status' || scope === 'tiles'

  while (ops.length < count) {
    if (scenes && next() < SCENE_CHANCE) {
      spriteScene(ops, next, pick, byte, port())
      continue
    }
    const roll = next()
    const p = port()
    if (roll < 0.25) {
      const register = next() < 0.9 ? pick(REGISTERS) : Math.floor(next() * 128)
      ops.push({ w: p | 1, v: byte() }, { w: p | 1, v: 0x80 | register })
    } else if (roll < 0.4) {
      const address = next() < 0.7 ? pick(ADDRESSES) + Math.floor(next() * 64) : Math.floor(next() * 0x10000)
      const bank = (address >> 14) & 3
      ops.push({ w: p | 1, v: bank }, { w: p | 1, v: 0x88 }) // VBANK
      ops.push({ w: p | 1, v: address & 0xff }, { w: p | 1, v: ((address >> 8) & 0x3f) | (next() < 0.6 ? 0x40 : 0) })
    } else if (roll < 0.62) {
      for (let n = 1 + Math.floor(next() * 16); n > 0; n--) ops.push({ w: p, v: byte() })
    } else if (roll < 0.72) {
      for (let n = 1 + Math.floor(next() * 8); n > 0; n--) ops.push({ r: p })
    } else if (roll < 0.82) {
      if (next() < 0.5) ops.push({ w: p | 1, v: Math.floor(next() * 16) }, { w: p | 1, v: p ? 0x8e : 0x8f })
      ops.push({ r: p | 1 })
    } else if (roll < 0.99) {
      if (scope !== 'all' && next() < 0.2) {
        if (next() < 0.5) {
          const address = next() < 0.7 ? pick(ADDRESSES) + Math.floor(next() * 600) : Math.floor(next() * 0x10000)
          ops.push({ p: address & 0xffff, v: byte() })
        } else {
          ops.push({ g: next() < 0.9 ? pick(REGISTERS) : Math.floor(next() * 128), v: byte() })
        }
        continue
      }
      ops.push({ t: next() < 0.95 ? 1 + Math.floor(next() * 200) : Math.floor(next() * TICKS_PER_FRAME) })
    } else if (roll < 0.998) {
      ops.push({ w: p | 1, v: byte() }) // half a command pair
    } else {
      ops.push({ x: next() < 0.2 })
    }
  }
  return ops.slice(0, count)
}

/** How often, in the status and tiles scopes, a sprite scene is written. */
const SCENE_CHANCE = 0.02

/** §5: the registers a sprite scene sets. */
const MODE1 = 0x01
const SPRATTR = 0x20
const SPRPAT = 0x21

/**
 * A sprite scene (§10), as a program writes one: the display on, perhaps a
 * sprite size and magnification, a new attribute or pattern table base, and
 * slots and patterns written through the data port. Y is biased onto the
 * picture and its edges and patterns toward solid, so that sprites cover lines,
 * overlap, collide and overflow; everything else is left to the stream.
 */
function spriteScene(ops, next, pick, byte, p) {
  const register = (index, value) => ops.push({ w: p | 1, v: value }, { w: p | 1, v: 0x80 | index })
  const pointAt = (address) => {
    register(0x08, (address >> 14) & 3)
    ops.push({ w: p | 1, v: address & 0xff }, { w: p | 1, v: ((address >> 8) & 0x3f) | 0x40 })
  }
  if (next() < 0.5) {
    // DISP, and sometimes IE, M1 (legacy Text, no sprites), size and magnification.
    register(MODE1, 0x40 | (next() < 0.3 ? 0x20 : 0) | (next() < 0.1 ? 0x10 : 0) | Math.floor(next() * 4))
  }
  if (next() < 0.3) register(SPRATTR, next() < 0.5 ? 0 : byte())
  if (next() < 0.3) register(SPRPAT, next() < 0.5 ? 0 : byte())

  if (next() < 0.7) {
    // Slots from one at or near the start of a table, at whichever base SPRATTR holds.
    const base = next() < 0.5 ? 0 : pick(SCENE_BASES)
    pointAt(base + 4 * Math.floor(next() * 8))
    for (let n = 1 + Math.floor(next() * 40); n > 0; n--) {
      const y = next() < 0.8 ? Math.floor(next() * 240) : pick([0xd0, 0xe0, 0xe1, 0xf0, 0xf1, 0xff, 191, 192, 239, 240])
      const x = next() < 0.8 ? Math.floor(next() * 256) : pick([0x00, 0x08, 0xf8, 0xff])
      const pattern = next() < 0.7 ? Math.floor(next() * 8) : byte()
      ops.push({ w: p, v: y }, { w: p, v: x }, { w: p, v: pattern }, { w: p, v: byte() })
    }
  } else {
    const base = next() < 0.5 ? 0 : pick(SCENE_BASES)
    pointAt(base + Math.floor(next() * 256))
    for (let n = 8 + Math.floor(next() * 248); n > 0; n--) ops.push({ w: p, v: next() < 0.6 ? 0xff : byte() })
  }
}

/** Where scenes write, besides $0000: sprite table bases a register value names. */
const SCENE_BASES = [0x0080, 0x0800, 0x1000, 0x2000, 0x3800, 0x4000, 0x7f80, 0xf800]

// ---- running ----

const interrupt = (video) =>
  typeof video.interruptAsserted === 'function' ? video.interruptAsserted() : video.peekStatus(1) !== 0

const describe = (op) =>
  op.w !== undefined ? `write $${hex(op.v)} to port ${op.w}`
    : op.r !== undefined ? `read port ${op.r}`
      : op.t !== undefined ? `tick ${op.t}`
        : op.p !== undefined ? `poke $${hex(op.v)} into VRAM $${op.p.toString(16).padStart(4, '0')}`
          : op.g !== undefined ? `set register $${hex(op.g)} to $${hex(op.v)}`
            : `${op.x ? 'cold' : 'warm'} reset`

/** §6: the status registers that are constants, which the bus scope compares. */
const CONSTANT_STATUS = [4, 5, 6]

/** The first difference in status (§3, §6), or null. Peeked, so nothing is acknowledged. */
function statusDifference(a, b) {
  if (a.getDisplayLine() !== b.getDisplayLine()) {
    return `display line is ${b.getDisplayLine()}, Video.ts's ${a.getDisplayLine()}`
  }
  for (let select = 0; select < 16; select++) {
    if (a.peekStatus(select) !== b.peekStatus(select)) {
      return `STAT${select} peeks $${hex(b.peekStatus(select))}, Video.ts's $${hex(a.peekStatus(select))}`
    }
  }
  return null
}

/** §5: LxCTRL, and its bit depth (§8). */
const LXCTRL = [0x15, 0x1d]
const LXCTRL_DEPTH = 0x03

/**
 * For the log: how often the tiles scope held the depth and the frames it
 * compared, and how often Video.ts's OVF and COL were set and its collision
 * map grew — that the stream reaches the sprites at all.
 */
const held = { depth: 0, frames: 0, drawn: 0, overflow: 0, collision: 0, map: 0 }

/** §6: STAT0's sprite flags. */
const STAT0_OVF = 0x40
const STAT0_COL = 0x20

/** The first difference in the card's bus-side state (§4, §5, §7, §11), or null. */
function stateDifference(a, b) {
  for (let register = 0; register < 128; register++) {
    if (a.getRegister(register) !== b.getRegister(register)) {
      return `register $${hex(register)} is $${hex(b.getRegister(register))}, Video.ts's $${hex(a.getRegister(register))}`
    }
  }
  for (const pair of ['a', 'b']) {
    const expected = JSON.stringify(a.portState(pair))
    const actual = JSON.stringify(b.portState(pair))
    if (expected !== actual) return `port ${pair.toUpperCase()} is ${actual}, Video.ts's ${expected}`
  }
  for (let entry = 0; entry < 256; entry++) {
    if (a.paletteEntry(entry) !== b.paletteEntry(entry)) {
      return `palette entry ${entry} is $${b.paletteEntry(entry).toString(16)}, Video.ts's $${a.paletteEntry(entry).toString(16)}`
    }
  }
  const vram = b.serialize ? Buffer.from(b.serialize().vram, 'base64') : null
  for (let address = 0; address < 0x10000; address++) {
    const actual = vram ? vram[address] : b.getVramByte(address)
    if (a.getVramByte(address) !== actual) {
      return `VRAM $${address.toString(16).padStart(4, '0')} is $${hex(actual)}, Video.ts's $${hex(a.getVramByte(address))}`
    }
  }
  return null
}

/** The first divergence, as `{ index, what }`, or null. */
function run(make, ops, { checkEvery, scope }) {
  const { a, b } = make()
  const bus = scope === 'bus'
  const tiles = scope === 'tiles'
  const status = scope === 'status' || tiles
  a.reset(true)
  b.reset(true)
  const previous = { stat0: 0, map: 0n }
  const at = (index, what) => ({ index, what: `${describe(ops[index])}: ${what}` })

  for (let index = 0; index < ops.length; index++) {
    const op = ops[index]
    try {
      if (op.w !== undefined) {
        a.write(op.w, op.v)
        b.write(op.w, op.v)
      } else if (op.r !== undefined) {
        const compared = !bus || (op.r & 1) === 0 ||
          CONSTANT_STATUS.includes(a.getRegister(op.r & 2 ? 0x0e : 0x0f) & 0x0f)
        const expected = a.read(op.r)
        const actual = b.read(op.r)
        if (compared && actual !== expected) return at(index, `read $${hex(actual)}, Video.ts read $${hex(expected)}`)
      } else if (op.t !== undefined) {
        for (let n = 0; n < op.t; n++) {
          if (a.tick(FREQUENCY) !== b.tick(FREQUENCY) && !bus) return at(index, `/INT differs after ${n + 1} tick(s)`)
          if (tiles && (a.frameReady || b.frameReady)) {
            if (a.frameReady !== b.frameReady) {
              return at(index, `after ${n + 1} tick(s) ${b.frameReady ? 'the core' : 'Video.ts'} alone presented a frame`)
            }
            const difference = frameDifference(a, b)
            if (difference) return at(index, `after ${n + 1} tick(s): ${difference}`)
            a.frameReady = b.frameReady = false
            held.frames++
            const frame = a.frameIndices()
            if (firstDifference(frame, new Uint8Array(frame.length).fill(frame[0])) >= 0) held.drawn++
          }
        }
      } else if (op.p !== undefined) {
        a.setVramByte(op.p, op.v)
        b.setVramByte(op.p, op.v)
      } else if (op.g !== undefined) {
        a.setRegister(op.g, op.v)
        b.setRegister(op.g, op.v)
      } else {
        a.reset(op.x)
        b.reset(op.x)
      }
      if (tiles) {
        for (const register of LXCTRL) {
          if (!(a.getRegister(register) & LXCTRL_DEPTH)) continue
          const value = a.getRegister(register) & ~LXCTRL_DEPTH
          a.setRegister(register, value)
          b.setRegister(register, value)
          held.depth++
        }
      }
      const checkpoint = (index + 1) % checkEvery === 0 || index === ops.length - 1
      if (bus) {
        const difference = checkpoint ? stateDifference(a, b) : null
        if (difference) return at(index, difference)
        continue
      }
      if (interrupt(a) !== interrupt(b)) return at(index, `/INT is ${interrupt(b) ? 1 : 0}, Video.ts's ${interrupt(a) ? 1 : 0}`)
      if (status) {
        // Each time Video.ts's OVF or COL goes from clear to set, and each
        // time its collision map gains a bit: that the sprites are reached.
        const stat0 = a.peekStatus(0)
        if (stat0 & ~previous.stat0 & STAT0_OVF) held.overflow++
        if (stat0 & ~previous.stat0 & STAT0_COL) held.collision++
        previous.stat0 = stat0
        if (stat0 & STAT0_COL) {
          let map = 0n
          for (let select = 15; select >= 8; select--) map = (map << 8n) | BigInt(a.peekStatus(select))
          if (map & ~previous.map) held.map++
          previous.map = map
        } else {
          previous.map = 0n
        }
        const difference = checkpoint ? statusDifference(a, b) ?? stateDifference(a, b) : null
        if (difference) return at(index, difference)
        continue
      }
      if (checkpoint) {
        const difference = frameDifference(a, b)
        if (difference) return at(index, difference)
      }
    } catch (error) {
      return at(index, `threw ${error.name}: ${error.message}`)
    }
  }
  return null
}

/** Drop chunks of the list, halving, for as long as it still diverges. */
function minimise(make, ops, options, budgetMs = 30_000) {
  const deadline = Date.now() + budgetMs
  let current = ops
  for (let chunk = Math.floor(current.length / 2); chunk >= 1; chunk = Math.floor(chunk / 2)) {
    for (let start = 0; start < current.length && Date.now() < deadline; ) {
      const candidate = current.slice(0, start).concat(current.slice(start + chunk))
      if (candidate.length && run(make, candidate, options)) current = candidate
      else start += chunk
    }
  }
  return current
}

/** The operations as Video.ts saw them, in docs/TRACE.md's format. */
function record(Reference, ops, name) {
  const video = new Reference()
  const recorder = new TraceRecorder(video)
  video.observer = recorder
  video.reset(true)
  for (const op of ops) {
    if (op.w !== undefined) video.write(op.w, op.v)
    else if (op.r !== undefined) video.read(op.r)
    else if (op.t !== undefined) for (let n = 0; n < op.t; n++) video.tick(FREQUENCY)
    else video.reset(false)
  }
  return writeTrace({ header: { fixture: name, emulator: 'fuzz', frequency: FREQUENCY }, lines: recorder.lines })
}

/** What a stream is made of, for the log. */
function composition(ops) {
  const names = { w: 'writes', r: 'reads', t: 'tick runs', p: 'pokes', g: 'register sets', x: 'resets' }
  const counts = {}
  let ticks = 0
  for (const op of ops) {
    const kind = Object.keys(op)[0]
    counts[kind] = (counts[kind] ?? 0) + 1
    if (op.t !== undefined) ticks += op.t
  }
  const parts = Object.entries(counts).map(([kind, count]) => `${count} ${names[kind]}`)
  return `${parts.join(', ')}; ${ticks} ticks, ${Math.floor(ticks / (FREQUENCY / 60 / 262))} line starts`
}

/** Where the two presented frames first differ, or null. */
function frameDifference(a, b) {
  const difference = firstDifference(a.frameIndices(), b.frameIndices())
  if (difference < 0) return null
  const x = difference % 320
  const y = Math.floor(difference / 320)
  return `frame differs at x ${x}, y ${y}: ${b.frameIndices()[difference]}, Video.ts ${a.frameIndices()[difference]}`
}

function firstDifference(a, b) {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return i
  return -1
}

const hex = (value) => value.toString(16).padStart(2, '0')

main()
