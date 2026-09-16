#!/usr/bin/env node

// vdpctl: the firmware's debug link from the Mac (PLAN.md section 5,
// docs/DEBUGLINK.md). The Nano and capture commands arrive in Phase 9.
//
//   vdpctl flash [file.uf2]              picotool load -x -f, then wait for INFO
//   vdpctl info                          build, clock, reset reason, the last fault record, symbolised
//   vdpctl stats [--reset] [--json]      renderer statistics
//   vdpctl snapshot [--frame N] [--out DIR]   the next frame as it went to VGA, and the state
//   vdpctl vram [--out FILE]             the bus copy of VRAM
//   vdpctl inject <fixture|trace> [checkpoint ...] [--out DIR]
//                                        replay checkpoints on the board, each against its golden
//   vdpctl reset [--power-on]            §15
//   vdpctl reboot [--bootsel]
//   vdpctl fault <core0|core1|hang|panic>
//   vdpctl scene <name>                  a worst-case scene (firmware/scenes.h)
//   vdpctl load [--bus HZ] [--handicap FIRST,LAST,EVERY,CYCLES] [--fonts]
//   vdpctl scene-log                     what the scene's program read, frame by frame
//   vdpctl profile [--row N] [--iterations N] [--json]   one row's stages, interrupts off
//   vdpctl late [--scene NAME] [--handicap FIRST,LAST,EVERY,CYCLES] [--seconds N]
//                                        §18's late line on purpose, checked against the host
//   vdpctl scenes [--seconds N] [--minutes N] [--only a,b] [--bus HZ] [--fonts] [--stream] [--out FILE.json]
//                                        run the worst-case scenes: statistics for each, and with
//                                        --stream every snapshot checked against vdp-scene; --fonts
//                                        adds FONT loads for both layers every frame (§7)
//
// The port is PICOVDP_PORT or the first /dev/cu.usbmodem*.

import { execFileSync, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { REPO } from './lib/emulator.mjs'
import {
  CMD,
  Link,
  LinkError,
  decodeInfo,
  decodeProfile,
  decodeSnapshot,
  decodeStats,
  findPort,
  packLoad,
  packSnapshot,
  u8,
} from './lib/link.mjs'
import { injectCheckpoints } from './lib/inject.mjs'

const DEFAULT_UF2 = join(REPO, 'build', 'pico2', 'firmware', 'picovdp.uf2')
const FAULTS = { core0: 0, core1: 1, hang: 2, panic: 3 }

function usage(message) {
  if (message) console.error(`vdpctl: ${message}`)
  console.error('usage: vdpctl flash|info|stats|snapshot|vram|inject|reset|reboot|fault|scene|load|scene-log ... (see the header of tools/vdpctl.mjs)')
  process.exit(2)
}

function options(args) {
  const flags = new Map()
  const positional = []
  for (let i = 0; i < args.length; i++) {
    if (args[i].startsWith('--')) {
      const name = args[i].slice(2)
      const next = args[i + 1]
      if (next !== undefined && !next.startsWith('--') && !['reset', 'json', 'power-on', 'bootsel', 'classes', 'stream', 'profile', 'fonts'].includes(name)) {
        flags.set(name, next)
        i++
      } else {
        flags.set(name, true)
      }
    } else {
      positional.push(args[i])
    }
  }
  return { flags, positional }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

/** Wait for the board to enumerate and answer INFO. */
export async function waitForBoard(timeoutMs = 15000) {
  const until = Date.now() + timeoutMs
  let last
  while (Date.now() < until) {
    const port = findPort()
    if (port) {
      let link
      try {
        link = await Link.open(port)
        const info = decodeInfo(await link.request(CMD.INFO, undefined, 1000))
        link.close()
        return info
      } catch (error) {
        last = error
        link?.close()
      }
    }
    await sleep(250)
  }
  throw new LinkError(`the board did not answer INFO within ${timeoutMs} ms${last ? `: ${last.message}` : ''}`)
}

function hex(v, width = 8) {
  return `0x${(v >>> 0).toString(16).padStart(width, '0')}`
}

function symbolise(addresses) {
  const elf = join(REPO, 'build', 'pico2', 'firmware', 'picovdp.elf')
  if (!existsSync(elf)) return new Map()
  const result = spawnSync('arm-none-eabi-addr2line', ['-f', '-C', '-p', '-e', elf, ...addresses.map((a) => hex(a))], {
    encoding: 'utf8',
  })
  const lines = (result.stdout ?? '').trim().split('\n')
  return new Map(addresses.map((a, i) => [a, lines[i] ?? '?']))
}

export function printInfo(info) {
  const uptime = (info.uptimeUs / 1e6).toFixed(1)
  console.log(`${info.board} ${info.id}, build ${info.build}, ${info.safeMode ? 'SAFE MODE' : 'running'}`)
  console.log(`  clock ${info.clockHz} Hz, uptime ${uptime} s, reset: ${info.resetReason}, STAT5 $${info.version.toString(16).padStart(2, '0')}`)
  const f = info.fault
  if (!f) {
    console.log('  no fault record')
    return
  }
  console.log(`  fault record: ${f.kind}${f.core <= 1 ? ` on core ${f.core}` : ''}, at ${(f.uptimeUs / 1e6).toFixed(3)} s, build ${f.build}`)
  if (f.message) console.log(`    ${f.message}`)
  if (f.kind === 'hardfault') {
    const names = symbolise([f.pc, f.lr])
    console.log(`    pc ${hex(f.pc)}  ${names.get(f.pc)}`)
    console.log(`    lr ${hex(f.lr)}  ${names.get(f.lr)}`)
    console.log(`    xpsr ${hex(f.xpsr)} sp ${hex(f.sp)} exc_return ${hex(f.excReturn)}`)
    console.log(`    cfsr ${hex(f.cfsr)} hfsr ${hex(f.hfsr)} mmfar ${hex(f.mmfar)} bfar ${hex(f.bfar)} sfsr ${hex(f.sfsr)} sfar ${hex(f.sfar)}`)
    console.log(`    r0-r3 ${[f.r0, f.r1, f.r2, f.r3].map((v) => hex(v)).join(' ')} r12 ${hex(f.r12)}`)
    for (let i = 0; i < 32; i += 8) console.log(`    stack+${i * 4}: ${f.stack.slice(i, i + 8).map((v) => hex(v)).join(' ')}`)
  }
  console.log(`    heartbeats: core 0 ${f.heartbeatCore0}, core 1 ${f.heartbeatCore1}`)
}

export function printStats(s) {
  const pct = (cycles) => `${(((s.budget - cycles) / s.budget) * 100).toFixed(0)}%`
  console.log(`${(s.uptimeUs / 1e6).toFixed(1)} s up, clock ${s.clockHz} Hz, budget ${s.budget} cycles a display line`)
  console.log(`  rows built ${s.rowsBuilt}, lines taken ${s.linesTaken}, LATE ROWS ${s.lateRows}, latches merged ${s.latchesMerged}`)
  console.log(`  missed bells ${s.missedBells}, journal overflows ${s.journalOverflows}, raster slips ${s.rasterSlips}, bus stand-ins ${s.busStandins}`)
  console.log(`  latency (latch to buffer): max ${s.latency.max} (spare ${pct(s.latency.max)}), 99.9% ${s.latency.p999}, mean ${s.latency.mean}`)
  console.log(`  build (catch-up to buffer): max ${s.build.max}, mean ${s.build.mean}`)
  console.log(
    `  stage maxima: core 1 catch-up ${s.catchUpMax}, half ${s.halfMax}, expand ${s.expandMax}, wait ${s.waitMax}, publish ${s.publishMax}; core 0 half and expand ${s.core0HalfMax}`
  )
  console.log(`  split mean ${s.splitMean} over ${s.splitRows} divided rows; latch interrupt max ${s.latchIsrMax}, line-start interrupt max ${s.lineIsrMax}`)
}

async function withLink(fn) {
  const link = await Link.open()
  try {
    return await fn(link)
  } finally {
    link.close()
  }
}

async function main() {
  const [command, ...rest] = process.argv.slice(2)
  const { flags, positional } = options(rest)
  switch (command) {
    case 'flash': {
      const uf2 = positional[0] ?? DEFAULT_UF2
      if (!existsSync(uf2)) usage(`no image at ${uf2}`)
      execFileSync('picotool', ['load', '-x', '-f', uf2], { stdio: 'inherit' })
      await sleep(1500)
      printInfo(await waitForBoard())
      break
    }
    case 'info':
      await withLink(async (link) => printInfo(decodeInfo(await link.request(CMD.INFO))))
      break
    case 'stats':
      await withLink(async (link) => {
        const s = decodeStats(await link.request(CMD.STATS, u8(flags.has('reset') ? 1 : 0)))
        if (flags.has('json')) console.log(JSON.stringify(s))
        else printStats(s)
      })
      break
    case 'snapshot':
      await withLink(async (link) => {
        const frame = flags.has('frame') ? Number(flags.get('frame')) : undefined
        const payload = packSnapshot(frame === undefined ? 0 : 1, frame ?? 0, 3000)
        const s = decodeSnapshot(await link.request(CMD.SNAPSHOT, payload, 5000))
        const late = [...s.late].filter(Boolean).length
        console.log(`frame ${s.frame}: ${late} late rows, state ${s.stateMatches ? 'of this frame' : `of frame ${s.state.frame}`}, STAT0 $${s.state.stat0.toString(16)}`)
        const out = flags.get('out')
        if (out) {
          mkdirSync(out, { recursive: true })
          writeFileSync(join(out, `frame-${s.frame}.idx.bin`), s.rows)
          const { rows, ...rest } = s
          writeFileSync(join(out, `frame-${s.frame}.json`), JSON.stringify({ ...rest, source: [...s.source], late: [...s.late], registers: [...s.state.registers] }, null, 1))
          console.log(`wrote ${out}/frame-${s.frame}.idx.bin and .json`)
        }
      })
      break
    case 'vram':
      await withLink(async (link) => {
        const vram = await link.request(CMD.VRAM)
        const out = flags.get('out') ?? 'vram.bin'
        writeFileSync(out, vram)
        console.log(`wrote ${vram.length} bytes to ${out}`)
      })
      break
    case 'inject': {
      if (!positional.length) usage('inject <fixture|trace> [checkpoint ...]')
      const failures = await injectCheckpoints(positional[0], positional.slice(1), { out: flags.get('out') })
      process.exit(failures ? 1 : 0)
    }
    case 'reset':
      await withLink((link) => link.request(CMD.RESET, u8(flags.has('power-on') ? 1 : 0)))
      break
    case 'reboot':
      await withLink((link) => link.request(CMD.REBOOT, u8(flags.has('bootsel') ? 1 : 0)).catch(() => {}))
      break
    case 'fault': {
      const kind = FAULTS[positional[0]]
      if (kind === undefined) usage(`fault ${Object.keys(FAULTS).join('|')}`)
      await withLink((link) => link.request(CMD.FAULT, u8(kind), 2000).catch(() => {}))
      console.log(`raised ${positional[0]}; waiting for the board`)
      await sleep(1500)
      printInfo(await waitForBoard())
      break
    }
    case 'scene':
      if (!positional[0]) usage('scene <name>')
      await withLink(async (link) => {
        const answer = await link.request(CMD.SCENE, Buffer.from(positional[0]))
        console.log(`scene ${answer[0]}: ${answer.subarray(1, 33).toString().replace(/\0.*$/, '')}`)
      })
      break
    case 'load': {
      const [first, last, every, cycles] = String(flags.get('handicap') ?? '0,0,1,0').split(',').map(Number)
      await withLink((link) => link.request(CMD.LOAD, packLoad({ busRate: Number(flags.get('bus') ?? 0), first, last, every, cycles, fonts: flags.has('fonts') })))
      break
    }
    case 'profile':
      await withLink(async (link) => {
        const payload = Buffer.alloc(6)
        payload.writeUInt16LE(Number(flags.get('row') ?? 101), 0)
        payload.writeUInt32LE(Number(flags.get('iterations') ?? 200), 2)
        const p = decodeProfile(await link.request(CMD.PROFILE, payload, 60000))
        if (flags.has('json')) return console.log(JSON.stringify(p))
        console.log(`row ${p.row}: ${p.sprites} sprites; counter probe ${p.probe}`)
        for (const [name, t] of Object.entries(p.stages)) console.log(`  ${name.padEnd(16)} min ${String(t.min).padStart(6)}  max ${String(t.max).padStart(6)}`)
      })
      break
    case 'scenes': {
      const { runScenes } = await import('./lib/scenes.mjs')
      const failures = await runScenes({
        seconds: flags.has('minutes') ? Number(flags.get('minutes')) * 60 : Number(flags.get('seconds') ?? 10),
        only: flags.has('only') ? String(flags.get('only')).split(',') : null,
        bus: Number(flags.get('bus') ?? 0),
        fonts: flags.has('fonts'),
        stream: flags.has('stream'),
        out: flags.get('out'),
        profile: flags.has('profile'),
      })
      process.exit(failures ? 1 : 0)
    }
    case 'late': {
      const { runLateTest } = await import('./lib/scenes.mjs')
      const [first, last, every, cycles] = String(flags.get('handicap') ?? '100,132,4,30000').split(',').map(Number)
      const scene = String(flags.get('scene') ?? 'full-4bpp-16-lim16')
      const r = await runLateTest({ scene, first, last, every, cycles, seconds: Number(flags.get('seconds') ?? 20) })
      const shown = [...r.snapshots.lateRowsShown.keys()].sort((a, b) => a - b)
      console.log(`${scene}, rows ${first}-${last - 1} every ${every} padded to ${cycles} cycles, ${r.stats.budget} a line:`)
      console.log(`  stats: late rows ${r.stats.lateRows} of ${r.stats.rowsBuilt}, latches merged ${r.stats.latchesMerged}, latency max ${r.stats.latency.max}`)
      console.log(`  ${r.snapshots.taken} snapshots: ${r.snapshots.late} late rows, ${r.snapshots.wrong} rows not the row they should show; late rows seen: ${shown.join(' ')}`)
      console.log(`  status read by the scene's program, frames ${r.frames[0]}-${r.frames[1]}: ${r.compared} frames compared with the host, ${r.differing.length} differ`)
      for (const d of r.differing.slice(0, 8)) console.log(`    ${d}`)
      process.exit(r.stats.lateRows && !r.snapshots.wrong && !r.differing.length && r.snapshots.late ? 0 : 1)
    }
    case 'scene-log':
      await withLink(async (link) => {
        const payload = await link.request(CMD.SCENE_LOG)
        const count = payload[0], reads = payload[1]
        for (let i = 0; i < count; i++) {
          const at = 2 + i * (4 + reads)
          console.log(`${payload.readUInt32LE(at)} ${[...payload.subarray(at + 4, at + 4 + reads)].map((b) => b.toString(16).padStart(2, '0')).join(' ')}`)
        }
      })
      break
    default:
      usage(command ? `unknown command ${command}` : undefined)
  }
  process.exit(0)
}

if (import.meta.url === `file://${process.argv[1]}`) {
  main().catch((error) => {
    console.error(`vdpctl: ${error.message}`)
    process.exit(1)
  })
}
