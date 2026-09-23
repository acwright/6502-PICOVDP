// vdpctl replay: the bus executor (PLAN.md section 4 and Phase 12,
// docs/TRACE.md section 6).
//
// A fixture's trace is played through the Nano to the card's pins with no
// timing at all: its reads and writes in order, 120 to a SCRIPT, and nothing
// else. Line starts, /INT and the ticks between operations are the card's own
// business, which is why only a static checkpoint can be reproduced this way
// (TRACE.md section 4) and only what does not depend on the raster compared:
//
//   - every data-port read, against the value the trace recorded (§4)
//   - status reads of STAT4 and STAT6, which are constant, against the trace;
//     and of STAT5 against the version the board reports in INFO, since the
//     trace holds the emulator's (PLAN.md section 4). The rest move with the
//     raster: they are read, which moves the card as the program moved it, but
//     not compared
//   - at each checkpoint's settle point, the next complete frame (SNAPSHOT)
//     against the golden index frame
//   - at the checkpoint itself, VRAM and the 128 registers against the golden's
//
// One operation is timed even here: a FONT load lands at the next vertical
// blank (§7). Every program in the oracle writes FONT and then polls status
// until the load is done, so the replay plays those polls and then holds back
// the operation after the load's landing line until the card has shown a frame
// with no load pending. An operation other than a status poll or a command
// write between a FONT and its landing would make that checkpoint depend on
// the raster; the replay would report it.
//
// Each fixture starts from the trace's cold reset, which is RESET with
// power-on over the debug link: the RST pin performs §15, which leaves VRAM as
// it was, and a cold start's VRAM is zeroed (PLAN.md section 4).

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { REPO, loadVideo } from './emulator.mjs'
import { ACCESS, BLOCK_MAX, NanoError, SCRIPT_MAX, scriptPort } from './nano.mjs'
import { events, readTrace } from './trace.mjs'

const ORACLE = join(REPO, 'tests', 'oracle')
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))
const lost = (error) => error instanceof NanoError && error.message.includes('did not answer')

/**
 * After an answer the Nano never gave: let anything still in flight arrive, and
 * forget it, so the next request starts clean (Nano.request does the same
 * before its own retry).
 */
async function recover(nano) {
  await sleep(100)
  nano.buffer = Buffer.alloc(0)
}
const hex = (v, width = 2) => v.toString(16).padStart(width, '0')

const STATSEL_B = 0x0e
const STATSEL_A = 0x0f
const FONT = 0x30
/** §5: $02-$06 are $10-$12, $20 and $21, as the link's raw register file holds them. */
const ALIASES = { 2: 0x10, 3: 0x11, 4: 0x12, 5: 0x20, 6: 0x21 }
const VBANK = 0x08
const VINC = 0x09
/** What each operation is, for the counts: a status read compared is its STATSEL, 4 to 6. */
const KIND = { WRITE: 0, DATA: 1, STATUS: 2 }

/**
 * A trace as the Nano will play it: every operation as a SCRIPT pair, each read
 * carrying what it must return where that can be compared untimed; the
 * checkpoints, each at the operation count it falls at; and the FONT holds.
 *
 * Video.ts, played the same operations with no ticks, is only the decoder: it
 * says which status register each status read selects, and which command
 * writes are FONT. Its registers follow the program exactly, since none of
 * them moves with time; its VRAM does not past a FONT, and is not used.
 */
export function planTrace(trace, version) {
  const Video = loadVideo()
  const video = new Video()
  video.reset(true)

  const pairs = []
  const kinds = [] // KIND, one an operation
  const checkpoints = []
  const holds = []
  const problems = []
  let ops = 0
  let loading = null // a FONT load the trace has not seen land: { op, value, busy }

  for (const event of events(trace)) {
    switch (event.type) {
      case 'X':
        if (!event.cold) throw new Error(`${trace.header.fixture}: a warm reset cannot be played through the bus`)
        break
      case 'L':
        // §7, §14: the load lands at the line start where vertical blank fires.
        if (loading && event.displayLine === video.getMode().lines) {
          holds.push({ at: ops, font: loading.value, written: loading.op })
          if (loading.busy.length) {
            problems.push(`FONT $${hex(loading.value)} at operation ${loading.op} has ${loading.busy.length} operations before it lands that are not status polls or command writes, the first at ${loading.busy[0]}: this fixture depends on the raster`)
          }
          loading = null
        }
        break
      case 'W': {
        const port = event.port
        if (port & 1) {
          const state = video.portState(port & 2 ? 'b' : 'a')
          if (state.awaitingCommand && event.value === (0x80 | FONT) && (state.payload & 0x7f) === 0) {
            loading = { op: event.op, value: state.payload, busy: [] }
          }
        } else if (loading) {
          loading.busy.push(event.op)
        }
        video.write(port, event.value)
        pairs.push(ACCESS.WRITE | scriptPort(port), event.value)
        kinds.push(KIND.WRITE)
        ops++
        break
      }
      case 'R': {
        const port = event.port
        let expected = null
        let kind = KIND.DATA
        if (port & 1) {
          const select = video.getRegister(port & 2 ? STATSEL_B : STATSEL_A) & 0x0f
          if (select === 4 || select === 6) expected = event.value
          else if (select === 5) expected = version
          kind = expected === null ? KIND.STATUS : select
        } else {
          expected = event.value
          if (loading) loading.busy.push(event.op)
        }
        kinds.push(kind)
        video.read(port)
        if (expected === null) {
          pairs.push(ACCESS.READ_IGNORED | scriptPort(port), 0)
        } else {
          pairs.push(ACCESS.READ | scriptPort(port), expected)
        }
        ops++
        break
      }
      case 'C':
        checkpoints.push({ name: event.name, at: ops, ...event.annotations })
        break
    }
  }
  if (loading) holds.push({ at: ops, font: loading.value, written: loading.op })
  return { pairs: Uint8Array.from(pairs), kinds: Uint8Array.from(kinds), ops, checkpoints, holds, problems }
}

/**
 * All 64 KB of VRAM read through the bus, on pair B: VINC 1, VBANK 0 and a read
 * address of $0000, then data reads, the pointer carrying through the banks
 * (§4). It writes VBANK, VINC and the pair's port state, so it comes only after
 * the last comparison a fixture makes.
 */
async function busVram(nano) {
  // Writing the same registers and address again changes nothing, so this may
  // be retried; a block read may not, because it moves the pointer. A block
  // whose answer is lost is read again from its own address.
  const point = (address) =>
    nano.script([(address >> 14) & 3, 0x80 | VBANK, 0x01, 0x80 | VINC, address & 0xff, (address >> 8) & 0x3f].flatMap((v) => [ACCESS.WRITE | scriptPort(3), v]))
  await point(0)
  const out = Buffer.alloc(0x10000)
  let lostAnswers = 0
  for (let at = 0; at < out.length; at += BLOCK_MAX) {
    const n = Math.min(BLOCK_MAX, out.length - at)
    let block
    try {
      block = await nano.readBlock(2, n, { retry: false })
    } catch (error) {
      if (!lost(error)) throw error
      lostAnswers++
      await recover(nano)
      await point(at)
      block = await nano.readBlock(2, n, { retry: false })
    }
    block.copy(out, at)
  }
  return { vram: out, lostAnswers }
}

function differences(a, b) {
  let count = 0
  let first = -1
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      if (first < 0) first = i
      count++
    }
  }
  return { count, first }
}

/**
 * Play fixtures' traces through the Nano, at each timing profile, and compare
 * every static checkpoint. `target` is a fixture's name, `all`, or a trace's
 * path; `names` picks checkpoints (all of them if empty). Returns the results,
 * one a fixture a profile.
 */
export async function replayTraces({ nano, link, target, names = [], timings, out = null, log = console.log }) {
  const { CMD, decodeInfo, decodeSnapshot, decodeStats, packSnapshot, u8 } = await import('./link.mjs')
  const manifest = JSON.parse(readFileSync(join(ORACLE, 'manifest.json'), 'utf8'))
  const fixtures = target === 'all' ? manifest.fixtures.map((f) => f.name) : [target]
  const version = decodeInfo(await link.request(CMD.INFO)).version
  const snapshot = async () => decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 3000), 5000))
  const busStats = async (reset) => decodeStats(await link.request(CMD.STATS, u8(reset ? 1 : 0))).bus
  if (out) mkdirSync(out, { recursive: true })

  const results = []
  for (const fixture of fixtures) {
    const known = manifest.fixtures.some((f) => f.name === fixture)
    const path = known ? join(ORACLE, fixture, `${fixture}.vdpt.gz`) : resolve(fixture)
    const directory = known ? join(ORACLE, fixture) : resolve(path, '..')
    const trace = readTrace(readFileSync(path))
    const name = trace.header.fixture
    const plan = planTrace(trace, version)
    const wanted = plan.checkpoints.filter((c) => !names.length || names.includes(c.name))
    const dynamic = wanted.filter((c) => c.class !== 'static')
    const chosen = wanted.filter((c) => c.class === 'static')
    for (const c of dynamic) log(`  ${name}/${c.name} is dynamic: not playable untimed; its injection result stands for it`)
    for (const p of plan.problems) log(`  ${name}: ${p}`)

    // The stops, in operation order: a frame at each settle point, the state at
    // each checkpoint, and each FONT hold. A settle point behind a checkpoint
    // already passed would need a second pass from cold; the oracle has none,
    // and it is refused rather than played wrong.
    const stops = [
      ...chosen.map((c) => ({ at: c.settle, kind: 'frame', checkpoint: c })),
      ...chosen.map((c) => ({ at: c.at, kind: 'state', checkpoint: c })),
      ...plan.holds.map((h) => ({ at: h.at, kind: 'hold', hold: h })),
    ].sort((a, b) => a.at - b.at || (a.kind === 'hold' ? -1 : b.kind === 'hold' ? 1 : a.kind === 'frame' ? -1 : 1))
    for (const c of chosen) {
      if (c.settle > c.at) throw new Error(`${name}/${c.name}: settle point ${c.settle} is after the checkpoint, at ${c.at}`)
    }
    const last = stops.length ? stops.at(-1).at : 0

    for (const timing of timings) {
      await nano.profile(timing)
      await busStats(true)
      const began = Date.now()
      const result = {
        fixture: name, timing, operations: last, reads: 0, compared: { data: 0, stat4: 0, stat5: 0, stat6: 0 },
        mismatches: [], checkpoints: [], holds: [], dynamic: dynamic.map((c) => c.name), problems: [...plan.problems],
        lostAnswers: [],
      }

      // The cold reset (TRACE.md section 3), and nothing on the bus since.
      await link.request(CMD.RESET, u8(1))
      await sleep(20)

      let at = 0
      let taken = 0 // accesses the card has taken since its counts were cleared, as far as the replay knows
      let lastPrint = Date.now()
      const playTo = async (end) => {
        while (at < end) {
          const count = Math.min(SCRIPT_MAX, end - at)
          const pairs = plan.pairs.subarray(2 * at, 2 * (at + count))
          // A SCRIPT whose answer is lost is not sent again blindly: the card's
          // own count of the accesses it took says whether it ran (Phase 11
          // found that count exact). Run, its reads went uncompared; not run,
          // it is sent again.
          let answer
          try {
            answer = await nano.script(pairs, { retry: false })
          } catch (error) {
            if (!lost(error)) throw error
            await recover(nano)
            const bus = await busStats(false)
            const done = bus.writes + bus.reads - taken
            if (done === 0) {
              answer = await nano.script(pairs, { retry: false })
            } else if (done === count) {
              let unverified = 0
              for (let op = at; op < at + count; op++) if (plan.kinds[op] === KIND.DATA || plan.kinds[op] >= 4) unverified++
              result.lostAnswers.push({ op: at, count, ran: true, unverified })
              log(`  ${name}: the Nano's answer to operations ${at}-${at + count - 1} was lost; the card took all ${count}, ${unverified} of them compared reads left unchecked`)
              answer = { differed: 0, log: [] }
            } else {
              throw new Error(`${name}: the Nano's answer to operations ${at}-${at + count - 1} was lost, and the card took ${done} of the ${count}`)
            }
            if (done === 0) result.lostAnswers.push({ op: at, count, ran: false, unverified: 0 })
          }
          taken += count
          for (let op = at; op < at + count; op++) {
            const kind = plan.kinds[op]
            if (kind !== KIND.WRITE) result.reads++
            if (kind === KIND.DATA) result.compared.data++
            else if (kind >= 4) result.compared[`stat${kind}`]++
          }
          if (answer.differed) {
            for (const m of answer.log) {
              result.mismatches.push({ op: at + m.index, port: (pairs[2 * m.index] >> 2) & 3, expected: m.expected, got: m.got })
            }
            if (answer.differed > answer.log.length) result.mismatches.push({ op: at, more: answer.differed - answer.log.length })
            log(`  ${name} MISMATCH in operations ${at}-${at + count - 1}: ${answer.log.map((m) => `op ${at + m.index} port ${(pairs[2 * m.index] >> 2) & 3} expected $${hex(m.expected)} got $${hex(m.got)}`).join('; ')}`)
          }
          at += count
          if (Date.now() - lastPrint > 2000) {
            lastPrint = Date.now()
            process.stdout.write(`\r  ${name}, ${timing}: ${at.toLocaleString()}/${last.toLocaleString()} operations   `)
          }
        }
      }

      const records = new Map(chosen.map((c) => [c.name, { name: c.name, frame: c.frame, settle: c.settle, at: c.at, problems: [] }]))
      for (const stop of stops) {
        await playTo(stop.at)
        if (stop.kind === 'hold') {
          // A frame shown after the load was written has passed a vertical
          // blank, where the load lands (§7).
          await sleep(20)
          const s = await snapshot()
          const ok = s.state.fontPending === 0
          result.holds.push({ at: stop.at, font: stop.hold.font, written: stop.hold.written, landed: ok, rasterFrame: s.frame })
          if (!ok) result.problems.push(`FONT $${hex(stop.hold.font)} written at operation ${stop.hold.written} was still pending at raster frame ${s.frame}`)
          continue
        }
        const record = records.get(stop.checkpoint.name)
        if (stop.kind === 'frame') {
          // Wait out more than a line, so every row of the next frame to start
          // is latched after the last operation, then take it (§3).
          await sleep(20)
          const s = await snapshot()
          const golden = readFileSync(join(directory, `${stop.checkpoint.name}.idx.bin`))
          const frame = differences(s.rows, golden)
          const late = [...s.late].filter(Boolean).length
          record.rasterFrame = s.frame
          record.pixelsDiffering = frame.count
          if (frame.count) record.problems.push(`index frame: ${frame.count} pixels differ, the first at (${frame.first % 320}, ${Math.floor(frame.first / 320)})`)
          if (late) record.problems.push(`${late} late rows in the frame`)
          if (frame.count && out) writeFileSync(join(out, `${name}-${stop.checkpoint.name}-${timing}.idx.bin`), s.rows)
        } else {
          // Every access sent arrived, and none twice.
          const bus = await busStats(false)
          record.accessesTaken = bus.writes + bus.reads
          if (record.accessesTaken !== taken) record.problems.push(`the card took ${record.accessesTaken} accesses; the Nano made ${taken}`)
          const vram = await link.request(CMD.VRAM, Buffer.alloc(0), 10000)
          const goldenVram = readFileSync(join(directory, `${stop.checkpoint.name}.vram.bin`))
          const v = differences(vram, goldenVram)
          record.vramDiffering = v.count
          if (v.count) record.problems.push(`VRAM: ${v.count} bytes differ, the first at $${hex(v.first, 4)}: $${hex(vram[v.first])}, the golden $${hex(goldenVram[v.first])}`)
          const json = JSON.parse(readFileSync(join(directory, `${stop.checkpoint.name}.json`), 'utf8'))
          const state = (await snapshot()).state
          let registers = 0
          for (let i = 0; i < 128; i++) {
            const got = state.registers[ALIASES[i] ?? i]
            if (got === json.registers[i]) continue
            registers++
            record.problems.push(`register $${hex(i)}: $${hex(got)}, the golden $${hex(json.registers[i])}`)
          }
          record.registersDiffering = registers
          // STAT0 moves with the raster (§6), and is compared by injection only.
          record.stat0 = { card: state.stat0, golden: json.status }
          // The fixture's last comparison: VRAM once more, read through the pins.
          if (stop === stops.at(-1)) {
            const { vram: read, lostAnswers } = await busVram(nano)
            if (lostAnswers) result.lostAnswers.push({ readback: lostAnswers })
            const r = differences(read, goldenVram)
            record.busVramDiffering = r.count
            if (r.count) record.problems.push(`VRAM read through the bus: ${r.count} bytes differ, the first at $${hex(r.first, 4)}: $${hex(read[r.first])}, the golden $${hex(goldenVram[r.first])}`)
          }
        }
      }

      process.stdout.write('\r' + ' '.repeat(90) + '\r')
      result.seconds = (Date.now() - began) / 1000
      result.bus = await busStats(false)
      result.checkpoints = [...records.values()]
      results.push(result)

      const b = result.bus
      for (const r of result.checkpoints) {
        log(
          `  ${r.problems.length ? 'DIFFERS' : 'exact  '} ${name}/${r.name} (${timing}): frame from operation ${r.settle.toLocaleString()}, raster frame ${r.rasterFrame}; ` +
            `VRAM and registers at operation ${r.at.toLocaleString()}` +
            (r.busVramDiffering !== undefined ? ', and VRAM read back through the bus' : '') +
            (r.problems.length ? `\n          ${r.problems.slice(0, 12).join('\n          ')}` : '')
        )
      }
      const c = result.compared
      log(
        `  ${name}, ${timing}: ${result.operations.toLocaleString()} operations, ${result.reads.toLocaleString()} of them reads; compared ` +
          `${c.data.toLocaleString()} data, ${c.stat4} STAT4, ${c.stat5} STAT5, ${c.stat6} STAT6: ` +
          `${result.mismatches.length} wrong; ${result.holds.length} FONT holds; ${result.seconds.toFixed(1)} s`
      )
      log(`    card: ${b.writes} writes, ${b.reads} reads, ${b.staleData} stale data reads, ${b.staleStatus} stale status reads, ${b.coincident} coincident, FIFO overruns ${b.writeOverruns}/${b.readOverruns}, interrupt max ${b.isrMax} cycles`)
      if (result.lostAnswers.length) log(`    the Nano lost ${result.lostAnswers.length} answer(s), each recovered without repeating an access: ${JSON.stringify(result.lostAnswers)}`)
      for (const p of result.problems) log(`    ${p}`)
    }
  }
  return results
}

/**
 * Whether a set of results is a pass: every read right, every checkpoint exact,
 * no access lost on the bus, and no compared read left unchecked by an answer
 * the Nano lost.
 */
export function replayFailed(results) {
  return results.some(
    (r) =>
      r.mismatches.length || r.problems.length || r.checkpoints.some((c) => c.problems.length) || r.bus.writeOverruns || r.bus.readOverruns ||
      r.lostAnswers.some((l) => l.unverified)
  )
}
