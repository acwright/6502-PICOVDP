// Phase 13 on the raster: the latch (§3), interrupt timing (§14) and how fresh
// status is (§6), each measured from outside the card through the Nano's
// interrupt-triggered runs (INT_RUN, docs/BENCH.md) and, for the latch, the
// frames the card sent to VGA (SNAPSHOT).
//
// Everything here sets the card up from an RST pulse over the bus, then drives
// it from pair B, as an interrupt handler on port B would (§4): pair A is left
// alone. Timer 1 ticks are 62.5 ns.

import { ACCESS, PORT, TICK_NS, scriptPort } from './nano.mjs'
import { CMD, decodeSnapshot, decodeStats, packSnapshot, u8 } from './link.mjs'

export const SCREEN_LINES = 262
/** 2 x 1,598 PIO ticks at 352 MHz / 7: one display line, in nanoseconds. */
export const LINE_NS = (2 * 1598 * 7 * 1e9) / 352e6

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

// §5's registers used here.
const REG = { MODE1: 0x01, COLOR: 0x07, VBANK: 0x08, VINC: 0x09, IRQEN: 0x0a, IRQLINE: 0x0b, VMODE: 0x0d, STATSEL_B: 0x0e, STATSEL_A: 0x0f, L0NAME: 0x10, L0PAT: 0x12, L0SCRX: 0x13, SPRCTRL: 0x23 }

/** The geometries by VMODE (§9): the picture's lines and its top border. */
export const GEOMETRIES = {
  text: { vmode: 1, lines: 192, top: 24 },
  compact: { vmode: 2, lines: 192, top: 24 },
  graphics: { vmode: 3, lines: 240, top: 0 },
  full: { vmode: 4, lines: 240, top: 0 },
}

/** A display line's screen line in a geometry (§3). */
export const screenOf = (display, g) => (display + g.top) % SCREEN_LINES

// ---- SCRIPT and INT_RUN ops ----

const cmd = (pair) => scriptPort(pair ? PORT.B_COMMAND : PORT.ADDRESS)
const dat = (pair) => scriptPort(pair ? PORT.B_DATA : PORT.DATA)
export const reg = (pair, index, value) => [ACCESS.WRITE | cmd(pair), value & 0xff, ACCESS.WRITE | cmd(pair), 0x80 | index]
export const statusRead = (pair) => [ACCESS.READ_IGNORED | cmd(pair), 0]
/** A status read whose byte an INT_RUN returns (INT_RUN returns every read's byte, compared or not). */
export const statusSample = statusRead

function address(pair, at, write) {
  return [ACCESS.WRITE | cmd(pair), at & 0xff, ACCESS.WRITE | cmd(pair), ((at >> 8) & 0x3f) | (write ? 0x40 : 0)]
}

async function writeBytes(nano, pair, at, bytes) {
  for (let i = 0; i < bytes.length; i += 100) {
    const chunk = bytes.subarray(i, i + 100)
    await nano.script([...address(pair, at + i, true), ...[...chunk].flatMap((v) => [ACCESS.WRITE | dat(pair), v])])
  }
}

/**
 * From an RST pulse: a geometry with its picture on, layer 0 at 1bpp coloured by
 * COLOR (§8's source "none") over a name table of pattern 0, which is stripes,
 * sprites off; VINC +1, VBANK 0; STATSEL_B on STAT1, so a status read on port B
 * acknowledges every interrupt; interrupts off. Border and picture both show
 * COLOR, so every row of the frame shows a COLOR change, and the picture's rows
 * an L0SCRX change.
 */
export async function rasterCard(nano, geometry, { color = 0xf4, scrx = 0 } = {}) {
  await nano.reset(100)
  await sleep(20)
  await nano.script([
    ...reg(1, REG.VINC, 0x01), ...reg(1, REG.VBANK, 0x00), ...reg(1, REG.IRQEN, 0x00),
    ...reg(1, REG.SPRCTRL, 0x00), ...reg(1, REG.VMODE, geometry.vmode),
    ...reg(1, REG.L0NAME, 0x04), ...reg(1, REG.L0PAT, 0x00),   // names at $1000, pattern 0 at $0000
    ...reg(1, REG.COLOR, color), ...reg(1, REG.L0SCRX, scrx), ...reg(1, REG.STATSEL_B, 0x01),
  ])
  await writeBytes(nano, 1, 0x0000, Buffer.alloc(8, 0xf0))
  await writeBytes(nano, 1, 0x1000, Buffer.alloc(1200, 0x00))
  await nano.script([...reg(1, REG.MODE1, 0x40), ...statusRead(1)])
}

// ---- the latch (§3) ----

/**
 * IRQLINE = N; on its /INT the handler writes COLOR and L0SCRX, sets IRQLINE to
 * M, and acknowledges; on M's it puts them back and IRQLINE to N. Every frame
 * shows the new state from display line N + 2 and the old from M + 2 (§3).
 * Snapshots are taken while it runs, and each row of each is classified as old
 * or new against frames of the two states held still. A transition is a trial:
 * the row it should first show on must, and the row before must not — or, where
 * those rows are not on screen, the first row after that is must.
 */
export async function runLatchTest({ nano, link, geometry = GEOMETRIES.compact, lines, snapshotsPerLine = 100, delayTicks = 0, log = console.log }) {
  const OLD = { color: 0xf4, scrx: 0 }
  const NEW = { color: 0x1b, scrx: 4 }
  const reference = async (state) => {
    await rasterCard(nano, geometry, state)
    await sleep(60)
    const s = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 2000), 5000))
    return s.rows
  }
  const frames = { old: await reference(OLD), new: await reference(NEW) }
  for (let row = 0; row < 240; row++) {
    const a = frames.old.subarray(row * 320, row * 320 + 320)
    const b = frames.new.subarray(row * 320, row * 320 + 320)
    if (a.equals(b)) throw new Error(`the two states look alike on row ${row}`)
  }

  const results = []
  for (const n of lines) {
    let m = (n + 131) % SCREEN_LINES
    if (m > 255) m -= 10
    await rasterCard(nano, geometry, OLD)
    await nano.script([...reg(1, REG.IRQLINE, n), ...reg(1, REG.IRQEN, 0x02), ...statusRead(1)])
    const toNew = [...reg(1, REG.COLOR, NEW.color), ...reg(1, REG.L0SCRX, NEW.scrx), ...reg(1, REG.IRQLINE, m), ...statusRead(1)]
    const toOld = [...reg(1, REG.COLOR, OLD.color), ...reg(1, REG.L0SCRX, OLD.scrx), ...reg(1, REG.IRQLINE, n), ...statusRead(1)]

    // Screen lines where the new state first shows, and the old again.
    const firstNew = screenOf(n + 2, geometry), firstOld = screenOf(m + 2, geometry)
    const expectNew = (s) => (firstNew < firstOld ? s >= firstNew && s < firstOld : s >= firstNew || s < firstOld)

    // A snapshot takes about ten frames to arm, wait for and send: the handler
    // runs for longer than the snapshots need, and only those taken wholly
    // inside its run count.
    const frameCount = snapshotsPerLine * 14 + 60
    const started = Date.now()
    const running = nano.intRun([toNew, toOld], { edges: 2 * frameCount, timeout: 0.1, delayTicks })
    const until = started + frameCount * 16.68 - 400
    await sleep(80)
    const r = { line: n, m, snapshots: 0, trials: 0, wrongRows: 0, lateRows: 0, examples: [] }
    while (Date.now() < until && r.snapshots < snapshotsPerLine) {
      const s = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 2000), 5000))
      if (Date.now() > until) break
      r.snapshots++
      let wrong = 0
      for (let row = 0; row < 240; row++) {
        if (s.late[row]) {
          r.lateRows++
          continue
        }
        const got = s.rows.subarray(row * 320, row * 320 + 320)
        const want = expectNew(row) ? frames.new : frames.old
        if (!got.equals(want.subarray(row * 320, row * 320 + 320))) {
          wrong++
          if (r.examples.length < 4) {
            const is = got.equals(frames.old.subarray(row * 320, row * 320 + 320)) ? 'old' : got.equals(frames.new.subarray(row * 320, row * 320 + 320)) ? 'new' : 'neither'
            r.examples.push(`frame ${s.frame} row ${row}: ${is}, expected ${expectNew(row) ? 'new' : 'old'}`)
          }
        }
      }
      r.wrongRows += wrong
      r.trials += 2
    }
    const run = await running
    r.edges = run.edges
    r.timedOut = run.timedOut
    results.push(r)
    log(`  IRQLINE ${String(n).padStart(3)} (then ${String(m).padStart(3)}): new from screen line ${String(firstNew).padStart(3)}, old from ${String(firstOld).padStart(3)}; ` +
      `${r.snapshots} frames, ${r.trials} trials, ${r.wrongRows} rows wrong, ${r.lateRows} late${r.timedOut ? ', THE HANDLER MISSED AN EDGE' : ''}` +
      (r.examples.length ? `\n      ${r.examples.join('\n      ')}` : ''))
  }
  await nano.script([...reg(1, REG.IRQEN, 0x00), ...statusRead(1)])
  return results
}

// ---- interrupt timing (§14) ----

/** Edge times from an INT_RUN, unwrapped to nanoseconds from the first. */
function edgeTimes(records) {
  const out = []
  let base = null, last = null, carry = 0
  for (const r of records) {
    let t = r.ticks
    if (last !== null && t < last) carry += 2 ** 32
    last = t
    t += carry
    if (base === null) base = t
    out.push((t - base) * TICK_NS)
  }
  return out
}

/**
 * The vertical blank interrupt's period, over `frames` frames: every edge's
 * time, acknowledged each time by a status read on port B.
 */
export async function measureVblank({ nano, geometry = GEOMETRIES.full, frames = 400 }) {
  await rasterCard(nano, geometry)
  await nano.script([...reg(1, REG.IRQEN, 0x01), ...statusRead(1)])
  const periods = []
  for (let got = 0; got < frames;) {
    const run = await nano.intRun([statusRead(1)], { edges: 40, timeout: 0.1, times: true })
    const t = edgeTimes(run.records)
    for (let i = 1; i < t.length; i++) periods.push(t[i] - t[i - 1])
    got += run.edges
  }
  await nano.script([...reg(1, REG.IRQEN, 0x00), ...statusRead(1)])
  return summary(periods)
}

export function summary(values) {
  const n = values.length
  const mean = values.reduce((a, b) => a + b, 0) / n
  const sd = Math.sqrt(values.reduce((a, b) => a + (b - mean) ** 2, 0) / n)
  return { n, mean, sd, min: Math.min(...values), max: Math.max(...values) }
}

/**
 * Where each scanline compare falls against vertical blank: IRQEN has both, and
 * each frame's two edges are told apart by STAT1, read on port B to acknowledge.
 * For every IRQLINE in `lines`, `frames` frames. Returns, per line, the mean time
 * from the vertical blank edge to the scanline edge after it, its spread, and
 * the same in display lines.
 */
export async function measureScanlines({ nano, geometry, lines, frames = 4 }) {
  await rasterCard(nano, geometry)
  const vblankLine = geometry.lines
  const out = []
  for (const n of lines) {
    await nano.script([...reg(1, REG.IRQLINE, n), ...reg(1, REG.IRQEN, 0x03), ...statusRead(1)])
    const run = await nano.intRun([statusRead(1)], { edges: 2 * frames + 2, timeout: 0.1, times: true, reads: true })
    const t = edgeTimes(run.records)
    const deltas = []
    let vblankAt = null
    for (let i = 0; i < run.records.length; i++) {
      const sources = run.records[i].bytes[0]
      if (sources & 0x01) vblankAt = t[i]
      if ((sources & 0x02) && vblankAt !== null) {
        // The same edge may carry both, when N is the vertical blank's line.
        deltas.push(t[i] - vblankAt)
      }
    }
    if (!deltas.length) {
      out.push({ line: n, samples: 0 })
      continue
    }
    const s = summary(deltas)
    // §3: vertical blank at the picture's end, screen line `lines + top`; the
    // odd VGA line after screen line 261, half a display line.
    const from = vblankLine + geometry.top, to = screenOf(n, geometry)
    const expected = ((to - from + SCREEN_LINES) % SCREEN_LINES) + (to < from ? 0.5 : 0)
    out.push({ line: n, samples: s.n, meanNs: s.mean, sdNs: s.sd, minNs: s.min, maxNs: s.max, expected })
  }
  await nano.script([...reg(1, REG.IRQEN, 0x00), ...statusRead(1)])
  return out
}

// ---- status freshness (§6) ----

/**
 * Ticks from the time INT_RUN reports for a run's start to its first access's
 * strobe falling, when the card chooses the byte it serves: 48 cycles setting
 * up the call, 10 in pacedRun's prologue, 18 into its loop. Counted from the
 * harness's compiled code (firmware 3); a change there moves it.
 */
export const FIRST_SAMPLE_TICKS = 76

/**
 * STAT2 read from scanline handlers: IRQLINE cycles through `targets`, one
 * handler reprogramming it for the next, and each handler reads STAT2 on port A
 * `delayTicks` after its edge, then acknowledges on port B. Returns, for each
 * handler, the line it was for, what STAT2 read, and when after the edge the
 * read was sampled, in ticks. An edge that came between two runs, while the
 * Nano was talking to the host, is served late and left out.
 */
export async function sampleStat2({ nano, targets, delayTicks, edges }) {
  const sequences = targets.map((n, i) => [
    ACCESS.READ_IGNORED | cmd(0), 0,
    ...reg(1, REG.IRQLINE, targets[(i + 1) % targets.length]),
    ...statusRead(1),
  ])
  const out = []
  let late = 0
  // Eight bytes an edge in 250, and whole turns of the targets.
  const per = Math.floor(250 / 8 / targets.length) * targets.length
  for (let done = 0; done < edges;) {
    const run = await nano.intRun(sequences, { edges: per, timeout: 0.1, reads: true, times: true, delayTicks })
    for (let i = 0; i < run.records.length; i++) {
      const r = run.records[i]
      if (r.offset > delayTicks + 100) {
        late++
        continue
      }
      out.push({ line: targets[i % targets.length], stat2: r.bytes[0], ticks: r.offset + FIRST_SAMPLE_TICKS })
    }
    done += run.records.length
    if (run.timedOut) break
  }
  return { samples: out, late }
}

/**
 * STAT3 across a line: a scanline handler at IRQLINE `line` reads STAT3 on
 * port A 30 times, 2 us apart, starting at each of `phases` ticks after its
 * edge, so the reads interleave at 2 us / phases. Returns, per tick after the
 * edge, how many reads there were and how many had b1 and b0 set.
 */
export async function sampleStat3({ nano, line, phases, edgesPerPhase = 18 }) {
  const reads = 30
  const sequence = [...Array(reads)].flatMap(() => [ACCESS.READ_IGNORED | cmd(0), 0]).concat(statusRead(1))
  await nano.script([...reg(1, REG.STATSEL_A, 0x03), ...reg(1, REG.IRQLINE, line), ...reg(1, REG.IRQEN, 0x02), ...statusRead(1)])
  const bins = new Map()
  for (const phase of phases) {
    for (let done = 0; done < edgesPerPhase;) {
      const run = await nano.intRun([sequence], { edges: 6, timeout: 0.1, times: true, reads: true, delayTicks: phase })
      for (const r of run.records) {
        done++
        if (r.offset > phase + 100) continue
        for (let i = 0; i < reads; i++) {
          const t = r.offset + FIRST_SAMPLE_TICKS + 32 * i
          const e = bins.get(t) ?? { n: 0, b1: 0, b0: 0 }
          e.n++
          if (r.bytes[i] & 2) e.b1++
          if (r.bytes[i] & 1) e.b0++
          bins.set(t, e)
        }
      }
    }
  }
  await nano.script([...reg(1, REG.IRQEN, 0x00), ...statusRead(1)])
  return bins
}

/** The card's own figures for the raster, from STATS. */
export async function cardRaster(link, reset = false) {
  return decodeStats(await link.request(CMD.STATS, u8(reset ? 1 : 0)))
}

export { REG }
