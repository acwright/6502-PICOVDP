#!/usr/bin/env node

// Draw a bench card (tools/lib/cards.mjs) into a trace and a golden of the
// oracle's shape, so that `vdpctl inject` replays it on the board like any
// other checkpoint (PLAN.md Phase 10).
//
//   node tools/card.mjs [palette|dac|all] [--out DIR] [--png]
//   node tools/card.mjs [palette|dac|all] --check
//
// The picture comes from the reference — the emulator's compiled `Video.ts`,
// ground rule 1 — and is then replayed through this repository's core, which
// must produce the same events, the same frame, the same VRAM and the same
// registers. A card is written only if both agree, and only if its checkpoint
// is static: the card's program stops, and the picture it leaves does not
// move, so what a capture sees at leisure is what the golden holds.
//
// Cards are not oracle goldens. They are this repository's own pictures, kept
// in bench/cards/, and nothing in tests/oracle depends on them.
//
// `--check` replays the cards as they stand through the core alone and holds
// them to the goldens beside them, which needs no emulator (CTest
// `cards_pinned`).

import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { join, relative, resolve } from 'node:path'
import { REPO, loadAdapter, loadVideo } from './lib/emulator.mjs'
import { CARDS, Writer } from './lib/cards.mjs'
import { FIRST_ROW_LATCH, LAST_ROW_LATCH, TraceRecorder, writeTrace } from './lib/trace.mjs'
import { capture, differs, replay } from './replay.mjs'
import { readTrace } from './lib/trace.mjs'
import { encodePng } from './lib/png.mjs'

const FREQUENCY = 1000000   // the oracle's traces are of a 1 MHz bus
const TICKS_PER_OP = 8      // 8 µs between port operations: slower than §4 asks
const SETTLE_FRAMES = 3     // frames the program idles for, so the picture is complete and still

function main() {
  const args = process.argv.slice(2)
  const option = (name, fallback) => {
    const at = args.indexOf(name)
    if (at < 0) return fallback
    const value = args[at + 1]
    if (value === undefined || value.startsWith('--')) fail(`${name} needs a value`)
    args.splice(at, 2)
    return value
  }
  const png = args.includes('--png')
  if (png) args.splice(args.indexOf('--png'), 1)
  const check = args.includes('--check')
  if (check) args.splice(args.indexOf('--check'), 1)
  const out = resolve(option('--out', join(REPO, 'bench', 'cards')))
  const unknown = args.find((arg) => arg.startsWith('--'))
  if (unknown) fail(`unknown option ${unknown}`)
  const names = !args.length || args[0] === 'all' ? Object.keys(CARDS) : args
  for (const name of names) if (!CARDS[name]) fail(`no card "${name}" (have ${Object.keys(CARDS).join(', ')})`)

  if (check) return verify(out, names)

  const Video = loadVideo()
  const Core = loadAdapter()
  mkdirSync(out, { recursive: true })
  for (const name of names) {
    const card = CARDS[name]
    const w = new Writer()
    card.draw(w)
    console.log(`${name} — ${card.title}`)
    console.log(`  ${w.ops.length} port operations, ${(w.ops.length * TICKS_PER_OP / FREQUENCY * 1000).toFixed(0)} ms at ${TICKS_PER_OP} µs each`)

    const drawn = draw(Video, name, card, w.ops)
    const file = join(out, `${name}.vdpt.gz`)
    writeFileSync(file, writeTrace(drawn.trace))

    // The core must replay what the reference drew, event for event.
    const trace = readTrace(writeTrace(drawn.trace))
    const { checkpoints } = replay(Core, trace)
    if (checkpoints.length !== 1) fail(`${name}: the replay found ${checkpoints.length} checkpoints`)
    const core = checkpoints[0]
    const problems = []
    const frame = differs(core.indices, drawn.checkpoint.indices)
    if (frame) problems.push(`the core's frame differs in ${frame.count} pixel(s), first at ${frame.first}`)
    const vram = differs(core.vram, drawn.checkpoint.vram)
    if (vram) problems.push(`the core's VRAM differs in ${vram.count} byte(s), first at $${vram.first.toString(16)}`)
    if (JSON.stringify(core.structural) !== JSON.stringify(drawn.checkpoint.structural)) {
      problems.push('the core\'s registers, mode or status differ')
    }
    for (const key of ['frame', 'settle', 'window']) {
      if (core[key] !== drawn.annotations[key]) problems.push(`the core puts ${key} at ${core[key]}, the reference at ${drawn.annotations[key]}`)
    }
    // Static: the program stops at the checkpoint's settle point and the card
    // scans on; the frame it then presents must be the golden (TRACE.md).
    const { frozen } = replay(Core, trace, { settle: drawn.annotations.settle, frame: drawn.annotations.frame })
    if (differs(frozen, drawn.checkpoint.indices)) problems.push('the checkpoint is not static')
    if (problems.length) fail(`${name}:\n  ${problems.join('\n  ')}`)

    const base = join(out, name)
    writeFileSync(`${base}.idx.bin`, drawn.checkpoint.indices)
    writeFileSync(`${base}.vram.bin`, drawn.checkpoint.vram)
    writeFileSync(`${base}.json`, JSON.stringify(drawn.checkpoint.structural, null, 2) + '\n')
    if (png) writeFileSync(`${base}.png`, encodePng(320, 240, rgba(drawn.checkpoint)))
    console.log(
      `  frame ${drawn.annotations.frame}, settle ${drawn.annotations.settle}, static; ` +
      `the core agrees\n  wrote ${relative(REPO, file)} and its golden${png ? ', with a PNG to look at' : ''}`
    )
  }
}

/**
 * The cards as they stand: each trace replayed through the core, against the
 * golden beside it, and checked to be static. Needs no emulator — this is what
 * says the files in bench/cards still belong together (CTest `cards_pinned`).
 */
function verify(directory, names) {
  const Core = loadAdapter()
  let failures = 0
  for (const name of names) {
    const trace = readTrace(readFileSync(join(directory, `${name}.vdpt.gz`)))
    const problems = []
    let checkpoint = null
    try {
      const { checkpoints } = replay(Core, trace)
      if (checkpoints.length !== 1) problems.push(`the replay found ${checkpoints.length} checkpoints`)
      checkpoint = checkpoints[0]
    } catch (error) {
      problems.push(error.message)
    }
    if (checkpoint) {
      const base = join(directory, name)
      const frame = differs(checkpoint.indices, new Uint8Array(readFileSync(`${base}.idx.bin`)))
      if (frame) problems.push(`the frame differs in ${frame.count} pixel(s), first at ${frame.first}`)
      const vram = differs(checkpoint.vram, new Uint8Array(readFileSync(`${base}.vram.bin`)))
      if (vram) problems.push(`VRAM differs in ${vram.count} byte(s), first at $${vram.first.toString(16)}`)
      if (JSON.stringify(checkpoint.structural) !== JSON.stringify(JSON.parse(readFileSync(`${base}.json`, 'utf8')))) {
        problems.push('the registers, mode or status differ')
      }
      const { frozen } = replay(Core, trace, { settle: checkpoint.settle, frame: checkpoint.frame })
      if (differs(frozen, checkpoint.indices)) problems.push('the checkpoint is no longer static')
    }
    if (problems.length) failures++
    console.log(`  ${problems.length ? 'DIFFERS' : 'exact  '} ${name}${problems.length ? `\n          ${problems.join('\n          ')}` : ''}`)
  }
  console.log(failures ? `${failures} card(s) differ` : 'every bench card replays to its golden through the core')
  process.exit(failures ? 1 : 0)
}

/** Run a card's operations into a fresh card, recording the trace as the emulator's recorder does. */
function draw(Video, name, card, ops) {
  const video = new Video()
  const recorder = new TraceRecorder(video)
  video.observer = recorder
  video.reset(true)

  let done = 0
  let frame = 0
  let latch = null
  let presented = null
  let opsSoFar = 0
  // Line starts are the card's own; the recorder writes them. Frames, settle
  // points and windows are counted here as tools/replay.mjs counts them.
  const watch = {
    ...recorder,
    lineStart(screenLine, displayLine) {
      recorder.lineStart(screenLine, displayLine)
      if (screenLine === 0) frame++
      if (screenLine === FIRST_ROW_LATCH) latch = { settle: opsSoFar }
      if (screenLine === LAST_ROW_LATCH && latch) {
        presented = { frame, settle: latch.settle, window: opsSoFar - latch.settle }
        latch = null
      }
    },
    read: (...a) => recorder.read(...a),
    write: (...a) => recorder.write(...a),
    reset: (...a) => recorder.reset(...a),
    checkpoint: (...a) => recorder.checkpoint(...a),
  }
  video.observer = watch

  const tickTo = (tick) => { while (video.tickCount < tick) video.tick(FREQUENCY) }
  let tick = 0
  for (const [port, value] of ops) {
    tick += TICKS_PER_OP
    tickTo(tick)
    video.write(port, value)
    opsSoFar++
  }
  // Idle until the card has presented whole frames with nothing changing.
  const until = presented === null ? 0 : presented.frame + SETTLE_FRAMES
  while (presented === null || presented.frame < until) { video.tick(FREQUENCY); done++ }
  tickTo(video.tickCount)

  recorder.checkpoint(card.checkpoint)
  const checkpoint = capture(video, video.tickCount)
  const annotations = { ...presented, class: 'static' }
  // The checkpoint line carries what tools/replay.mjs checks it against.
  recorder.lines[recorder.lines.length - 1] +=
    ` frame=${annotations.frame} settle=${annotations.settle} window=${annotations.window} class=static`
  return {
    trace: { header: { fixture: name, emulator: 'tools/card.mjs', frequency: FREQUENCY }, lines: recorder.lines },
    checkpoint,
    annotations,
  }
}

/** The golden's indices through its own palette, as the emulator's PNGs are written. */
function rgba(checkpoint) {
  const base = (checkpoint.structural.registers[0x0c] & 0x3f) << 10
  const out = new Uint8Array(320 * 240 * 4)
  for (let i = 0; i < 320 * 240; i++) {
    const entry = checkpoint.indices[i]
    const at = (base + 2 * entry) & 0xffff
    const red = checkpoint.vram[at] & 0x0f
    const green = (checkpoint.vram[(at + 1) & 0xffff] >> 4) & 0x0f
    const blue = checkpoint.vram[(at + 1) & 0xffff] & 0x0f
    out[i * 4] = red * 17
    out[i * 4 + 1] = green * 17
    out[i * 4 + 2] = blue * 17
    out[i * 4 + 3] = 0xff
  }
  return out
}

function fail(message) {
  console.error(`card: ${message}`)
  process.exit(2)
}

main()
