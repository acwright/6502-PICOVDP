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
//
// Through the Nano bus harness (docs/BENCH.md), not the debug link:
//   vdpctl text [--nano PATH]            load the font and put the Phase 9 test screen up
//   vdpctl grab [--device N] [--out FILE]   one frame from the capture card, as a PNG
//   vdpctl compare-capture [--device N] [--out DIR]
//                                        grab, render what VRAM says should be on screen,
//                                        and report the match rate
//   vdpctl irq-timing [--frames N] [--nano PATH]
//                                        enable the vblank interrupt and measure the /INT period
//   vdpctl sweep [--nano PATH]           how far the strobes can be squeezed before readback fails
//   vdpctl soak [--bytes N] [--timing NAME] [--nano PATH]
//                                        random bytes through all 16 KB of VRAM and back
//   vdpctl reopen [--times N] [--nano PATH]
//                                        reset the Nano N times over; VRAM must not move
//   vdpctl bus [--timing NAME] [--nano PATH]
//                                        the wiring check: ping, status, and a VRAM readback
//                                        that would show a reversed data bus at once.
//                                        NAME is 6502-1mhz (default), 6502-2mhz or fastest
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
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
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
import { Nano, PORT, PROFILES, sampleNs, strobeNs } from './lib/nano.mjs'
import {
  readVram, renderText, setRegister, status, testScreen, textMode, writeVram, TEXT, VRAM_SIZE,
} from './lib/tms.mjs'
import { grab as grabFrame, inkBounds, toBits, DEFAULT_DEVICE } from './lib/capture.mjs'
import { bitmapToRgba, encodePng } from './lib/png.mjs'

const DEFAULT_UF2 = join(REPO, 'build', 'pico2', 'firmware', 'picovdp.uf2')
const FAULTS = { core0: 0, core1: 1, hang: 2, panic: 3 }

const byte = (v) => `$${v.toString(16).padStart(2, '0')}`
function timingOf(flags) {
  const name = flags.get('timing') ?? '6502-1mhz'
  if (typeof name !== 'string' || !PROFILES[name]) {
    usage(`--timing must be one of ${Object.keys(PROFILES).join(', ')}`)
  }
  return name
}
const reverseBits = (v) => {
  let out = 0
  for (let i = 0; i < 8; i++) out |= ((v >> i) & 1) << (7 - i)
  return out
}

function usage(message) {
  if (message) console.error(`vdpctl: ${message}`)
  console.error('usage: vdpctl flash|info|stats|snapshot|vram|inject|reset|reboot|fault|scene|load|scene-log|bus|soak|reopen|irq-timing|sweep|text|grab|compare-capture ... (see the header of tools/vdpctl.mjs)')
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
    case 'text': {
      const nano = await Nano.open(flags.get('nano') ?? null)
      try {
        await nano.profile(timingOf(flags))
        const font = readFileSync(join(REPO, 'fonts', 'cp437-6x8.bin'))
        console.log(`loading ${font.length} bytes of font to $${TEXT.pattern.toString(16)}`)
        await writeVram(nano, TEXT.pattern, font)
        await writeVram(nano, TEXT.name, Buffer.from(testScreen()))
        await textMode(nano)
        console.log(`text mode up: ${TEXT.columns} x ${TEXT.rows}, white on black`)
        await nano.idle()
      } finally {
        await nano.close()
      }
      break
    }
    case 'grab': {
      const out = flags.get('out') ?? 'grab.png'
      const frame = grabFrame({ device: flags.get('device') ?? DEFAULT_DEVICE })
      const rgba = new Uint8Array(frame.width * frame.height * 4)
      for (let i = 0; i < frame.width * frame.height; i++) {
        rgba[i * 4] = frame.rgb[i * 3]
        rgba[i * 4 + 1] = frame.rgb[i * 3 + 1]
        rgba[i * 4 + 2] = frame.rgb[i * 3 + 2]
        rgba[i * 4 + 3] = 0xff
      }
      writeFileSync(out, encodePng(frame.width, frame.height, rgba))
      console.log(`${frame.width} x ${frame.height} -> ${out}`)
      break
    }
    case 'compare-capture': {
      const dir = flags.get('out') ?? '.'
      if (!existsSync(dir)) mkdirSync(dir, { recursive: true })

      // What should be on screen, taken from the card itself rather than from a
      // golden file: read the name and pattern tables back over the bus.
      const nano = await Nano.open(flags.get('nano') ?? null)
      let reference
      try {
        await nano.profile(timingOf(flags))
        const cells = await readVram(nano, TEXT.name, TEXT.columns * TEXT.rows)
        const pattern = await readVram(nano, TEXT.pattern, 2048)
        reference = renderText(cells, pattern)
        await nano.idle()
      } finally {
        await nano.close()
      }

      const frame = grabFrame({ device: flags.get('device') ?? DEFAULT_DEVICE })
      const captured = toBits(frame)
      const box = inkBounds(frame.width, frame.height, captured)
      if (!box) {
        console.log('the captured frame is blank: is the dongle connected, and is anything else holding the capture card?')
        process.exit(1)
      }
      console.log(`picture found at ${box.x0},${box.y0} ${box.width} x ${box.height} in ${frame.width} x ${frame.height}`)

      // The border puts ink in the outermost cells, so the box is the text area.
      const diff = new Uint8Array(reference.width * reference.height)
      let agree = 0
      for (let y = 0; y < reference.height; y++) {
        const cy = box.y0 + Math.floor(((y + 0.5) * box.height) / reference.height)
        for (let x = 0; x < reference.width; x++) {
          const cx = box.x0 + Math.floor(((x + 0.5) * box.width) / reference.width)
          const want = reference.bits[y * reference.width + x]
          const got = captured[cy * frame.width + cx]
          if (want === got) agree++
          else diff[y * reference.width + x] = 1
        }
      }
      const total = reference.width * reference.height
      const rate = (agree / total) * 100

      writeFileSync(join(dir, 'capture-reference.png'),
        encodePng(reference.width, reference.height, bitmapToRgba(reference.width, reference.height, reference.bits)))
      writeFileSync(join(dir, 'capture-difference.png'),
        encodePng(reference.width, reference.height, bitmapToRgba(reference.width, reference.height, diff, [0xff, 0x30, 0x30])))
      const rgba = new Uint8Array(frame.width * frame.height * 4)
      for (let i = 0; i < frame.width * frame.height; i++) {
        rgba[i * 4] = frame.rgb[i * 3]
        rgba[i * 4 + 1] = frame.rgb[i * 3 + 1]
        rgba[i * 4 + 2] = frame.rgb[i * 3 + 2]
        rgba[i * 4 + 3] = 0xff
      }
      writeFileSync(join(dir, 'capture.png'), encodePng(frame.width, frame.height, rgba))

      console.log(`${agree.toLocaleString()}/${total.toLocaleString()} pixels agree: ${rate.toFixed(2)}%`)
      console.log(`wrote capture.png, capture-reference.png and capture-difference.png to ${dir}`)
      // A converter resamples and a card compresses; SPEC's capture tolerance is
      // why this is never the pass/fail oracle. 98% is a clean picture.
      process.exit(rate >= 98 ? 0 : 1)
    }
    case 'irq-timing': {
      const frames = Number(flags.get('frames') ?? 120)
      const nano = await Nano.open(flags.get('nano') ?? null)
      try {
        await nano.profile(timingOf(flags))
        await textMode(nano, { interrupts: true })
        const before = await nano.int()
        console.log(`/INT ${before.level ? 'high' : 'low'} before the run`)
        const r = await nano.intPeriod(frames, 10)
        if (r.edges < 2) {
          console.log(`only ${r.edges} edge(s) in 10 s: the vblank interrupt is not reaching D8`)
          await setRegister(nano, 1, 0xd0)
          process.exit(1)
        }
        const hz = 1000 / r.milliseconds
        console.log(`${r.edges} edges, ${r.milliseconds.toFixed(4)} ms apart (${hz.toFixed(3)} Hz)`)
        // 262 lines at 59.94 Hz is 16.6833 ms (SPEC section 18); the emulator's
        // 60 Hz would be 16.6667 ms.
        const off = ((r.milliseconds - 16.6833) / 16.6833) * 100
        console.log(`against 16.6833 ms (59.94 Hz): ${off >= 0 ? '+' : ''}${off.toFixed(3)}%`)
        await setRegister(nano, 1, 0xd0)       // interrupts off again
        await nano.idle()
        process.exit(Math.abs(off) < 0.5 ? 0 : 1)
      } finally {
        await nano.close()
      }
    }
    case 'sweep': {
      const nano = await Nano.open(flags.get('nano') ?? null)
      try {
        const probe = Buffer.alloc(1024)
        for (let i = 0; i < probe.length; i++) probe[i] = (i * 13 + 7) & 0xff
        const works = async (timing) => {
          await nano.profile(timing)
          await writeVram(nano, 0x0000, probe)
          const back = await readVram(nano, 0x0000, probe.length)
          return back.equals(probe)
        }
        const base = { ...PROFILES['6502-1mhz'] }
        console.log('each step writes and reads 1 KB of VRAM, the other three axes held at 6502-1mhz')
        const label = {
          width: (n) => `${strobeNs(n)} ns strobe`,
          hold: (n) => `${3 * n * 62.5} ns hold`,
          setup: (n) => `${3 * n * 62.5} ns setup`,
          gap: (n) => `${n} us apart`,
        }
        for (const axis of ['width', 'hold', 'setup', 'gap']) {
          const results = []
          for (let n = 0; n <= 4; n++) {
            results.push([n, await works({ ...base, [axis]: n })])
          }
          const least = results.find(([, ok]) => ok)
          console.log(
            `  ${axis.padEnd(6)} ${results.map(([n, ok]) => `${n}:${ok ? 'ok' : 'FAIL'}`).join('  ')}` +
            `   least that works: ${least ? label[axis](least[0]) : 'none'}`,
          )
        }
        await nano.profile('6502-1mhz')
        await nano.idle()
        process.exit(0)
      } finally {
        await nano.close()
      }
    }
    case 'soak': {
      const name = timingOf(flags)
      const target = Number(flags.get('bytes') ?? 1_000_000)
      const nano = await Nano.open(flags.get('nano') ?? null)
      try {
        await nano.ping()
        await nano.profile(name)
        console.log(`soaking ${target.toLocaleString()} bytes at ${name}, ${VRAM_SIZE} bytes a pass`)
        const started = Date.now()
        let done = 0
        let wrong = 0
        let pass = 0
        while (done < target) {
          const written = Buffer.alloc(VRAM_SIZE)
          for (let i = 0; i < VRAM_SIZE; i++) written[i] = (Math.random() * 256) | 0
          await writeVram(nano, 0x0000, written)
          const back = await readVram(nano, 0x0000, VRAM_SIZE)
          for (let i = 0; i < VRAM_SIZE; i++) {
            if (back[i] === written[i]) continue
            if (wrong < 8) {
              console.log(`  $${i.toString(16).padStart(4, '0')}: wrote ${byte(written[i])}, read ${byte(back[i])}`)
            }
            wrong++
          }
          done += VRAM_SIZE
          pass++
          if (pass % 8 === 0) process.stdout.write(`\r  ${done.toLocaleString()} bytes, ${wrong} wrong   `)
        }
        const seconds = (Date.now() - started) / 1000
        process.stdout.write('\r')
        console.log(`${done.toLocaleString()} bytes in ${pass} passes, ${seconds.toFixed(1)} s (${Math.round(done / seconds).toLocaleString()} B/s)`)
        console.log(wrong ? `FAILED: ${wrong} bytes wrong` : 'no errors')
        await nano.idle()
        process.exit(wrong ? 1 : 0)
      } finally {
        await nano.close()
      }
    }
    case 'reopen': {
      const times = Number(flags.get('times') ?? 100)
      const path = flags.get('nano') ?? null
      // A known pattern, laid down once.
      const pattern = Buffer.alloc(VRAM_SIZE)
      for (let i = 0; i < VRAM_SIZE; i++) pattern[i] = (i * 7 + (i >> 8) * 31) & 0xff
      let nano = await Nano.open(path)
      try {
        await nano.profile(timingOf(flags))
        await writeVram(nano, 0x0000, pattern)
        console.log(`wrote ${VRAM_SIZE} bytes; now opening and closing the port ${times} times`)
      } finally {
        await nano.close()
      }
      // Each open pulses DTR, which resets the Nano. While it is in reset every
      // pin floats, and only the 10 k pull-ups hold the strobes high.
      for (let i = 1; i <= times; i++) {
        const round = await Nano.open(path, { settle: 250 })
        await round.close()
        if (i % 10 === 0) process.stdout.write(`\r  ${i}/${times}   `)
      }
      process.stdout.write('\r')
      nano = await Nano.open(path)
      try {
        await nano.profile(timingOf(flags))
        const back = await readVram(nano, 0x0000, VRAM_SIZE)
        let wrong = 0
        for (let i = 0; i < VRAM_SIZE; i++) {
          if (back[i] === pattern[i]) continue
          if (wrong < 8) console.log(`  $${i.toString(16).padStart(4, '0')}: expected ${byte(pattern[i])}, read ${byte(back[i])}`)
          wrong++
        }
        console.log(`after ${times} resets: ${VRAM_SIZE - wrong}/${VRAM_SIZE} bytes unchanged, status ${byte(await status(nano))}`)
        console.log(wrong ? `FAILED: ${wrong} bytes moved` : 'no stray access')
        await nano.idle()
        process.exit(wrong ? 1 : 0)
      } finally {
        await nano.close()
      }
    }
    case 'bus': {
      const name = timingOf(flags)
      const nano = await Nano.open(flags.get('nano') ?? null)
      try {
        const { firmware, protocol } = await nano.ping()
        console.log(`harness on ${nano.path}: firmware ${firmware}, protocol ${protocol}`)
        const timing = await nano.profile(name)
        console.log(`timing ${name}: /CSW low ${strobeNs(timing.width)} ns, data sampled ${sampleNs(timing.width)} ns after /CSR falls, gap ${timing.gap} us`)

        // The first status read clears whatever vblank left set behind it.
        const first = await status(nano)
        const second = await status(nano)
        console.log(`status ${byte(first)} then ${byte(second)}`)
        if (first === 0xff || first === 0x00 && second === 0x00) {
          console.log('  note: a status stuck at $ff or $00 usually means /CSR or MODE is not reaching the card')
        }

        // A walking one and its complements through VRAM. A reversed data bus
        // shows up as $01 coming back $80 -- the mistake PLAN.md section 5 warns
        // about, and the whole reason this test exists.
        const pattern = Buffer.from([0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                                     0xff, 0x00, 0xaa, 0x55, 0x5a, 0xa5])
        await writeVram(nano, 0x0000, pattern)
        const back = await readVram(nano, 0x0000, pattern.length)

        let wrong = 0
        let reversed = 0
        for (let i = 0; i < pattern.length; i++) {
          const w = pattern[i], r = back[i]
          if (w === r) continue
          wrong++
          if (r === reverseBits(w)) reversed++
          console.log(`  ${byte(w)} read back as ${byte(r)}${r === reverseBits(w) ? '  (bit-reversed)' : ''}`)
        }
        if (!wrong) {
          console.log(`VRAM readback ${pattern.length}/${pattern.length}: the data bus is right way round`)
        } else if (reversed === wrong) {
          console.log(`VRAM readback ${pattern.length - wrong}/${pattern.length}: the data bus is REVERSED -- CD0 is the MSB (PLAN.md section 5)`)
        } else {
          console.log(`VRAM readback ${pattern.length - wrong}/${pattern.length}: ${wrong} wrong`)
        }

        const int = await nano.int()
        console.log(`/INT ${int.level ? 'high (idle)' : 'LOW (asserted)'}, ${int.edges} edge${int.edges === 1 ? '' : 's'} seen`)
        await nano.idle()
        process.exit(wrong ? 1 : 0)
      } finally {
        await nano.close()
      }
    }
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
