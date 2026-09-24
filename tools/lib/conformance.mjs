// Port conformance through the bus (PLAN.md Phase 11): accesses on all four
// ports played through the Nano to the card, every read the reference can
// answer independently of time compared with what Video.ts returns.
//
// Untimed, so only what does not depend on where the raster is may be compared:
//
//   - every data-port read (§4), which is VRAM and the prefetch alone
//   - status reads of STAT4 and STAT6, which are constant (§6); STAT5 is this
//     firmware's version where the emulator reports the spec's (PLAN.md
//     section 4), and the rest move with the raster, so those are read — each
//     still resets its pair's flip-flop — but not compared
//   - no FONT load (§7), whose copy lands at the next vertical blank: a FONT
//     command naming a reserved font does nothing and is kept, and one that
//     would load font $00 is sent to a reserved register instead
//
// The card starts from an RST pulse (§15); its VRAM, which RST leaves alone, is
// read over the debug link and given to the reference, so neither starts from
// an assumption. The reference then plays each access as the card takes it.
//
// Two sources of accesses: the scripts in tests/bench (a small language, below),
// and a seeded stream drawn from tools/fuzz.mjs's bus scope with the edge cases
// Phase 11 names added: every command form, VBANK and VINC at their limits, and
// both read orders.

import { readFileSync, readdirSync } from 'node:fs'
import { basename, join } from 'node:path'
import { REPO, loadVideo } from './emulator.mjs'
import { ACCESS, PACED_MAX, SCRIPT_MAX, opPort, scriptPort } from './nano.mjs'
import { generate } from '../fuzz.mjs'

export const SCRIPTS_DIR = join(REPO, 'tests', 'bench')

const STATSEL_B = 0x0e
const STATSEL_A = 0x0f
const VBANK = 0x08
const VINC = 0x09
const FONT = 0x30
/** Status registers a read may be compared on (§6): STAT4 and STAT6. */
const COMPARED_STATUS = new Set([4, 6])

// ---- the reference ----

/**
 * Video.ts as the card should stand: RST'd, over the VRAM the card holds. Each
 * access becomes a SCRIPT pair for the Nano, a read carrying what Video.ts
 * returned when it can be compared.
 */
export class Reference {
  constructor(vram) {
    const Video = loadVideo()
    this.video = new Video()
    this.video.reset(true)
    for (let address = 0; address < vram.length; address++) this.video.setVramByte(address, vram[address])
    this.video.reset(false)
    this.fontsRedirected = 0
  }

  /**
   * The SCRIPT pair for one access, applied here: `{ w: port, v }`, `{ r: port }`,
   * or `{ reset: us }`, an RST pulse (§15). `{ r, compare: true }` compares a
   * read whatever it selects, and `{ w, v, font: true }` lets a FONT command
   * through: both for the RST check, which makes its own timing safe.
   */
  pair(access) {
    const video = this.video
    if (access.reset !== undefined) {
      video.reset(false)
      return [ACCESS.RESET, access.reset]
    }
    if (access.w !== undefined) {
      let value = access.v & 0xff
      const port = access.w & 3
      if (port & 1 && value === (0x80 | FONT) && !access.font) {
        const state = video.portState(port & 2 ? 'b' : 'a')
        if (state.awaitingCommand && (state.payload & 0x7f) === 0) {
          value = 0x80 | (FONT + 1) // a reserved register: §7's load would be timed
          this.fontsRedirected++
        }
      }
      video.write(port, value)
      return [ACCESS.WRITE | scriptPort(port), value]
    }
    const port = access.r & 3
    const select = video.getRegister(port & 2 ? STATSEL_B : STATSEL_A) & 0x0f
    const value = video.read(port)
    const compared = access.compare || !(port & 1) || COMPARED_STATUS.has(select)
    return [(compared ? ACCESS.READ : ACCESS.READ_IGNORED) | scriptPort(port), compared ? value : 0]
  }

  /** What the card should hold: VRAM, the registers through their aliases, the ports. */
  state() {
    const video = this.video
    const vram = Buffer.alloc(0x10000)
    for (let address = 0; address < vram.length; address++) vram[address] = video.getVramByte(address)
    const registers = Array.from({ length: 128 }, (_, i) => video.getRegister(i))
    const ports = ['a', 'b'].map((which) => {
      const p = video.portState(which)
      return { pointer: p.pointer, prefetch: p.readAhead, payload: p.payload, readMode: p.readMode, second: p.awaitingCommand }
    })
    return { vram, registers, ports }
  }
}

/** §5: $02-$06 are $10-$12, $20 and $21, as the link's raw register file holds them. */
const ALIASES = { 2: 0x10, 3: 0x11, 4: 0x12, 5: 0x20, 6: 0x21 }

/**
 * Where the card, as the debug link reads it (VRAM, and a snapshot's state),
 * differs from the reference's. An empty list is agreement.
 */
export function stateDifferences(reference, vram, state) {
  const want = reference.state()
  const out = []
  let bytes = 0
  let first = -1
  for (let i = 0; i < want.vram.length; i++) {
    if (vram[i] !== want.vram[i]) {
      if (first < 0) first = i
      bytes++
    }
  }
  if (bytes) out.push(`VRAM: ${bytes} bytes differ, the first at $${hex(first, 4)}: $${hex(vram[first])}, Video.ts $${hex(want.vram[first])}`)
  for (let i = 0; i < 128; i++) {
    const got = state.registers[ALIASES[i] ?? i]
    if (got !== want.registers[i]) out.push(`register $${hex(i)}: $${hex(got)}, Video.ts $${hex(want.registers[i])}`)
  }
  for (let pair = 0; pair < 2; pair++) {
    for (const field of ['pointer', 'prefetch', 'payload', 'readMode', 'second']) {
      const got = state.ports[pair][field]
      const w = want.ports[pair][field]
      if (got !== w) out.push(`port ${'AB'[pair]} ${field}: ${fmt(got)}, Video.ts ${fmt(w)}`)
    }
  }
  return out
}

const hex = (v, width = 2) => v.toString(16).padStart(width, '0')
const fmt = (v) => (typeof v === 'number' ? `$${hex(v)}` : String(v))

// ---- the scripts in tests/bench ----

/**
 * A bus script, one access or group a line; `#` starts a comment. `A` and `B`
 * are the port pairs, and a number is decimal or $hex.
 *
 *   w A $55             write the data port
 *   r A [n]             read the data port, n times (1)
 *   s A [n]             read the status port, n times
 *   c A $12             write the command port: half a pair
 *   reg A $0f 4         a register, through pair A's command port
 *   read A $1000        VBANK, then a read address (which prefetches)
 *   write A $1000       VBANK, then a write address
 *   fill A $10 $20 ...  write each byte to the data port
 *
 * `read` and `write` set VBANK through the same pair, as a program would.
 */
export function parseScript(text, name = 'script') {
  const accesses = []
  const lines = text.split('\n')
  lines.forEach((raw, index) => {
    const line = raw.replace(/#.*/, '').trim()
    if (!line) return
    const [verb, pairName, ...rest] = line.split(/\s+/)
    const fail = (why) => {
      throw new Error(`${name}:${index + 1}: ${why}: ${raw.trim()}`)
    }
    const pair = { A: 0, B: 2 }[pairName]
    if (pair === undefined) fail('the pair is A or B')
    const numbers = rest.map((word) => {
      const n = word.startsWith('$') ? parseInt(word.slice(1), 16) : Number(word)
      if (!Number.isInteger(n)) fail(`not a number: ${word}`)
      return n
    })
    const register = (r, v) => accesses.push({ w: pair | 1, v: v & 0xff }, { w: pair | 1, v: 0x80 | (r & 0x7f) })
    const point = (address, write) => {
      register(VBANK, (address >> 14) & 3)
      accesses.push({ w: pair | 1, v: address & 0xff }, { w: pair | 1, v: ((address >> 8) & 0x3f) | (write ? 0x40 : 0) })
    }
    switch (verb) {
      case 'w':
        if (numbers.length !== 1) fail('w takes one byte')
        accesses.push({ w: pair, v: numbers[0] })
        break
      case 'r':
      case 's':
        for (let n = numbers[0] ?? 1; n > 0; n--) accesses.push({ r: pair | (verb === 's' ? 1 : 0) })
        break
      case 'c':
        if (numbers.length !== 1) fail('c takes one byte')
        accesses.push({ w: pair | 1, v: numbers[0] })
        break
      case 'reg':
        if (numbers.length !== 2) fail('reg takes a register and a value')
        register(numbers[0], numbers[1])
        break
      case 'read':
      case 'write':
        if (numbers.length !== 1) fail(`${verb} takes an address`)
        point(numbers[0] & 0xffff, verb === 'write')
        break
      case 'fill':
        if (!numbers.length) fail('fill takes bytes')
        for (const v of numbers) accesses.push({ w: pair, v })
        break
      default:
        fail(`unknown verb ${verb}`)
    }
  })
  return accesses
}

/** tests/bench's scripts, by name. */
export function loadScripts(only = null) {
  return readdirSync(SCRIPTS_DIR)
    .filter((file) => file.endsWith('.bus'))
    .sort()
    .map((file) => ({ name: basename(file, '.bus'), accesses: parseScript(readFileSync(join(SCRIPTS_DIR, file), 'utf8'), file) }))
    .filter((script) => !only || only.includes(script.name))
}

// ---- the random stream ----

/** mulberry32, as tools/fuzz.mjs has it. */
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

/** §4, §5: the strides and banks at their limits. */
const VINC_EDGES = [0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0xfe, 0xff]
const VBANK_EDGES = [0x00, 0x01, 0x02, 0x03, 0x04, 0xfc, 0xff]
/** Where a pointer carries between banks or wraps round 64 KB. */
const CARRIES = [0x0000, 0x3ff0, 0x4000, 0x7ff8, 0x8000, 0xbff8, 0xc000, 0xfff0, 0xfffc, 0xffff]

/**
 * The accesses of a run, `count` of them, from `seed`: runs of the fuzzer's bus
 * scope with its ticks, pokes and resets taken out (the bus replay is untimed,
 * and those are not bus accesses), between the edge cases.
 */
export function* stream(seed, count) {
  const next = random(seed ^ 0x9e3779b9)
  const pick = (list) => list[Math.floor(next() * list.length)]
  const byte = () => Math.floor(next() * 256)
  let made = 0
  let chunk = 0
  const out = function* (accesses) {
    for (const a of accesses) {
      if (made >= count) return
      made++
      yield a
    }
  }
  while (made < count) {
    // A run of the fuzz corpus.
    const corpus = generate((seed * 7919 + chunk++) >>> 0, 200 + Math.floor(next() * 800), 'bus').filter(
      (op) => op.w !== undefined || op.r !== undefined
    )
    yield* out(corpus)

    // Then an edge case or two.
    for (let n = 1 + Math.floor(next() * 3); n > 0 && made < count; n--) {
      const pair = next() < 0.5 ? 0 : 2
      const other = pair ^ 2
      const register = (r, v) => [{ w: pair | 1, v }, { w: pair | 1, v: 0x80 | r }]
      const roll = next()
      const edge = []
      if (roll < 0.3) {
        // A stride at a limit, across a carry or a wrap, written then read back.
        const address = (pick(CARRIES) + Math.floor(next() * 16) - 8) & 0xffff
        const vbank = pick(VBANK_EDGES)
        edge.push(...register(VINC, pick(VINC_EDGES)), ...register(VBANK, vbank))
        edge.push({ w: pair | 1, v: address & 0xff }, { w: pair | 1, v: ((address >> 8) & 0x3f) | 0x40 })
        const length = 1 + Math.floor(next() * 24)
        for (let i = 0; i < length; i++) edge.push({ w: pair, v: byte() })
        // VBANK written after the address command moves nothing already set.
        if (next() < 0.5) edge.push(...register(VBANK, pick(VBANK_EDGES)))
        edge.push(...register(VBANK, vbank))
        edge.push({ w: pair | 1, v: address & 0xff }, { w: pair | 1, v: (address >> 8) & 0x3f })
        for (let i = 0; i < length + 1; i++) edge.push({ r: pair })
      } else if (roll < 0.55) {
        // Both read orders: the pairs' data ports interleaved, one way then the
        // other, and a status read between.
        for (let i = 0, n = 2 + Math.floor(next() * 12); i < n; i++) {
          edge.push({ r: i & 1 ? other : pair })
          if (next() < 0.15) edge.push({ r: (i & 1 ? other : pair) | 1 })
        }
        for (let i = 0, n = 2 + Math.floor(next() * 12); i < n; i++) edge.push({ r: i & 1 ? pair : other })
      } else if (roll < 0.75) {
        // Every command form: a register, a write address, a read address, and
        // a half pair abandoned by a data access or a status read (§4).
        edge.push(...register(pick([STATSEL_A, STATSEL_B]), pick([0x04, 0x06, 0xf4, 0x16, byte()])))
        edge.push({ w: pair | 1, v: byte() })
        edge.push(next() < 0.5 ? { r: pair | 1 } : next() < 0.5 ? { r: pair } : { w: pair, v: byte() })
        edge.push({ w: pair | 1, v: byte() }, { w: pair | 1, v: (next() < 0.5 ? 0x40 : 0x00) | Math.floor(next() * 64) })
        edge.push({ r: pair }, { r: pair | 1 }, { r: other | 1 })
      } else if (roll < 0.9) {
        // A register write on one pair between the halves of a pair on the other.
        edge.push({ w: other | 1, v: byte() }, ...register(Math.floor(next() * 128), byte()), { w: other | 1, v: 0x80 | pick([VINC, STATSEL_A, STATSEL_B, 0x20]) })
        edge.push({ r: other | 1 }, { r: pair | 1 })
      } else {
        // FONT, naming a font or not: only a reserved one reaches the card (above).
        edge.push(...register(FONT, next() < 0.5 ? 0x00 : byte()))
      }
      yield* out(edge)
    }
  }
}

/**
 * READ_RUN spacings: 4 us exactly, which Phase 11 requires (a 1 MHz 6502's
 * back-to-back `lda`), and, measured but not required, a 2 MHz 6502's 2 us and
 * as fast as the Nano goes.
 */
const BACK_TO_BACK = [
  { label: '4 us', extra: 16, required: true },
  { label: '2 us', extra: 5, required: false },
  { label: 'flat out', extra: 0, required: false },
]

/**
 * §4 at the bus's tightest: runs of data reads on one port back to back, each
 * returning the prefetch the read before it fetched, which the card has to have
 * restaged in between (PLAN.md risk 5). VRAM is filled with random bytes first,
 * through the other pair, so every read is of a byte this run wrote.
 */
async function runBackToBack({ nano, start, seed, log, trials = 100 }) {
  const reference = await start()
  const next = random(seed ^ 0x5bd1e995)
  const out = []
  for (const spacing of BACK_TO_BACK) {
    let wrong = 0
    let first = null
    let ns = 0
    for (let trial = 0; trial < trials; trial++) {
      const [writer, reader] = trial & 1 ? [2, 0] : [0, 2]
      const address = Math.floor(next() * 0xff00)
      const fill = [...pointAccesses(writer, address, true)]
      for (let i = fill.length; i < SCRIPT_MAX; i++) fill.push({ w: writer, v: Math.floor(next() * 256) })
      await nano.script(fill.flatMap((access) => reference.pair(access)))
      await nano.script(pointAccesses(reader, address, false).flatMap((access) => reference.pair(access)))
      const expected = Array.from({ length: SCRIPT_MAX }, () => reference.pair({ r: reader })[1])
      const run = await nano.readRun(reader, SCRIPT_MAX, spacing.extra)
      ns += run.ns
      for (let i = 0; i < SCRIPT_MAX; i++) {
        if (run.bytes[i] === expected[i]) continue
        wrong++
        first ??= `trial ${trial}, read ${i} of port ${reader}: expected $${hex(expected[i])}, got $${hex(run.bytes[i])}`
      }
    }
    const period = ns / (trials * SCRIPT_MAX)
    out.push({ spacing: spacing.label, required: spacing.required, periodNs: Math.round(period), reads: trials * SCRIPT_MAX, wrong, first })
    log(`  back to back, ${spacing.label}: ${trials * SCRIPT_MAX} data reads ${(period / 1000).toFixed(2)} us apart: ${wrong ? `${wrong} WRONG, the first ${first}` : 'all right'}`)
  }
  return out
}

/** VBANK, then an address command, through one pair (§4). */
function pointAccesses(pair, address, write) {
  return [
    { w: pair | 1, v: (address >> 14) & 3 }, { w: pair | 1, v: 0x80 | VBANK },
    { w: pair | 1, v: address & 0xff }, { w: pair | 1, v: ((address >> 8) & 0x3f) | (write ? 0x40 : 0) },
  ]
}

/**
 * Accesses grouped into SCRIPT batches for the Nano, each with its expected
 * reads from `reference()`, asked afresh for every access so a resynchronised
 * run carries on against its new reference.
 */
export function* batches(reference, accesses, size = SCRIPT_MAX) {
  let pairs = []
  let held = []
  for (const access of accesses) {
    pairs.push(...reference().pair(access))
    held.push(access)
    if (held.length === size) {
      yield { pairs, accesses: held }
      pairs = []
      held = []
    }
  }
  if (held.length) yield { pairs, accesses: held }
}

/** One access, for a report. */
export function describe(access, pair) {
  const port = opPort(pair[0])
  const names = ['data A', 'status A', 'data B', 'status B']
  if ((pair[0] & 0xc0) === ACCESS.WRITE) return `write ${names[port]} $${hex(pair[1])}`
  if ((pair[0] & 0xc0) === ACCESS.READ) return `read ${names[port]}, expecting $${hex(pair[1])}`
  return `read ${names[port]} (not compared)`
}

// ---- the run ----

/**
 * Play tests/bench's scripts and `ops` accesses of the stream through the Nano
 * at each timing profile, and compare. Each script, and the stream, starts from
 * an RST pulse. Every read is compared on the Nano; every `checkEvery` accesses,
 * and at the end of each, the card's VRAM, registers and ports are read over the
 * debug link and compared whole, which catches a lost write that no read
 * happened to see. A mismatch is reported and the run resynchronised from a
 * fresh RST, so one fault is not counted many times.
 */
export async function runConformance({ nano, link, timings, ops, seed, scripts, checkEvery, paced = false, log = console.log }) {
  const { CMD, decodeSnapshot, decodeStats, packSnapshot, u8 } = await import('./link.mjs')

  // Phase 13's release parity: no debug link. The card's VRAM comes through
  // the pins instead — read between two RST pulses, which leave it as it was
  // but for the palette and the font (§15) — and a whole-card check is all 64
  // KB read back through the pins, compared against Video.ts as the stream's
  // own reads are. The registers are write-only, and are not checked.
  const busVram = async () => {
    const point = [0x00, 0x80 | VBANK, 0x01, 0x80 | 0x09, 0x00, 0x00].flatMap((v) => [ACCESS.WRITE | scriptPort(3), v])
    await nano.script(point)
    const out = Buffer.alloc(0x10000)
    for (let at = 0; at < out.length; at += 240) (await nano.readBlock(2, Math.min(240, out.length - at), { retry: false })).copy(out, at)
    return out
  }
  const start = async () => {
    await nano.reset(100)
    await new Promise((resolve) => setTimeout(resolve, 20))
    if (!link) {
      const vram = await busVram()
      await nano.reset(100)
      await new Promise((resolve) => setTimeout(resolve, 20))
      return new Reference(vram)
    }
    return new Reference(await link.request(CMD.VRAM, undefined, 10000))
  }
  const cardState = async () => {
    const vram = await link.request(CMD.VRAM, undefined, 10000)
    const snapshot = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 3000), 5000))
    return { vram, state: snapshot.state }
  }
  const busStats = async (reset) => (link ? decodeStats(await link.request(CMD.STATS, u8(reset ? 1 : 0))).bus : null)

  const results = []
  const backToBack = await runBackToBack({ nano, start, seed, log })
  for (const timing of timings) {
    const profile = await nano.profile(timing)
    const result = { timing: paced ? 'paced 2 us' : timing, profile, scripts: [], accesses: 0, paced: 0, reads: 0, compared: 0, mismatches: [], stateChecks: 0, stateFailures: [], fontsRedirected: 0 }
    const before = await busStats(true)
    const began = Date.now()

    // Play accesses from a fresh start, comparing; returns false on a mismatch.
    const play = async (label, accesses, every) => {
      let reference = await start()
      let since = 0
      let ok = true
      let batchIndex = 0
      const window = []
      // Phase 13: PACED plays each batch's accesses exactly 2 us apart, as a
      // 2 MHz 6502's back-to-back instructions would; a batch with a control
      // in it (a reset or a wait) goes by SCRIPT.
      for (const batch of batches(() => reference, accesses, paced ? PACED_MAX : SCRIPT_MAX)) {
        const control = batch.pairs.some((v, i) => i % 2 === 0 && (v & 0xc0) === 0xc0)
        const answer = paced && !control ? await nano.paced(batch.pairs) : await nano.script(batch.pairs)
        if (paced) result.paced += control ? 0 : batch.accesses.length
        result.accesses += batch.accesses.length
        for (let i = 0; i < batch.pairs.length; i += 2) {
          if ((batch.pairs[i] & 0xc0) !== ACCESS.WRITE) result.reads++
          if ((batch.pairs[i] & 0xc0) === ACCESS.READ) result.compared++
        }
        if (answer.differed) {
          ok = false
          const first = answer.log[0]
          const context = []
          const all = [...window, ...Array.from({ length: first.index + 1 }, (_, i) => [batch.pairs[2 * i], batch.pairs[2 * i + 1]])]
          for (const pair of all.slice(-12)) context.push(describe(null, pair))
          const mismatch = {
            where: `${label}, batch ${batchIndex}, access ${first.index}`,
            differed: answer.differed,
            reads: answer.log.map((m) => `access ${m.index}: expected $${hex(m.expected)}, got $${hex(m.got)}`),
            context,
          }
          result.mismatches.push(mismatch)
          log(`  MISMATCH ${mismatch.where}: ${mismatch.reads.join('; ')}`)
          for (const line of context) log(`      ${line}`)
          if (result.mismatches.length >= 20) return false
          // Carry on from a known state: a fresh start, and the stream goes on.
          reference = await start()
          window.length = 0
          batchIndex++
          continue
        }
        for (let i = 0; i < batch.pairs.length; i += 2) window.push([batch.pairs[i], batch.pairs[i + 1]])
        if (window.length > 24) window.splice(0, window.length - 24)
        since += batch.accesses.length
        batchIndex++
        if (every && since >= every) {
          since = 0
          if (!(await check(label, reference))) ok = false
        }
      }
      if (!(await check(label, reference))) ok = false
      result.fontsRedirected += reference.fontsRedirected
      return ok
    }
    const check = async (label, reference) => {
      result.stateChecks++
      if (!link) {
        // The readback as accesses of the stream's own: Video.ts takes them
        // too, so the stream carries on in step after it.
        const accesses = [{ w: 3, v: 0x00 }, { w: 3, v: 0x80 | VBANK }, { w: 3, v: 0x01 }, { w: 3, v: 0x89 }, { w: 3, v: 0x00 }, { w: 3, v: 0x00 }]
        for (let i = 0; i < 0x10000; i++) accesses.push({ r: 2 })
        let differed = 0
        for (const batch of batches(() => reference, accesses)) differed += (await nano.script(batch.pairs)).differed
        if (!differed) return true
        result.stateFailures.push({ where: `${label}, after ${result.accesses} accesses`, differences: [`VRAM read through the bus: ${differed} bytes differ from Video.ts`] })
        log(`  STATE DIFFERS ${label}: ${differed} bytes of VRAM read through the bus`)
        return false
      }
      const { vram, state } = await cardState()
      const differences = stateDifferences(reference, vram, state)
      if (!differences.length) return true
      result.stateFailures.push({ where: `${label}, after ${result.accesses} accesses`, differences: differences.slice(0, 12) })
      log(`  STATE DIFFERS ${label}: ${differences.slice(0, 4).join('; ')}`)
      return false
    }

    for (const script of scripts) {
      const ok = await play(`script ${script.name}`, script.accesses, 0)
      result.scripts.push({ name: script.name, accesses: script.accesses.length, ok })
      log(`  ${timing}: script ${script.name}, ${script.accesses.length} accesses: ${ok ? 'ok' : 'FAILED'}`)
    }
    if (ops > 0) {
      let reported = 0
      const counted = (function* () {
        let n = 0
        for (const access of stream(seed, ops)) {
          yield access
          if (++n - reported >= 1_000_000) {
            reported = n
            const seconds = (Date.now() - began) / 1000
            log(`  ${timing}: ${n.toLocaleString()} accesses, ${Math.round(result.accesses / seconds).toLocaleString()} a second`)
          }
        }
      })()
      await play(`stream seed ${seed}`, counted, checkEvery)
    }

    result.seconds = (Date.now() - began) / 1000
    result.bus = await busStats(false)
    result.busBefore = before
    result.backToBack = backToBack
    results.push(result)
    const b = result.bus
    log(
      `${timing}: ${result.accesses.toLocaleString()} accesses (${result.reads.toLocaleString()} reads, ${result.compared.toLocaleString()} compared) in ${result.seconds.toFixed(0)} s; ` +
        `${result.mismatches.length} mismatching batches, ${result.stateFailures.length} of ${result.stateChecks} state checks differ`
    )
    if (b) log(`  card: ${b.writes} writes, ${b.reads} reads, ${b.staleData} stale data reads, ${b.staleStatus} stale status reads, ${b.coincident} coincident, FIFO overruns ${b.writeOverruns}/${b.readOverruns}, staging waits ${b.stagingWaits}, resets ${b.resets}, interrupt max ${b.isrMax} cycles`)
  }
  return results
}

// ---- RST ----

/**
 * §15 through the RST pin, `trials` times: the card is put in a state far from
 * reset over the bus — registers, both pointers, a half pair, the vblank
 * interrupt asserted, the font and palette areas overwritten — and then, in one
 * Nano batch, a FONT load for layer 1 is asked for and RST pulsed, so the load
 * is still pending when the pulse cancels it. Straight after the pulse the same
 * batch reads STAT0, STAT1, STAT7 and the collision map, which must all be clear.
 * Then, frames later: /INT released, and VRAM, the registers and both ports as
 * Video.ts has them after its own reset(false) — so $0800-$0FFF is the built-in
 * font, $FC00-$FDFF the default palette, the FONT load's destination untouched
 * and the rest of VRAM as it was — and the snapshot's latches clear.
 */
export async function runResetCheck({ nano, link, trials, seed, log = console.log }) {
  const { CMD, decodeSnapshot, packSnapshot } = await import('./link.mjs')
  const next = random(seed ^ 0x2545f491)
  const byte = () => Math.floor(next() * 256)
  const font = readFileSync(join(REPO, 'fonts', 'cp437-6x8.bin'))
  const failures = []
  for (let trial = 0; trial < trials; trial++) {
    await nano.reset(100)
    await new Promise((resolve) => setTimeout(resolve, 20))
    const reference = new Reference(await link.request(CMD.VRAM, undefined, 10000))
    const problems = []

    // Far from reset. VINC stays 1 so the writes below land where they say.
    const register = (pair, r, v) => [{ w: pair | 1, v }, { w: pair | 1, v: 0x80 | r }]
    const scramble = [
      ...register(0, 0x01, 0x60 | (byte() & 0x13)),   // display on, vblank interrupt on
      ...register(2, 0x0a, 0x01 | (byte() & 0x02)),   // IRQEN
      ...register(0, 0x07, byte()), ...register(2, 0x0d, byte() & 0x0f), ...register(0, 0x13, byte()),
      ...register(2, 0x1a, 0x04),                     // L1PAT: $2000, where the FONT load would land
      ...register(0, STATSEL_A, 4 + (byte() & 1) * 2), ...register(2, STATSEL_B, byte()),
      ...pointAccesses(0, 0x0800 + Math.floor(next() * 0x700), true),
    ]
    for (let i = 0; i < 64; i++) scramble.push({ w: 0, v: byte() })        // over the font
    scramble.push(...pointAccesses(2, 0xfc00 + Math.floor(next() * 0x1c0), true))
    for (let i = 0; i < 32; i++) scramble.push({ w: 2, v: byte() })        // over the palette
    scramble.push(...pointAccesses(0, Math.floor(next() * 0x10000), false))
    scramble.push(...register(0, VINC, [0x01, 0xff, 0x40][Math.floor(next() * 3)]))
    for (let at = 0; at < scramble.length; at += SCRIPT_MAX) {
      await nano.script(scramble.slice(at, at + SCRIPT_MAX).flatMap((access) => reference.pair(access)))
    }
    await new Promise((resolve) => setTimeout(resolve, 40))                // vertical blank, twice
    if ((await nano.int()).level !== 0) problems.push('/INT was not asserted before the pulse')

    // FONT for layer 1, a half pair on A, RST, then status straight after.
    const pulse = [
      { w: 3, v: 0x80 }, { w: 3, v: 0x80 | FONT, font: true },
      { w: 1, v: byte() },
      { reset: 100 },
      { r: 1, compare: true },                                             // STAT0: STATSEL_A is 0
      ...register(2, STATSEL_B, 1), { r: 3, compare: true },               // STAT1
      ...register(2, STATSEL_B, 7), { r: 3, compare: true },               // STAT7
    ]
    for (let s = 8; s < 16; s++) pulse.push(...register(2, STATSEL_B, s), { r: 3, compare: true })
    pulse.push(...register(2, STATSEL_B, 0))
    const answer = await nano.script(pulse.flatMap((access) => reference.pair(access)))
    if (answer.differed) {
      for (const m of answer.log) problems.push(`after the pulse, access ${m.index}: expected $${hex(m.expected)}, got $${hex(m.got)}`)
    }

    await new Promise((resolve) => setTimeout(resolve, 60))                // frames: a live FONT load would land
    if ((await nano.int()).level !== 1) problems.push('/INT is still asserted after the pulse')
    const vram = await link.request(CMD.VRAM, undefined, 10000)
    const snapshot = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 3000), 5000))
    problems.push(...stateDifferences(reference, vram, snapshot.state))
    if (Buffer.compare(vram.subarray(0x0800, 0x1000), font)) problems.push('$0800-$0FFF is not fonts/cp437-6x8.bin')
    const st = snapshot.state
    if (st.fontPending) problems.push(`a FONT load is still pending: $${hex(st.fontPending)}`)
    if (st.irqLatch) problems.push(`STAT1 latches $${hex(st.irqLatch)}`)
    if (st.overflowSprite || st.collisionMap.some((b) => b) || st.stat0 & 0x7f) problems.push(`sprite status not clear: STAT0 $${hex(st.stat0)}`)
    if (st.interrupt) problems.push('the snapshot has /INT asserted')

    if (problems.length) failures.push({ trial, problems })
    log(`  RST ${trial + 1}/${trials}: ${problems.length ? problems.join('; ') : 'the state §15 names'}`)
  }
  return failures
}
