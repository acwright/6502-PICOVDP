#!/usr/bin/env node

// Replay the pinned traces into the core, and check them against the goldens.
//
//   node tools/replay.mjs [fixture | path.vdpt.gz ...] [--classes] [--reference] [--out DIR]
//
// The host executor of PLAN.md section 4, on the Node path: the adapter
// (host/node/Video.cjs, or PICOVDP_ADDON) with no CPU, ticked to each event of a
// docs/TRACE.md trace, as the emulator's scripts/replay-trace.mjs ticks
// Video.ts. Resets, writes, reads and checkpoints are fed in at their ticks.
// Line starts, /INT and the values reads return are the card's side: the card
// makes them, and they are compared with the trace line for line. The first
// that differs stops that fixture.
//
// At each checkpoint its golden in tests/oracle is compared exactly: the index
// frame, all 64 KB of VRAM, and the structural JSON (registers, mode, STAT0,
// VRAM hash, text grid). So are the frame, settle point and window the trace
// records for it.
//
//   --classes     also decide each checkpoint's class again, by replaying to
//                 its settle point and letting the card run on (TRACE.md,
//                 "Classes"), and compare it with the trace's
//   --reference   replay into the emulator's compiled Video.ts instead: the
//                 harness's own check, which must pass
//   --out DIR     write each replayed checkpoint's .idx.bin, .vram.bin and
//                 .json under DIR/<fixture>/
//
// With no fixture named, every fixture in tests/oracle/manifest.json. Exits 1
// if anything differs.

import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { basename, join, relative, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { REPO, loadAdapter, loadVideo } from './lib/emulator.mjs'
import { FIRST_ROW_LATCH, LAST_ROW_LATCH, TraceRecorder, checkpointsOf, readTrace } from './lib/trace.mjs'

const ORACLE = join(REPO, 'tests', 'oracle')
const FRAME_BYTES = 320 * 240

function main() {
  const args = process.argv.slice(2)
  const option = (name) => {
    const at = args.indexOf(name)
    if (at < 0) return undefined
    const value = args[at + 1]
    if (value === undefined || value.startsWith('--')) fail(`${name} needs a value`)
    args.splice(at, 2)
    return value
  }
  const flag = (name) => {
    const at = args.indexOf(name)
    if (at >= 0) args.splice(at, 1)
    return at >= 0
  }
  const out = option('--out')
  const classes = flag('--classes')
  const reference = flag('--reference')
  const unknown = args.find((arg) => arg.startsWith('--'))
  if (unknown) fail(`unknown option ${unknown}`)

  const Video = reference ? loadVideo() : loadAdapter()
  const manifest = JSON.parse(readFileSync(join(ORACLE, 'manifest.json'), 'utf8'))
  const paths = (args.length ? args : manifest.fixtures.map((fixture) => fixture.name)).map((target) =>
    manifest.fixtures.some((fixture) => fixture.name === target)
      ? join(ORACLE, target, `${target}.vdpt.gz`)
      : resolve(target)
  )

  console.log(`replay: into ${reference ? 'Video.ts' : 'the core'}`)
  let failures = 0
  for (const path of paths) {
    if (!existsSync(path)) fail(`no trace at ${path}`)
    const trace = readTrace(readFileSync(path))
    const { fixture, emulator } = trace.header
    console.log(`${fixture} — ${relative(REPO, path)}, recorded at ${emulator.slice(0, 12)}`)
    const started = Date.now()
    const recorded = new Map(checkpointsOf(trace).map((checkpoint) => [checkpoint.name, checkpoint]))

    let checkpoints
    try {
      ;({ checkpoints } = replay(Video, trace))
    } catch (error) {
      if (!(error instanceof Divergence)) throw error
      console.log(`  FAILED  ${error.message}`)
      failures++
      continue
    }

    for (const checkpoint of checkpoints) {
      const problems = compare(fixture, checkpoint)
      const noted = recorded.get(checkpoint.name) ?? {}
      for (const key of ['frame', 'settle', 'window']) {
        if (noted[key] !== checkpoint[key]) problems.push(`${key} is ${checkpoint[key]}, the trace says ${noted[key]}`)
      }
      if (classes) {
        const { frozen } = replay(Video, trace, { settle: checkpoint.settle, frame: checkpoint.frame })
        checkpoint.class = differs(frozen, checkpoint.indices) === null ? 'static' : 'dynamic'
        if (noted.class !== checkpoint.class) problems.push(`class is ${checkpoint.class}, the trace says ${noted.class}`)
      }
      if (out) write(out, fixture, checkpoint)
      if (problems.length) failures++
      console.log(
        `  ${problems.length ? 'DIFFERS' : 'exact  '} ${fixture}/${checkpoint.name} — frame ${checkpoint.frame}, ` +
          `settle ${checkpoint.settle}${checkpoint.class ? `, ${checkpoint.class}` : ''}` +
          (problems.length ? `\n          ${problems.join('\n          ')}` : '')
      )
    }
    console.log(`  ${trace.lines.length} events, ${((Date.now() - started) / 1000).toFixed(1)}s`)
  }

  if (out) console.log(`wrote the replayed checkpoints under ${out}/`)
  console.log(failures === 0 ? 'every checkpoint replays exactly' : `${failures} failure(s)`)
  process.exit(failures === 0 ? 0 : 1)
}

export class Divergence extends Error {}

/** The text of an event line, less the annotations a C line carries. */
function bare(line) {
  const fields = line.split(' ')
  return fields[1] === 'C' ? fields.slice(0, 4).join(' ') : line
}

/**
 * Replay a trace into a fresh card, as the emulator's replayTrace does.
 *
 * Returns each checkpoint with its capture and where its golden frame was
 * built. With `freeze = { settle, frame }` it stops following the trace at
 * that frame's first-row latch, `settle` operations in, ticks on with nothing
 * more applied until the frame is presented, and returns that frame as
 * `frozen`.
 */
export function replay(Video, trace, freeze) {
  const { fixture, frequency } = trace.header
  const lines = trace.lines
  const video = new Video()
  const produced = new TraceRecorder(video)
  video.observer = produced

  let compared = 0
  const check = () => {
    for (; compared < produced.lines.length; compared++) {
      const actual = produced.lines[compared]
      const expected = lines[compared] === undefined ? '(the end of the trace)' : bare(lines[compared])
      if (actual !== expected) {
        throw new Divergence(`${fixture}: event ${compared + 1} is "${expected}", the replay made "${actual}"`)
      }
    }
  }

  let tick = 0
  let ops = 0
  let frame = 0
  let latch = null
  let presented = null
  let frozeAtLatch = false
  const checkpoints = []

  // An input happens at its tick; ticking there must not make an event the
  // trace does not have first.
  const advanceTo = (index) => {
    while (video.tickCount < tick) {
      video.tick(frequency)
      if (produced.lines.length > index) check()
    }
  }

  for (let index = 0; index < lines.length && !frozeAtLatch; index++) {
    const fields = lines[index].split(' ')
    tick += Number(fields[0])

    switch (fields[1]) {
      case 'X':
        if (index !== 0 || fields[2] !== 'cold') throw new Error(`${fixture}: event ${index + 1}: only a leading cold reset replays`)
        video.reset(true)
        break
      case 'L':
      case 'I':
        while (produced.lines.length <= index && video.tickCount < tick) video.tick(frequency)
        if (produced.lines.length <= index) {
          throw new Divergence(`${fixture}: event ${index + 1} is "${lines[index]}", the replay made nothing by tick ${video.tickCount}`)
        }
        break
      case 'W':
      case 'R':
        advanceTo(index)
        if (fields[1] === 'W') video.write(Number(fields[2]), parseInt(fields[3], 16))
        else video.read(Number(fields[2]))
        ops++
        break
      case 'C':
        advanceTo(index)
        produced.checkpoint(fields[2])
        check()
        if (!presented) throw new Error(`${fixture}/${fields[2]}: no complete frame before it`)
        checkpoints.push({ name: fields[2], cycles: tick, ...capture(video, tick), ...presented })
        break
      default:
        throw new Error(`${fixture}: event ${index + 1}: unknown event "${lines[index]}"`)
    }
    check()

    if (fields[1] === 'L') {
      const screenLine = Number(fields[2])
      if (screenLine === 0) frame++
      if (screenLine === FIRST_ROW_LATCH) {
        latch = { settle: ops }
        if (freeze && freeze.frame === frame + 1 && freeze.settle === ops) frozeAtLatch = true
      }
      if (screenLine === LAST_ROW_LATCH && latch) {
        presented = { frame, settle: latch.settle, window: ops - latch.settle }
        latch = null
      }
    }
  }

  if (!freeze) return { checkpoints }
  if (!frozeAtLatch) throw new Error(`${fixture}: no first-row latch of frame ${freeze.frame} after ${freeze.settle} operations`)

  // The program stops here; the card scans on.
  let done = false
  video.observer = {
    read() {},
    write() {},
    reset() {},
    lineStart(screenLine) {
      if (screenLine === 0) frame++
      if (screenLine === LAST_ROW_LATCH && frame === freeze.frame) done = true
    }
  }
  const limit = video.tickCount + 2 * Math.ceil(frequency / 60)
  while (!done && video.tickCount < limit) video.tick(frequency)
  if (!done) throw new Error(`${fixture}: frame ${freeze.frame} was never presented`)
  return { checkpoints, frozen: Uint8Array.from(video.frameIndices()) }
}

/** What a golden holds, read off the card without disturbing it — the emulator's captureState. */
export function capture(video, cycles) {
  const vram = new Uint8Array(video.vramSize)
  for (let address = 0; address < vram.length; address++) vram[address] = video.readVRAM(address)
  const registers = []
  for (let register = 0; register < 128; register++) registers.push(video.getRegister(register))
  return {
    structural: {
      cycles,
      mode: video.getMode(),
      displayEnabled: video.isDisplayEnabled(),
      status: video.getStatus(),
      registers,
      vramSha256: createHash('sha256').update(vram).digest('hex'),
      textGrid: video.textGrid()
    },
    vram,
    indices: Uint8Array.from(video.frameIndices())
  }
}

function compare(fixture, checkpoint) {
  const base = join(ORACLE, fixture, checkpoint.name)
  const problems = []
  const json = JSON.parse(readFileSync(`${base}.json`, 'utf8'))
  if (JSON.stringify(checkpoint.structural) !== JSON.stringify(json)) {
    const keys = Object.keys(json).filter((key) => JSON.stringify(json[key]) !== JSON.stringify(checkpoint.structural[key]))
    problems.push(`JSON differs in ${keys.join(', ')}`)
  }
  const vram = differs(checkpoint.vram, new Uint8Array(readFileSync(`${base}.vram.bin`)))
  if (vram) problems.push(`VRAM: ${vram.count} byte(s), first at $${vram.first.toString(16).padStart(4, '0')}`)
  const golden = new Uint8Array(readFileSync(`${base}.idx.bin`))
  if (golden.length !== FRAME_BYTES) throw new Error(`${base}.idx.bin is ${golden.length} bytes`)
  const frame = differs(checkpoint.indices, golden)
  if (frame) {
    const x = frame.first % 320
    const y = (frame.first - x) / 320
    problems.push(
      `index frame: ${frame.count} pixel(s), first at (${x}, ${y}): ${checkpoint.indices[frame.first]}, the golden ${golden[frame.first]}`
    )
  }
  return problems
}

/** Where two byte arrays of one length differ: null, or the count and the first index. */
export function differs(actual, expected) {
  if (actual.length !== expected.length) return { count: Math.abs(actual.length - expected.length), first: 0 }
  let count = 0
  let first = -1
  for (let i = 0; i < actual.length; i++) {
    if (actual[i] === expected[i]) continue
    if (first < 0) first = i
    count++
  }
  return count ? { count, first } : null
}

function write(out, fixture, checkpoint) {
  const base = join(resolve(out), fixture, basename(checkpoint.name))
  mkdirSync(join(resolve(out), fixture), { recursive: true })
  writeFileSync(`${base}.json`, JSON.stringify(checkpoint.structural, null, 2) + '\n')
  writeFileSync(`${base}.vram.bin`, checkpoint.vram)
  writeFileSync(`${base}.idx.bin`, checkpoint.indices)
}

function fail(message) {
  console.error(`replay: ${message}`)
  process.exit(2)
}

// A tool when run, a library when imported: tools/card.mjs replays a card it
// has just drawn through the core, exactly as this replays a fixture.
if (import.meta.url === pathToFileURL(process.argv[1]).href) main()
