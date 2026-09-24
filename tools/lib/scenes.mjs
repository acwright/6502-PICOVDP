// vdpctl scenes: Phase 1's worst-case scenes on the board (firmware/scenes.h),
// each run for a while with its statistics taken, optionally under the bus
// stand-in and with snapshots streaming over the link the whole time. Every
// snapshot is checked against the same scene frame drawn by the core on the
// host (host/scene's vdp-scene): rows on time exactly, late rows as the row
// they repeat.

import { execFile, execFileSync } from 'node:child_process'
import { existsSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { REPO } from './emulator.mjs'
import { CMD, Link, decodeProfile, decodeSnapshot, decodeStats, packLoad, packSnapshot, u8 } from './link.mjs'

const VDP_SCENE = join(REPO, 'build', 'host', 'host', 'scene', 'vdp-scene')
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))
const WIDTH = 320

function sceneNames() {
  if (!existsSync(VDP_SCENE)) throw new Error(`no ${VDP_SCENE}: build the host preset first`)
  return execFileSync(VDP_SCENE, ['--list'], { encoding: 'utf8' }).trim().split('\n')
}

const frames = new Map()
function hostFrame(name, n) {
  const key = `${name}/${n}`
  if (!frames.has(key)) {
    if (frames.size > 64) frames.clear()
    frames.set(key, execFileSync(VDP_SCENE, [name, '--frame', String(n)], { maxBuffer: 1 << 20 }))
  }
  return frames.get(key)
}

/**
 * The same frame without blocking the process: a caller also streaming a
 * snapshot must go on reading its port, or the host's tty buffer overflows
 * and bytes are lost (Phase 13's load run).
 */
const pending = new Map()
export function hostFrameAsync(name, n) {
  const key = `${name}/${n}`
  if (frames.has(key)) return Promise.resolve(frames.get(key))
  if (!pending.has(key)) {
    pending.set(key, new Promise((resolve, reject) => {
      execFile(VDP_SCENE, [name, '--frame', String(n)], { encoding: 'buffer', maxBuffer: 1 << 20 }, (error, out) => {
        pending.delete(key)
        if (error) return reject(error)
        if (frames.size > 64) frames.clear()
        frames.set(key, out)
        resolve(out)
      })
    }))
  }
  return pending.get(key)
}

/** A snapshot against the host's frame: on-time rows exactly, late rows as the row they show. */
export function checkSnapshot(name, snapshot, expected = hostFrame(name, snapshot.state.sceneFrame)) {
  let wrong = 0
  let late = 0
  for (let row = 0; row < 240; row++) {
    const got = snapshot.rows.subarray(row * WIDTH, (row + 1) * WIDTH)
    if (snapshot.late[row]) {
      late++
      const source = snapshot.source[row]
      if (source < row && !got.equals(expected.subarray(source * WIDTH, (source + 1) * WIDTH))) wrong++
      continue
    }
    if (!got.equals(expected.subarray(row * WIDTH, (row + 1) * WIDTH))) wrong++
  }
  return { wrong, late, stateMatches: snapshot.stateMatches }
}

/**
 * Run the worst-case scenes. `nano`, from Phase 13: the Nano's 2 MHz trials
 * (tools/lib/twomhz.mjs) on port B while each scene runs, in place of Phase
 * 1's stand-in — the real bus, a burst of 100 accesses 2 us apart, then a
 * host round trip, over and over.
 */
export async function runScenes({ seconds, only, bus, fonts, stream, out, profile, nano = null }) {
  const names = sceneNames().filter((name) => !only || only.includes(name))
  const link = await Link.open()
  const { Trials, prepare } = await import('./twomhz.mjs')
  const results = []
  let failures = 0
  try {
    await link.request(CMD.LOAD, packLoad({ busRate: bus, fonts }))
    console.log(`${names.length} scenes, ${seconds} s each${bus ? `, bus stand-in at ${bus} Hz` : ''}${nano ? ', the Nano\'s 2 MHz traffic on port B' : ''}${fonts ? ', FONT loads for both layers every frame' : ''}${stream ? ', snapshots streaming' : ''}`)
    for (const name of names) {
      await link.request(CMD.SCENE, Buffer.from(name))
      await sleep(1500)  // set up at the next line 250, and settled
      let profiled = null
      if (profile) profiled = decodeProfile(await link.request(CMD.PROFILE, Buffer.from([101, 0, 100, 0, 0, 0]), 60000))
      let trials = null
      if (nano) {
        trials = new Trials({ seed: 21, pairs: [1] })
        await prepare(nano, trials)
      }
      await link.request(CMD.STATS, u8(1))
      const started = Date.now()
      const snapshots = { taken: 0, wrong: 0, late: 0, stale: 0 }
      const traffic = { batches: 0, wrong: 0 }
      const trafficLoop = nano && (async () => {
        while (Date.now() - started < seconds * 1000) {
          const r = await nano.paced(trials.next().pairs)
          traffic.batches++
          traffic.wrong += r.differed
        }
      })()
      while (Date.now() - started < seconds * 1000) {
        if (!stream) {
          await sleep(Math.min(500, seconds * 1000 - (Date.now() - started)))
          continue
        }
        const s = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 2000), 5000))
        const check = checkSnapshot(name, s)
        snapshots.taken++
        snapshots.wrong += check.wrong
        snapshots.late += check.late
        if (!check.stateMatches) snapshots.stale++
      }
      if (trafficLoop) await trafficLoop
      const stats = decodeStats(await link.request(CMD.STATS, u8(0)))
      const result = { name, seconds: (Date.now() - started) / 1000, stats, snapshots, profile: profiled, traffic: nano ? { ...traffic, trials: trials.counts } : null }
      delete result.stats.histogram
      results.push(result)
      const spare = (((stats.budget - stats.latency.max) / stats.budget) * 100).toFixed(0)
      const bad = stats.lateRows || stats.latchesMerged || snapshots.wrong || traffic.wrong || (nano && stats.bus.staleData)
      if (bad) failures++
      console.log(
        `${bad ? 'LATE ' : 'ok   '} ${name.padEnd(22)} rows ${stats.rowsBuilt}, late ${stats.lateRows}, merged ${stats.latchesMerged}; ` +
          `latency max ${stats.latency.max} (${spare}% spare), 99.9% ${stats.latency.p999}, mean ${stats.latency.mean}; ` +
          `core 1 half ${stats.halfMax}, core 0 ${stats.core0HalfMax}, split ${stats.splitMean}; latch isr max ${stats.latchIsrMax}` +
          (stream ? `; ${snapshots.taken} snapshots, ${snapshots.wrong} rows wrong, ${snapshots.late} late, ${snapshots.stale} stale state` : '') +
          (nano ? `; ${traffic.batches} batches of 2 MHz accesses, ${traffic.wrong} reads wrong, ${stats.bus.staleData} stale, bus interrupt mean ${stats.bus.isrMean}` : '')
      )
    }
  } finally {
    await link.request(CMD.LOAD, packLoad({})).catch(() => {})
    link.close()
  }
  if (out) {
    writeFileSync(out, JSON.stringify(results, null, 1))
    console.log(`wrote ${out}`)
  }
  console.log(failures ? `${failures} scene(s) with late rows or wrong snapshots` : 'no late rows')
  return failures
}

function hostReads(name, frames) {
  const text = execFileSync(VDP_SCENE, [name, '--reads', String(frames)], { encoding: 'utf8', maxBuffer: 1 << 26 })
  const reads = new Map()
  for (const line of text.trim().split('\n')) {
    const [frame, ...values] = line.split(' ')
    reads.set(Number(frame), values.join(' '))
  }
  return reads
}

function decodeSceneLog(payload) {
  const count = payload[0]
  const width = payload[1]
  const frames = []
  for (let i = 0; i < count; i++) {
    const at = 2 + i * (4 + width)
    frames.push({ frame: payload.readUInt32LE(at), reads: [...payload.subarray(at + 4, at + 4 + width)].map((b) => b.toString(16).padStart(2, '0')).join(' ') })
  }
  return frames
}

/**
 * §18's late line, on purpose: a scene whose rows [first, last) are padded every
 * `every` rows to `cycles`, past the budget. Late rows must be counted, must show
 * the row before them, and must leave status as it would have been: what the
 * scene's program reads each frame is compared with the host's reads.
 */
export async function runLateTest({ scene, first, last, every, cycles, seconds }) {
  const link = await Link.open()
  const log = []
  const snapshots = { taken: 0, wrong: 0, late: 0, stale: 0, lateRowsShown: new Map() }
  let stats
  try {
    await link.request(CMD.LOAD, packLoad({}))
    await link.request(CMD.SCENE, Buffer.from(scene))
    await sleep(1500)
    await link.request(CMD.LOAD, packLoad({ first, last, every, cycles }))
    await sleep(200)
    await link.request(CMD.SCENE_LOG)  // drain what came before the handicap
    await link.request(CMD.STATS, u8(1))
    const started = Date.now()
    let lastLog = Date.now()
    while (Date.now() - started < seconds * 1000) {
      const s = decodeSnapshot(await link.request(CMD.SNAPSHOT, packSnapshot(0, 0, 2000), 5000))
      const check = checkSnapshot(scene, s)
      snapshots.taken++
      snapshots.wrong += check.wrong
      snapshots.late += check.late
      if (!check.stateMatches) snapshots.stale++
      for (let row = 0; row < 240; row++) if (s.late[row]) snapshots.lateRowsShown.set(row, (snapshots.lateRowsShown.get(row) ?? 0) + 1)
      if (Date.now() - lastLog > 400) {
        log.push(...decodeSceneLog(await link.request(CMD.SCENE_LOG)))
        lastLog = Date.now()
      }
    }
    log.push(...decodeSceneLog(await link.request(CMD.SCENE_LOG)))
    stats = decodeStats(await link.request(CMD.STATS, u8(0)))
  } finally {
    await link.request(CMD.LOAD, packLoad({})).catch(() => {})
    link.close()
  }
  const last_frame = Math.max(...log.map((f) => f.frame))
  const expected = hostReads(scene, last_frame + 1)
  let compared = 0
  const differing = []
  for (const f of log) {
    compared++
    if (expected.get(f.frame) !== f.reads) differing.push(`frame ${f.frame}: board ${f.reads}, host ${expected.get(f.frame)}`)
  }
  return { stats, snapshots, compared, differing, frames: [log[0]?.frame, last_frame] }
}
