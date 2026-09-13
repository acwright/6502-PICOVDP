// VDP port traces, docs/TRACE.md format version 1: read, check, write.
//
// Written from TRACE.md rather than copied from the emulator's
// src/tests/goldens/traces.js, so that two readers agree on what the document
// says. The emulator's is the reference; this is the firmware project's.

import { gzipSync, gunzipSync } from 'node:zlib'

export const TRACE_VERSION = 1
export const SCREEN_LINES = 262
/** The line starts that build a frame's first and last rows (TRACE.md, "Frames"). */
export const FIRST_ROW_LATCH = 261
export const LAST_ROW_LATCH = 238

const HEADER_END = '---'

/** Decompress and split a trace. Checks the envelope, not the events. */
export function readTrace(file) {
  const all = gunzipSync(file).toString('utf8').split('\n')
  if (all.at(-1) === '') all.pop()

  const [magic, version] = (all[0] ?? '').split(' ')
  if (magic !== 'vdpt') throw new Error('not a VDP trace')
  if (Number(version) !== TRACE_VERSION) throw new Error(`trace version ${version}; this reads ${TRACE_VERSION}`)

  const header = {}
  let index = 1
  for (; index < all.length && all[index] !== HEADER_END; index++) {
    const line = all[index]
    const space = line.indexOf(' ')
    if (space <= 0) throw new Error(`header line ${index + 1} is not "key value": ${line}`)
    header[line.slice(0, space)] = line.slice(space + 1)
  }
  if (index === all.length) throw new Error('the header never ends')
  for (const key of ['fixture', 'emulator', 'frequency']) {
    if (header[key] === undefined) throw new Error(`the header has no ${key}`)
  }
  header.frequency = Number(header.frequency)

  const footer = all.pop()
  const lines = all.slice(index + 1)
  if (footer !== `end ${lines.length}`) throw new Error(`truncated: ends "${footer}" after ${lines.length} events`)
  return { header, lines }
}

export function writeTrace({ header, lines }) {
  const head = [`vdpt ${TRACE_VERSION}`, `fixture ${header.fixture}`, `emulator ${header.emulator}`, `frequency ${header.frequency}`]
  const text = `${head.join('\n')}\n${HEADER_END}\n${lines.length ? lines.join('\n') + '\n' : ''}end ${lines.length}\n`
  return gzipSync(Buffer.from(text, 'utf8'), { level: 9 })
}

/**
 * Every event with what TRACE.md derives for it: absolute `tick`, `frame`,
 * `screenLine`, `displayLine` and `tickInLine` (ticks since the line start the
 * event falls in), and `op`, the index a read or write has among them.
 *
 * Throws at the first event that breaks a rule of the format.
 */
export function* events(trace) {
  let tick = 0
  let frame = 0
  let screenLine = -1
  let displayLine = -1
  let lineTick = 0
  let interrupt = 0
  let ops = 0

  for (let index = 0; index < trace.lines.length; index++) {
    const line = trace.lines[index]
    const fields = line.split(' ')
    const fail = (why) => {
      throw new Error(`${trace.header.fixture}: event ${index + 1} ("${line}"): ${why}`)
    }
    if (!/^\d+$/.test(fields[0] ?? '')) fail('no tick delta')
    tick += Number(fields[0])
    const type = fields[1]
    if (index === 0 && type !== 'X') fail('a trace starts with a reset')

    const event = { index, tick, type }
    switch (type) {
      case 'X':
        if (fields.length !== 4 || !['cold', 'warm'].includes(fields[2])) fail('expected X cold|warm <screen line>')
        if (index === 0 ? fields[2] !== 'cold' || tick !== 0 : fields[2] === 'cold') {
          fail('version 1 has exactly one cold reset, at tick 0, first')
        }
        event.cold = fields[2] === 'cold'
        event.screenLine = Number(fields[3])
        if (event.cold) {
          screenLine = event.screenLine
          // A cold start's geometry is always Compact's (§15), 24 lines down.
          displayLine = (screenLine - 24 + SCREEN_LINES) % SCREEN_LINES
          lineTick = tick
          interrupt = 0
        }
        break
      case 'L': {
        if (fields.length !== 4) fail('expected L <screen line> <display line>')
        const next = (screenLine + 1) % SCREEN_LINES
        event.screenLine = Number(fields[2])
        event.displayLine = Number(fields[3])
        if (event.screenLine !== next) fail(`line starts run in order: expected screen line ${next}`)
        if (!(event.displayLine >= 0 && event.displayLine < SCREEN_LINES)) fail('display line out of range')
        if (event.screenLine === 0) frame++
        screenLine = event.screenLine
        displayLine = event.displayLine
        lineTick = tick
        break
      }
      case 'W':
      case 'R':
        if (fields.length !== 4 || !/^[0-3]$/.test(fields[2]) || !/^[0-9a-f]{2}$/.test(fields[3])) {
          fail(`expected ${type} <port 0-3> <two hex digits>`)
        }
        event.port = Number(fields[2])
        event.value = parseInt(fields[3], 16)
        event.op = ops++
        break
      case 'I':
        if (fields.length !== 3 || !/^[01]$/.test(fields[2])) fail('expected I 0|1')
        event.level = Number(fields[2])
        if (event.level === interrupt) fail('/INT is written only when it changes')
        interrupt = event.level
        break
      case 'C': {
        if (fields.length < 4 || !/^\d+$/.test(fields[3])) fail('expected C <name> <cycles> [key=value ...]')
        event.name = fields[2]
        event.cycles = Number(fields[3])
        if (event.cycles !== tick) fail(`a checkpoint's cycles are its tick, ${tick}`)
        event.annotations = {}
        for (const field of fields.slice(4)) {
          const [key, value, extra] = field.split('=')
          if (value === undefined || extra !== undefined) fail(`"${field}" is not key=value`)
          event.annotations[key] = key === 'class' ? value : Number(value)
        }
        break
      }
      default:
        fail(`unknown event type ${type}`)
    }
    event.frame = frame
    event.screenLine ??= screenLine
    event.displayLine ??= displayLine
    event.tickInLine = tick - lineTick
    yield event
  }
}

/** The checkpoints, in order, with their annotations. Walks and checks the whole trace. */
export function checkpointsOf(trace) {
  const found = []
  for (const event of events(trace)) {
    if (event.type === 'C') found.push({ name: event.name, cycles: event.cycles, ...event.annotations })
  }
  return found
}

/** A Video observer that writes TRACE.md's event lines, as the emulator's recorder does. */
export class TraceRecorder {
  constructor(video) {
    this.video = video
    this.lines = []
    this.lastTick = 0
    this.interrupt = 0
  }

  push(type, fields) {
    const tick = this.video.tickCount
    this.lines.push(`${tick - this.lastTick} ${type} ${fields}`)
    this.lastTick = tick
  }

  sampleInterrupt() {
    const level = this.video.peekStatus(1) !== 0 ? 1 : 0
    if (level !== this.interrupt) {
      this.interrupt = level
      this.push('I', level)
    }
  }

  read(port, value) {
    this.push('R', `${port} ${value.toString(16).padStart(2, '0')}`)
    this.sampleInterrupt()
  }

  write(port, value) {
    this.push('W', `${port} ${value.toString(16).padStart(2, '0')}`)
    this.sampleInterrupt()
  }

  lineStart(screenLine, displayLine) {
    this.push('L', `${screenLine} ${displayLine}`)
    this.sampleInterrupt()
  }

  reset(coldStart, screenLine) {
    if (coldStart) this.lastTick = 0
    this.push('X', `${coldStart ? 'cold' : 'warm'} ${screenLine}`)
    this.sampleInterrupt()
  }
}
