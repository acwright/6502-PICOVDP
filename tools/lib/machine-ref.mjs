// The AC6502 in the emulator, for what Phase 14 runs on the real one and the
// oracle has no golden of: a ROM, a .prg loaded as BASIC's LOAD leaves it,
// keys typed at the serial card, and the card's state captured on the way —
// in the shape of a golden ({ indices, vram, registers }), so that
// lib/screen.mjs can score a capture against it exactly as it scores one
// against the oracle.
//
// It drives the emulator's compiled engine in out/ (`npm run build:cli` there),
// the way its capture-goldens script does, and boots it the way fixtures.js
// does: cold, at 1 MHz, the PICOVDP in slot 8.

import { createRequire } from 'node:module'
import { existsSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { EMULATOR, loadFixtures } from './emulator.mjs'

const require = createRequire(join(EMULATOR, 'package.json'))

export const ROMS = {
  'bios-2.0': join(EMULATOR, 'src', 'renderer', 'public', 'roms', 'BIOS2.bin'),
  'bios-1.6': join(EMULATOR, 'src', 'renderer', 'public', 'roms', 'BIOS.bin'),
}

const CYCLES_PER_KEYSTROKE = 20_000

function engine() {
  const out = join(EMULATOR, 'out', 'core')
  if (!existsSync(join(out, 'Machine.js'))) {
    throw new Error(`no compiled engine in ${out} — run \`npm run build:cli\` in the emulator`)
  }
  return {
    Machine: require(join(out, 'Machine.js')).Machine,
    RTC: require(join(out, 'IO', 'RTC.js')).RTC,
    Video: require(join(out, 'IO', 'Video.js')).Video,
    ProgramImage: require(join(out, 'ProgramImage.js')),
    Empty: require(join(out, 'IO', 'Empty.js')).Empty,
  }
}

/**
 * Boot `rom` and play `steps`, each one of
 *   { run: cycles }        advance the machine
 *   { type: string }       keys to the serial card, paced as fixtures.js paces them
 *   { load: Uint8Array }   a .prg into $0800, with BASIC's pointers moved past it
 *   { capture: name }      the card's state, as a golden
 *   { peek: name, address, length }   bytes of memory
 *   { vramWhenSent: name, typed }      keys typed, and VRAM as it stood when
 *                          the machine first sent a byte on the serial port
 * and return what the captures and peeks took, by name. `absent` names slots
 * to leave empty ('io4' the CompactFlash card, 'io7' the SID), for a machine
 * whose Kernal did not find them.
 */
export function emulate(rom, steps, { absent = [] } = {}) {
  const { Machine, RTC, Video, ProgramImage, Empty } = engine()
  const { captureState } = loadFixtures()
  const slots = { io3: new RTC(() => ({ year: 2026, month: 9, date: 24, day: 5, hours: 12, minutes: 0, seconds: 0 })), io8: new Video() }
  for (const slot of absent) slots[slot] = new Empty()
  const machine = new Machine(slots)
  machine.frequency = 1_000_000
  machine.loadROM(new Uint8Array(readFileSync(ROMS[rom] ?? rom)))
  machine.reset(true)
  const taken = new Map()
  for (const step of steps) {
    if (step.run !== undefined) {
      machine.runCycles(step.run)
    } else if (step.type !== undefined) {
      for (const ch of step.type) {
        machine.onReceive(ch.charCodeAt(0))
        machine.runCycles(CYCLES_PER_KEYSTROKE)
      }
    } else if (step.load !== undefined) {
      const status = ProgramImage.loadProgramImage(machine, step.load)
      if (status !== 'ok') throw new Error(`loading the program into the emulator: ${status}`)
    } else if (step.capture !== undefined) {
      const state = captureState(machine)
      taken.set(step.capture, { indices: state.indices, vram: state.vram, registers: state.structural.registers, rgba: state.rgba })
    } else if (step.vramWhenSent !== undefined) {
      // Type `step.typed`, and take VRAM at the moment the machine first sends
      // a byte on the serial port: what a program printing VRAM over serial saw.
      let vram = null
      const video = machine.video()
      machine.transmit = () => {
        if (vram) return
        vram = new Uint8Array(video.vramSize)
        for (let a = 0; a < vram.length; a++) vram[a] = video.readVRAM(a)
      }
      for (const ch of step.typed) {
        machine.onReceive(ch.charCodeAt(0))
        machine.runCycles(CYCLES_PER_KEYSTROKE)
      }
      for (let n = 0; n < 200 && !vram; n++) machine.runCycles(100_000)
      machine.transmit = undefined
      if (!vram) throw new Error('the emulated machine never sent a byte')
      taken.set(step.vramWhenSent, vram)
    } else if (step.peek !== undefined) {
      const bytes = new Uint8Array(step.length)
      for (let i = 0; i < step.length; i++) bytes[i] = machine.peek(step.address + i)
      taken.set(step.peek, bytes)
    } else {
      throw new Error(`unrecognised step ${JSON.stringify(step)}`)
    }
  }
  return taken
}
