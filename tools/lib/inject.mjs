// vdpctl inject: the injection executor's host side (PLAN.md section 4,
// docs/DEBUGLINK.md's INJECT, docs/TRACE.md section 5).
//
// Each checkpoint is replayed on the board from the trace's cold reset: the
// program's operations up to the checkpoint, each addressed by the (frame,
// screen line) it fell in and carrying the value a read must return and the
// level /INT must be at after it. The board captures the golden frame as it
// goes to VGA, and at the checkpoint records its registers and status. Then the
// frame, VRAM, the registers and STAT0 are compared with the golden.

import { readFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { REPO } from './emulator.mjs'
import { CMD, Link, decodeInjectStatus, decodeSnapshot, packSnapshot } from './link.mjs'
import { checkpointsOf, events, readTrace } from './trace.mjs'

const ORACLE = join(REPO, 'tests', 'oracle')
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

const LINE = 0x01
const COLD = 0x02
const END = 0x03
const OP = 0x80
const CHUNK = 16000
const RECORD_BYTES = (type) => (type & OP ? 2 : type === LINE ? 6 : type === COLD ? 3 : 1)

/** The stream for one checkpoint: every program event before it (TRACE.md), as records. */
export function encode(trace, checkpointName) {
  const bytes = []
  let level = 0 // /INT
  let line = null // { frame, screen, interrupt, emitted, changed }
  let lastOp = -1 // offset of the last op record, to patch its /INT bit
  let afterLatch = false // the events since the last L or X are /INT's at the latch

  const emitLine = () => {
    if (!line || line.emitted) return
    bytes.push(LINE, line.frame & 0xff, line.frame >> 8, line.screen & 0xff, line.screen >> 8, line.interrupt)
    line.emitted = true
  }

  for (const event of events(trace)) {
    switch (event.type) {
      case 'X':
        if (!event.cold) throw new Error(`${trace.header.fixture}: a warm reset is not injected`)
        bytes.push(COLD, event.screenLine & 0xff, event.screenLine >> 8)
        level = 0
        line = { frame: 0, screen: event.screenLine, interrupt: 0, emitted: false, changed: false }
        afterLatch = true
        break
      case 'L':
        // A line whose latch moved /INT is checked even with no operations.
        if (line && line.changed) emitLine()
        line = { frame: event.frame, screen: event.screenLine, interrupt: level, emitted: false, changed: false }
        afterLatch = true
        break
      case 'I':
        level = event.level
        if (afterLatch) {
          line.interrupt = level
          line.changed = true
        } else if (lastOp >= 0) {
          bytes[lastOp] = (bytes[lastOp] & ~0x20) | (level ? 0x20 : 0)
        }
        break
      case 'W':
      case 'R':
        emitLine()
        afterLatch = false
        lastOp = bytes.length
        bytes.push(OP | (event.type === 'R' ? 0x40 : 0) | (level ? 0x20 : 0) | event.port, event.value)
        break
      case 'C':
        if (event.name === checkpointName) {
          emitLine()
          bytes.push(END)
          return Buffer.from(bytes)
        }
        break
    }
  }
  throw new Error(`${trace.header.fixture}: no checkpoint ${checkpointName}`)
}

async function stream(link, data, onStatus) {
  let at = 0
  let status = decodeInjectStatus(await link.request(CMD.INJECT, Buffer.from([2])))
  while (at < data.length) {
    if (status.state === 'failed') return status
    const room = Math.min(CHUNK, status.space)
    // Whole records only.
    let end = at
    while (end < data.length && end - at + RECORD_BYTES(data[end]) <= room) end += RECORD_BYTES(data[end])
    if (end === at) {
      await sleep(5)
      status = decodeInjectStatus(await link.request(CMD.INJECT, Buffer.from([2])))
      onStatus(status, at)
      continue
    }
    status = decodeInjectStatus(await link.request(CMD.INJECT, Buffer.concat([Buffer.from([1]), data.subarray(at, end)])))
    at += status.taken
    onStatus(status, at)
  }
  return status
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

export async function injectCheckpoints(target, names, { out } = {}) {
  const manifest = JSON.parse(readFileSync(join(ORACLE, 'manifest.json'), 'utf8'))
  const fixtures = target === 'all' ? manifest.fixtures.map((f) => f.name) : [target]
  const link = await Link.open()
  let failures = 0
  const results = []
  try {
    for (const fixture of fixtures) {
      const known = manifest.fixtures.some((f) => f.name === fixture)
      const path = known ? join(ORACLE, fixture, `${fixture}.vdpt.gz`) : resolve(fixture)
      const directory = known ? join(ORACLE, fixture) : resolve(path, '..')
      const trace = readTrace(readFileSync(path))
      const checkpoints = checkpointsOf(trace).filter((c) => !names.length || names.includes(c.name))
      for (const checkpoint of checkpoints) {
        const started = Date.now()
        const data = encode(trace, checkpoint.name)
        await link.request(CMD.INJECT, Buffer.from([3])).catch(() => {}) // ABORT anything before
        await sleep(100)
        const begin = Buffer.from([0, checkpoint.frame & 0xff, checkpoint.frame >> 8])
        await link.request(CMD.INJECT, begin)
        let lastPrint = 0
        let status = await stream(link, data, (s, at) => {
          if (Date.now() - lastPrint > 2000) {
            lastPrint = Date.now()
            process.stdout.write(`\r  ${trace.header.fixture}/${checkpoint.name}: ${at}/${data.length} bytes sent, trace frame ${s.frame}   `)
          }
        })
        const deadline = Date.now() + 30000 + checkpoint.frame * 40
        while (status.state !== 'ended' && status.state !== 'failed' && Date.now() < deadline) {
          await sleep(100)
          status = decodeInjectStatus(await link.request(CMD.INJECT, Buffer.from([2])))
        }
        process.stdout.write('\r' + ' '.repeat(100) + '\r')
        const problems = []
        if (status.state !== 'ended') problems.push(`the injection ${status.state} at trace frame ${status.frame}, line ${status.screenLine}`)
        for (const m of status.log) {
          problems.push(`${m.kind} at op ${m.op}, frame ${m.frame} line ${m.screenLine}: port ${m.port}${m.select !== 0xff ? ` (STAT${m.select})` : ''}, expected $${m.expected.toString(16)}, got $${m.got.toString(16)}`)
        }
        if (status.mismatches > status.log.length) problems.push(`${status.mismatches - status.log.length} more mismatches`)

        let snapshot = null
        if (status.state === 'ended') {
          snapshot = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(2, 0, 5000), 10000))
          const golden = readFileSync(join(directory, `${checkpoint.name}.idx.bin`))
          const frame = differences(snapshot.rows, golden)
          if (frame.count) problems.push(`index frame: ${frame.count} pixels differ, first at (${frame.first % 320}, ${Math.floor(frame.first / 320)})`)
          const late = [...snapshot.late].filter(Boolean).length
          if (late) problems.push(`${late} late rows in the golden frame`)
          if (snapshot.frame !== status.baseFrame + checkpoint.frame) problems.push(`captured raster frame ${snapshot.frame}, not ${status.baseFrame + checkpoint.frame}`)

          const vram = await link.request(CMD.VRAM, Buffer.alloc(0), 10000)
          const goldenVram = readFileSync(join(directory, `${checkpoint.name}.vram.bin`))
          const vramDiff = differences(vram, goldenVram)
          if (vramDiff.count) problems.push(`VRAM: ${vramDiff.count} bytes differ, first at $${vramDiff.first.toString(16)}`)

          const json = JSON.parse(readFileSync(join(directory, `${checkpoint.name}.json`), 'utf8'))
          // §5: $02-$06 are $10-$12, $20 and $21, read through their aliases.
          const ALIASES = { 2: 0x10, 3: 0x11, 4: 0x12, 5: 0x20, 6: 0x21 }
          const registers = Array.from({ length: 128 }, (_, i) => status.endState.registers[ALIASES[i] ?? i])
          for (let i = 0; i < 128; i++) {
            if (registers[i] !== json.registers[i]) problems.push(`register $${i.toString(16)}: $${registers[i].toString(16)}, the golden $${json.registers[i].toString(16)}`)
          }
          if (status.endState.stat0 !== json.status) problems.push(`STAT0 $${status.endState.stat0.toString(16)}, the golden $${json.status.toString(16)}`)
        }
        await link.request(CMD.INJECT, Buffer.from([3]))

        const seconds = ((Date.now() - started) / 1000).toFixed(1)
        if (problems.length) failures++
        results.push({ fixture: trace.header.fixture, checkpoint: checkpoint.name, frame: checkpoint.frame, ops: status.ops, reads: status.reads, stat5: status.stat5Reads, bytes: data.length, seconds: Number(seconds), problems })
        console.log(
          `  ${problems.length ? 'DIFFERS' : 'exact  '} ${trace.header.fixture}/${checkpoint.name} — frame ${checkpoint.frame}, ` +
            `${status.ops} operations (${status.reads} reads, ${status.stat5Reads} of STAT5 not compared), ${data.length} bytes, ${seconds} s` +
            (problems.length ? `\n          ${problems.slice(0, 12).join('\n          ')}` : '')
        )
      }
    }
  } finally {
    link.close()
  }
  if (out) {
    const { writeFileSync } = await import('node:fs')
    writeFileSync(out, JSON.stringify(results, null, 1))
  }
  console.log(failures ? `${failures} checkpoint(s) differ` : 'every checkpoint reproduces exactly on the board')
  return failures
}
