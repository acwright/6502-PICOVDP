// vdpctl scenes: Phase 1's worst-case scenes on the board (firmware/scenes.h),
// each run for a while with its statistics taken, optionally under the bus
// stand-in and with snapshots streaming over the link the whole time. Every
// snapshot is checked against the same scene frame drawn by the core on the
// host (host/scene's vdp-scene): rows on time exactly, late rows as the row
// they repeat.

import { execFileSync } from 'node:child_process'
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

/** A snapshot against the host's frame: on-time rows exactly, late rows as the row they show. */
export function checkSnapshot(name, snapshot) {
  const expected = hostFrame(name, snapshot.state.sceneFrame)
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

export async function runScenes({ seconds, only, bus, stream, out, profile }) {
  const names = sceneNames().filter((name) => !only || only.includes(name))
  const link = await Link.open()
  const results = []
  let failures = 0
  try {
    await link.request(CMD.LOAD, packLoad({ busRate: bus }))
    console.log(`${names.length} scenes, ${seconds} s each${bus ? `, bus stand-in at ${bus} Hz` : ''}${stream ? ', snapshots streaming' : ''}`)
    for (const name of names) {
      await link.request(CMD.SCENE, Buffer.from(name))
      await sleep(1500)  // set up at the next line 250, and settled
      let profiled = null
      if (profile) profiled = decodeProfile(await link.request(CMD.PROFILE, Buffer.from([101, 0, 100, 0, 0, 0]), 60000))
      await link.request(CMD.STATS, u8(1))
      const started = Date.now()
      const snapshots = { taken: 0, wrong: 0, late: 0, stale: 0 }
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
      const stats = decodeStats(await link.request(CMD.STATS, u8(0)))
      const result = { name, seconds: (Date.now() - started) / 1000, stats, snapshots, profile: profiled }
      delete result.stats.histogram
      results.push(result)
      const spare = (((stats.budget - stats.latency.max) / stats.budget) * 100).toFixed(0)
      const bad = stats.lateRows || stats.latchesMerged || snapshots.wrong
      if (bad) failures++
      console.log(
        `${bad ? 'LATE ' : 'ok   '} ${name.padEnd(22)} rows ${stats.rowsBuilt}, late ${stats.lateRows}, merged ${stats.latchesMerged}; ` +
          `latency max ${stats.latency.max} (${spare}% spare), 99.9% ${stats.latency.p999}, mean ${stats.latency.mean}; ` +
          `core 1 half ${stats.halfMax}, core 0 ${stats.core0HalfMax}, split ${stats.splitMean}` +
          (stream ? `; ${snapshots.taken} snapshots, ${snapshots.wrong} rows wrong, ${snapshots.late} late, ${snapshots.stale} stale state` : '')
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
