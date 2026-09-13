#!/usr/bin/env node

// Fuzz the core against Video.ts (PLAN.md section 4).
//
//   node tools/fuzz.mjs [--seed N] [--ops N] [--frames-every N] [--out DIR] [--self]
//
// Loads the emulator's compiled Video and the adapter (host/node/Video.cjs, or
// PICOVDP_ADDON) into one process, feeds both the same seeded stream of port
// operations and ticks — biased toward the registers, addresses and values that
// mean something — and compares every read, /INT after every operation and
// every tick, and the frame at intervals. The first divergence prints the seed,
// is minimised to the shortest operation list that still diverges, and is
// written to DIR (build/fuzz) as that list and as a docs/TRACE.md trace
// recorded from Video.ts, ready to become a unit test.
//
// --self fuzzes Video.ts against itself: the harness's own check, which must
// never diverge.
//
// Phase 2: a skeleton. Against the empty core it diverges at once; Phases 3-7
// widen the stream as the core grows.

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

  const ops = generate(options.seed, options.ops)
  console.log(
    `fuzz: seed ${options.seed}, ${ops.length} operations, Video.ts against ${options.self ? 'itself' : 'the core'}`
  )
  const started = Date.now()
  const found = run(make, ops, options.framesEvery)
  if (!found) {
    console.log(`no divergence in ${((Date.now() - started) / 1000).toFixed(1)}s`)
    return
  }

  console.log(`DIVERGED at operation ${found.index}: ${found.what}`)
  const minimal = minimise(make, ops.slice(0, found.index + 1), options.framesEvery)
  const last = run(make, minimal, options.framesEvery)
  console.log(`minimised to ${minimal.length} operation(s): ${last.what}`)

  mkdirSync(options.out, { recursive: true })
  const base = join(options.out, `fuzz-${options.seed}`)
  writeFileSync(`${base}.json`, JSON.stringify({ seed: options.seed, diverged: last.what, ops: minimal }, null, 1) + '\n')
  if (minimal.some((op) => op.x)) {
    console.log(`wrote ${relative(REPO, base)}.json (it cold-resets, which a version 1 trace cannot hold)`)
  } else {
    writeFileSync(`${base}.vdpt.gz`, record(Reference, minimal, `fuzz-${options.seed}`))
    console.log(`wrote ${relative(REPO, base)}.json and .vdpt.gz`)
  }
  process.exitCode = 1
}

function parseArgs(args) {
  const options = { seed: 1, ops: 100_000, framesEvery: 5_000, out: join(REPO, 'build', 'fuzz'), self: false }
  for (let i = 0; i < args.length; i++) {
    const value = () => args[++i] ?? usage()
    switch (args[i]) {
      case '--seed': options.seed = Number(value()) >>> 0; break
      case '--ops': options.ops = Number(value()); break
      case '--frames-every': options.framesEvery = Number(value()); break
      case '--out': options.out = value(); break
      case '--self': options.self = true; break
      default: usage()
    }
  }
  return options
}

function usage() {
  console.error('usage: fuzz.mjs [--seed N] [--ops N] [--frames-every N] [--out DIR] [--self]')
  process.exit(2)
}

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
 * `{ x: cold }`. Each kind of thing a program does becomes a few of them.
 */
function generate(seed, count) {
  const next = random(seed)
  const pick = (list) => list[Math.floor(next() * list.length)]
  const byte = () => (next() < 0.6 ? pick(VALUES) : Math.floor(next() * 256))
  const port = () => (next() < 0.8 ? 0 : 2) // port A mostly, B too
  const ops = []

  while (ops.length < count) {
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
      ops.push({ t: next() < 0.95 ? 1 + Math.floor(next() * 200) : Math.floor(next() * TICKS_PER_FRAME) })
    } else if (roll < 0.998) {
      ops.push({ w: p | 1, v: byte() }) // half a command pair
    } else {
      ops.push({ x: next() < 0.2 })
    }
  }
  return ops.slice(0, count)
}

// ---- running ----

const interrupt = (video) =>
  typeof video.interruptAsserted === 'function' ? video.interruptAsserted() : video.peekStatus(1) !== 0

const describe = (op) =>
  op.w !== undefined ? `write $${op.v.toString(16).padStart(2, '0')} to port ${op.w}`
    : op.r !== undefined ? `read port ${op.r}`
      : op.t !== undefined ? `tick ${op.t}`
        : `${op.x ? 'cold' : 'warm'} reset`

/** The first divergence, as `{ index, what }`, or null. */
function run(make, ops, framesEvery) {
  const { a, b } = make()
  a.reset(true)
  b.reset(true)
  const at = (index, what) => ({ index, what: `${describe(ops[index])}: ${what}` })

  for (let index = 0; index < ops.length; index++) {
    const op = ops[index]
    try {
      if (op.w !== undefined) {
        a.write(op.w, op.v)
        b.write(op.w, op.v)
      } else if (op.r !== undefined) {
        const expected = a.read(op.r)
        const actual = b.read(op.r)
        if (actual !== expected) return at(index, `read $${hex(actual)}, Video.ts read $${hex(expected)}`)
      } else if (op.t !== undefined) {
        for (let n = 0; n < op.t; n++) {
          if (a.tick(FREQUENCY) !== b.tick(FREQUENCY)) return at(index, `/INT differs after ${n + 1} tick(s)`)
        }
      } else {
        a.reset(op.x)
        b.reset(op.x)
      }
      if (interrupt(a) !== interrupt(b)) return at(index, `/INT is ${interrupt(b) ? 1 : 0}, Video.ts's ${interrupt(a) ? 1 : 0}`)
      if ((index + 1) % framesEvery === 0 || index === ops.length - 1) {
        const difference = firstDifference(a.frameIndices(), b.frameIndices())
        if (difference >= 0) {
          const x = difference % 320
          const y = Math.floor(difference / 320)
          return at(index, `frame differs at x ${x}, y ${y}: ${b.frameIndices()[difference]}, Video.ts ${a.frameIndices()[difference]}`)
        }
      }
    } catch (error) {
      return at(index, `threw ${error.name}: ${error.message}`)
    }
  }
  return null
}

/** Drop chunks of the list, halving, for as long as it still diverges. */
function minimise(make, ops, framesEvery, budgetMs = 30_000) {
  const deadline = Date.now() + budgetMs
  let current = ops
  for (let chunk = Math.floor(current.length / 2); chunk >= 1; chunk = Math.floor(chunk / 2)) {
    for (let start = 0; start < current.length && Date.now() < deadline; ) {
      const candidate = current.slice(0, start).concat(current.slice(start + chunk))
      if (candidate.length && run(make, candidate, framesEvery)) current = candidate
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

function firstDifference(a, b) {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return i
  return -1
}

const hex = (value) => value.toString(16).padStart(2, '0')

main()
