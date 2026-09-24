#!/usr/bin/env node

// acectl: the AC6502 with the PRO fitted, from the Mac (PLAN.md Phase 14,
// docs/results/phase-14.md). Everything goes over the machine's own serial
// console (tools/lib/ace.mjs) and the capture card; nothing touches the PRO's
// USB, which a release build does not have.
//
//   acectl info                          STAT4-STAT6 through BASIC's VSTAT, and the reset vector
//   acectl reset                         restart through the reset vector (SYS), back to BIOS
//   acectl type TEXT                     type at BASIC; \r in TEXT is Return
//   acectl run FILE.prg                  LOAD it over XMODEM and RUN it
//   acectl probe [--version BCD]         tests/machine/probe.asm: SPEC §16's detection, 10,000 times
//   acectl bus [--passes N] [--seed N]   tests/machine/bus.asm: a 6502's tightest accesses on both pairs
//   acectl bios [--rom bios-2.0|bios-1.6] [--no-restart]
//                                        the BIOS's boot and the bios fixture's inputs typed by hand, each
//                                        checkpoint's picture judged against its golden (2.0) or the
//                                        emulator's run of the same ROM on the PICOVDP (1.6)
//   acectl graphics-1 [--rom bios-2.0|bios-1.6]
//                                        6502-DOCS v1's graphics-1.asm, against the emulator's picture
//   acectl cart vdp-modes|vdp-layers [--seconds N]
//                                        the sample cartridge's source from RAM, each checkpoint
//                                        judged on the captured frame that matches it
//   acectl all [--passes N] [--version BCD]
//                                        on BIOS 2.0: probe, bus, bios and both cartridges
//   acectl all-1.6 [--passes N]          on BIOS 1.6: bios, graphics-1, probe and bus
//
// Every check takes [--out FILE.json] [--pictures DIR] [--device N]. Build the
// programs first: make -C tests/machine. The port is ACE_PORT or the first
// /dev/cu.usbserial-*.

import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname } from 'node:path'
import { Ace } from './lib/ace.mjs'
import { DEFAULT_DEVICE } from './lib/capture.mjs'
import { checkBios, checkBus, checkCart, checkGraphics1, checkProbe } from './lib/machine.mjs'

const SWITCHES = ['no-restart']

function usage(message) {
  if (message) console.error(`acectl: ${message}`)
  console.error('usage: acectl info|reset|type|run|probe|bus|bios|graphics-1|cart|all|all-1.6 ... (see the header of tools/acectl.mjs)')
  process.exit(2)
}

function options(args) {
  const flags = new Map()
  const positional = []
  for (let i = 0; i < args.length; i++) {
    if (args[i].startsWith('--')) {
      const name = args[i].slice(2)
      if (!SWITCHES.includes(name) && args[i + 1] !== undefined && !args[i + 1].startsWith('--')) {
        flags.set(name, args[++i])
      } else {
        flags.set(name, true)
      }
    } else {
      positional.push(args[i])
    }
  }
  return { flags, positional }
}

/** Cancel whatever the machine is doing on the serial port and find BASIC again. */
async function recover(ace) {
  ace.write([0x18, 0x18, 0x18])
  await new Promise((resolve) => setTimeout(resolve, 3000))
  ace.write('\r')
  await new Promise((resolve) => setTimeout(resolve, 1000))
  ace.drain()
}

const number = (v) => (typeof v === 'string' ? Number(v.startsWith('$') ? `0x${v.slice(1)}` : v) : undefined)

function report(name, result) {
  const ok = !result.problems?.length
  console.log(`${name}: ${ok ? 'PASS' : 'FAIL'}`)
  for (const p of result.problems ?? []) console.log(`  - ${p}`)
  return ok
}

async function main() {
  const [command, ...rest] = process.argv.slice(2)
  if (!command) usage()
  const { flags, positional } = options(rest)
  const device = flags.get('device') ?? DEFAULT_DEVICE
  const pictures = flags.get('pictures')
  const out = flags.get('out')
  const ace = Ace.open()
  const results = {}
  let ok = true
  try {
    switch (command) {
      case 'info': {
        console.log(`STAT4 STAT5 STAT6: ${(await ace.query('PRINT VSTAT(4);VSTAT(5);VSTAT(6)')).trim()}`)
        console.log(`reset vector: ${await ace.value('PEEK(65532)+256*PEEK(65533)')}`)
        break
      }
      case 'reset':
        console.log(`SYS ${await ace.softReset()}`)
        break
      case 'type':
        await ace.type(positional.join(' ').replace(/\\r/g, '\r'))
        break
      case 'run':
        if (!positional[0]) usage('run needs a .prg')
        await ace.run(new Uint8Array(readFileSync(positional[0])), {
          onBlock: (n, of) => process.stdout.write(`\rblock ${n}/${of}`),
        })
        process.stdout.write('\n')
        break
      case 'probe':
        results.probe = await checkProbe(ace, { version: number(flags.get('version')) })
        console.log(JSON.stringify(results.probe))
        ok = report('probe', results.probe)
        break
      case 'bus':
        results.bus = await checkBus(ace, { passes: number(flags.get('passes')) ?? 16, seed: number(flags.get('seed')) ?? 0xc33c })
        console.log(JSON.stringify(results.bus))
        ok = report('bus', results.bus)
        break
      case 'bios':
        results.bios = await checkBios(ace, { rom: flags.get('rom') ?? 'bios-2.0', device, pictures, restart: !flags.get('no-restart') })
        console.log(JSON.stringify(results.bios, null, 1))
        ok = report('bios', results.bios)
        break
      case 'graphics-1':
        results.graphics1 = await checkGraphics1(ace, { rom: flags.get('rom') ?? 'bios-2.0', device, pictures })
        console.log(JSON.stringify(results.graphics1, null, 1))
        ok = report('graphics-1', results.graphics1)
        break
      case 'cart': {
        const fixture = positional[0]
        if (!['vdp-modes', 'vdp-layers'].includes(fixture)) usage('cart takes vdp-modes or vdp-layers')
        const seconds = number(flags.get('seconds')) ?? (fixture === 'vdp-layers' ? 70 : 12)
        results[fixture] = await checkCart(ace, fixture, { seconds, device, pictures })
        console.log(JSON.stringify(results[fixture], null, 1))
        ok = report(fixture, results[fixture])
        break
      }
      case 'all': {
        const version = number(flags.get('version'))
        const steps = [
          ['probe', () => checkProbe(ace, { version })],
          ['bus', () => checkBus(ace, { passes: number(flags.get('passes')) ?? 64 })],
          ['bios', () => checkBios(ace, { device, pictures })],
          ['vdp-modes', () => checkCart(ace, 'vdp-modes', { seconds: 12, device, pictures })],
          ['vdp-layers', () => checkCart(ace, 'vdp-layers', { seconds: 70, device, pictures })],
        ]
        for (const [name, step] of steps) {
          console.log(`-- ${name}`)
          try {
            results[name] = await step()
          } catch (error) {
            results[name] = { problems: [`did not finish: ${error.message}`] }
            await recover(ace)
          }
          ok = report(name, results[name]) && ok
        }
        break
      }
      case 'all-1.6': {
        const steps = [
          ['bios', () => checkBios(ace, { rom: 'bios-1.6', device, pictures })],
          ['graphics-1', () => checkGraphics1(ace, { rom: 'bios-1.6', device, pictures })],
          ['probe', () => checkProbe(ace, { version: number(flags.get('version')) })],
          ['bus', () => checkBus(ace, { passes: number(flags.get('passes')) ?? 64 })],
        ]
        for (const [name, step] of steps) {
          console.log(`-- ${name}`)
          try {
            results[name] = await step()
          } catch (error) {
            results[name] = { problems: [`did not finish: ${error.message}`] }
            await recover(ace)
          }
          ok = report(name, results[name]) && ok
        }
        break
      }
      default:
        usage(`no command ${command}`)
    }
  } finally {
    ace.close()
  }
  results.replays = ace.replays
  if (ace.replays.length) console.log(`the ACE replayed old input ${ace.replays.length} time(s) after a load`)
  if (out) {
    mkdirSync(dirname(out), { recursive: true })
    writeFileSync(out, `${JSON.stringify({ at: new Date().toISOString(), results }, null, 1)}\n`)
  }
  process.exit(ok ? 0 : 1)
}

main().catch((error) => {
  console.error(`acectl: ${error.message}`)
  process.exit(1)
})
