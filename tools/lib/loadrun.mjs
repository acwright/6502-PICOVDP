// Phase 13's load run (PLAN.md): everything at once, for as long as it takes.
//
//   - a worst-case scene (firmware/scenes.h), its program on one port pair,
//     with FONT written for both layers every frame (LOAD's fonts);
//   - on the other pair, the Nano's 2 MHz trials (tools/lib/twomhz.mjs), every
//     access 2 us after the last, in TRAFFIC batches, with a scanline handler
//     between batches that moves IRQLINE on by eight lines every time /INT
//     falls, so there is a scanline interrupt every eight lines;
//   - SNAPSHOTs streaming over the debug link, each checked against the host's
//     frame of the same scene (vdp-scene);
//   - a capture every so often, kept, and afterwards matched against the host's
//     frames between the snapshots either side of it.
//
// A capture of these scenes cannot meet Phase 10's tolerance, which was set on
// the oracle's pictures: every pixel is detail, and the picture scrolls every
// frame. What it can show is that the picture is there, locked and in place:
// one of the candidate frames must match it clearly better than the rest, at
// the offset the dongle puts every picture (tools/lib/screen.mjs).
//
// At the end the card's statistics say whether any row was late, any FIFO
// overran, and how long anything held the bus off.

import { CMD, Link, decodeSnapshot, decodeStats, packLoad, packSnapshot, u8 } from './link.mjs'
import { checkSnapshot, hostFrameAsync } from './scenes.mjs'
import { Trials, prepare } from './twomhz.mjs'
import { ACCESS, PORT, scriptPort } from './nano.mjs'
import { grabAsync } from './capture.mjs'
import { TOLERANCE, align, expectedFrame, paletteOf } from './screen.mjs'
import { execFileSync } from 'node:child_process'
import { join } from 'node:path'
import { REPO } from './emulator.mjs'

const VDP_SCENE = join(REPO, 'build', 'host', 'host', 'scene', 'vdp-scene')
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

const command = (pair) => scriptPort(pair ? PORT.B_COMMAND : PORT.ADDRESS)
const reg = (pair, index, value) => [ACCESS.WRITE | command(pair), value, ACCESS.WRITE | command(pair), 0x80 | index]

function hostIndices(scene, frame) {
  return execFileSync(VDP_SCENE, [scene, '--frame', String(frame)], { maxBuffer: 1 << 20 })
}

/** The least a matching frame must beat the others' median by, in levels. */
const MARGIN = 5

/**
 * One capture against the scene: every host frame from `from` to `to`, each at
 * the offset that suits it best (the dongle locks where it likes, and moves
 * when the card is reset: docs/BENCH.md section 6). Stable if the best frame
 * beats the median of the rest by MARGIN, where TOLERANCE puts every picture.
 */
export function judgeCapture(scene, capture, from, to, palette) {
  const scores = []
  for (let n = Math.max(0, from); n <= to; n++) {
    const o = align(expectedFrame(hostIndices(scene, n), palette), capture)
    scores.push({ frame: n, mae: o.mae, offset: { dx: o.dx, dy: o.dy } })
  }
  scores.sort((a, b) => a.mae - b.mae)
  const best = scores[0]
  const rest = scores.slice(1).map((s) => s.mae)
  const median = rest.length ? rest[Math.floor(rest.length / 2)] : Infinity
  const problems = []
  if (median - best.mae < MARGIN) problems.push(`no frame of ${scores.length} stands out: the best ${best.mae.toFixed(1)} levels out, the rest ${median.toFixed(1)}`)
  const { dx, dy } = best.offset
  if (Math.abs(dx - TOLERANCE.offset.dx) > TOLERANCE.offset.slack || Math.abs(dy - TOLERANCE.offset.dy) > TOLERANCE.offset.slack) {
    problems.push(`the picture sits at ${dx},${dy}`)
  }
  return { frame: best.frame, candidates: scores.length, mae: best.mae, others: median, offset: best.offset, problems }
}

export async function runLoadRun({ nano, link: opened, scene, minutes, nanoPair = 1, captureEvery = 60, device = '0', log = console.log }) {
  // A request left unanswered has the port reopened, once for every request
  // then in flight, and is asked again: the card's run is what is being
  // measured, not the host's serial port. Each reopening is counted and logged,
  // with what the old port last received and whether another process's port
  // is answered meanwhile.
  let link = opened
  let generation = 0
  let reopening = null
  const relinked = []
  const reopen = async (why) => {
    const idle = link.receivedAt ? Date.now() - link.receivedAt : null
    let other
    try {
      other = execFileSync(process.execPath, [join(REPO, 'tools', 'vdpctl.mjs'), 'info'], { encoding: 'utf8', timeout: 20000, stdio: ['ignore', 'pipe', 'pipe'] }).split('\n')[0]
    } catch (error) {
      other = `failed: ${String(error.stderr ?? error.message).trim().split('\n')[0]}`
    }
    relinked.push({ at: Date.now(), why, received: link.received, idleMs: idle, other })
    log(`  the link failed (${why}); ${link.received} bytes received, the last ${idle} ms ago; another process: ${other}; reopened`)
    link.close()
    link = await Link.open()
    generation++
  }
  const ask = async (command, payload, timeout) => {
    const asked = generation
    try {
      return await link.request(command, payload, timeout)
    } catch (error) {
      if (!/^(no answer|the link|closed|the answer .* CRC)/.test(String(error.message))) throw error
      if (asked === generation) reopening ??= reopen(error.message).finally(() => { reopening = null })
      await reopening
      return link.request(command, payload, timeout)
    }
  }
  // The scene, with its program on the pair the Nano leaves it.
  await link.request(CMD.LOAD, packLoad({ fonts: true, scenePairB: nanoPair === 0 }))
  await link.request(CMD.SCENE, Buffer.from(scene))
  await sleep(1500)
  const vram = await link.request(CMD.VRAM, undefined, 5000)   // before any traffic: its copy holds the bus off

  // The Nano's pair: its trial window, STAT1 on its status port, a scanline
  // interrupt at line 0 to start the chain.
  await nano.profile('6502-2mhz')
  const trials = new Trials({ seed: 13, pairs: [nanoPair] })
  await prepare(nano, trials)
  const statsel = nanoPair ? 0x0e : 0x0f
  await nano.script([...reg(nanoPair, statsel, 0x01), ...reg(nanoPair, 0x0b, 0x00), ...reg(nanoPair, 0x0a, 0x02), ACCESS.READ_IGNORED | command(nanoPair), 0])

  await link.request(CMD.STATS, u8(1))
  const started = Date.now()
  const until = started + minutes * 60_000
  const result = {
    scene, minutes, nanoPair: nanoPair ? 'B' : 'A',
    snapshots: { taken: 0, wrong: 0, late: 0, stale: 0 },
    captures: [],
    trials: null, wrong: { write: 0, read: 0, address: 0 }, batches: 0, handlers: 0, handlerWaitMaxNs: 0,
  }

  // The snapshot stream, alongside the Nano's traffic.
  let lastFrame = 0
  let registers = null
  let snapshotsSeen = 0
  const grabs = []
  let streamError = null
  let stop = false
  const stream = (async () => {
    while (Date.now() < until && !stop) {
      const asked = Date.now()
      const s = decodeSnapshot(await ask(CMD.SNAPSHOT, packSnapshot(0, 0, 2000), 30000))
      if (Date.now() - asked > 3000) log(`  a snapshot took ${Date.now() - asked} ms`)
      const check = checkSnapshot(scene, s, await hostFrameAsync(scene, s.state.sceneFrame))
      result.snapshots.taken++
      result.snapshots.wrong += check.wrong
      result.snapshots.late += check.late
      if (!check.stateMatches) result.snapshots.stale++
      lastFrame = s.state.sceneFrame
      registers = s.state.registers
      snapshotsSeen++
    }
  })().catch((error) => {
    streamError = error
  })

  let nextCapture = started + 5000
  let reported = started
  try {
  while (Date.now() < until) {
    if (streamError) throw streamError
    const b = trials.next()
    const r = await nano.traffic(b.pairs, { step: 8, wrap: 0, pair: nanoPair })
    result.batches++
    result.handlers = r.serviced
    result.handlerWaitMaxNs = r.latencyNs
    if (r.differed) {
      result.wrong[b.kind] += r.differed
      log(`  ${b.kind} batch: ${r.log.map((e) => `access ${e.index} read $${e.got.toString(16)} for $${e.expected.toString(16)}`).join(', ')}`)
    }
    if (Date.now() >= nextCapture && registers) {
      // The traffic pauses for the capture's second; the snapshots and the
      // card do not. It is judged after the run, between the last snapshot
      // before it and the first wholly after it.
      const before = lastFrame
      const frame = await grabAsync({ device })
      grabs.push({ at: (Date.now() - started) / 1000, frame, before, seen: snapshotsSeen })
      nextCapture = Date.now() + captureEvery * 1000
    }
    for (const g of grabs) if (g.after === undefined && snapshotsSeen >= g.seen + 2) g.after = lastFrame
    if (Date.now() - reported > 60_000) {
      reported = Date.now()
      const s = decodeStats(await ask(CMD.STATS, u8(0), 30000))
      log(`  ${((Date.now() - started) / 60000).toFixed(1)} min: late rows ${s.lateRows}, FIFO overruns ${s.bus.writeOverruns + s.bus.readOverruns}, stale data ${s.bus.staleData}; ` +
        `${result.batches} batches, trials ${JSON.stringify(trials.counts)}, ${result.handlers} scanline handlers; ${result.snapshots.taken} snapshots, ${result.snapshots.wrong} rows wrong`)
    }
  }
  } finally {
    stop = true
    await stream
  }
  if (streamError) throw streamError
  const palette = paletteOf(vram, registers)
  for (const g of grabs) {
    const judged = judgeCapture(scene, g.frame, g.before, g.after ?? g.before + 90, palette)
    result.captures.push({ at: g.at, between: [g.before, g.after], ...judged })
    log(`  capture at ${g.at.toFixed(0)} s: scene frame ${judged.frame} of ${judged.candidates}, ${judged.mae.toFixed(1)} levels out against ${judged.others.toFixed(1)} for the rest, ` +
      `at ${judged.offset.dx},${judged.offset.dy}${judged.problems.length ? `: ${judged.problems.join('; ')}` : ''}`)
  }
  result.trials = trials.counts
  result.relinked = relinked.map((r) => ({ at: (r.at - started) / 1000, why: r.why }))
  result.stats = decodeStats(await ask(CMD.STATS, u8(0), 30000))
  delete result.stats.histogram
  await nano.idle()                                          // the handler disarmed
  await nano.script([...reg(nanoPair, 0x0a, 0x00), ACCESS.READ_IGNORED | command(nanoPair), 0])
  await ask(CMD.LOAD, packLoad({}))
  await ask(CMD.RESET, u8(1))                                 // and the scene stopped
  if (link !== opened) link.close()
  return result
}
