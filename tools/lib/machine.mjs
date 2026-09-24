// Phase 14's checks, on an AC6502 with the PRO fitted (PLAN.md Phase 14,
// docs/results/phase-14.md): each drives the machine over its serial console
// (lib/ace.mjs), runs one of tests/machine's programs or types at BASIC, and
// judges what it reads back or what the capture card sees against a golden —
// the oracle's where one exists, the emulator's own run (lib/machine-ref.mjs)
// where none does.

import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { REPO } from './emulator.mjs'
import { queryLine, readLabels } from './ace.mjs'
import { emulate } from './machine-ref.mjs'
import { grabMean, stream, DEFAULT_DEVICE } from './capture.mjs'
import { BLOCK_TOLERANCE, TOLERANCE, calibrate, compareCapture, expectedFrame, luma, paletteOf, wrongBlocks } from './screen.mjs'
import { writePictures } from './inject.mjs'

export const PROGRAMS = join(REPO, 'build', 'machine')
const HW_SID = 0x40  // HW_PRESENT ($030D) b6, 6502-BIOS BIOS.inc
const ORACLE = join(REPO, 'tests', 'oracle')

/** The frame period Phase 9 measured, and Phase 13 again: 59.94 Hz. */
export const FRAME_MS = 16.6838

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

export function program(name) {
  return {
    bytes: new Uint8Array(readFileSync(join(PROGRAMS, `${name}.prg`))),
    labels: readLabels(join(PROGRAMS, `${name}.lbl`)),
  }
}

/** An oracle checkpoint in the shape compareCapture takes. */
export function oracle(fixture, checkpoint) {
  const directory = join(ORACLE, fixture)
  const json = JSON.parse(readFileSync(join(directory, `${checkpoint}.json`), 'utf8'))
  return {
    indices: readFileSync(join(directory, `${checkpoint}.idx.bin`)),
    vram: readFileSync(join(directory, `${checkpoint}.vram.bin`)),
    registers: json.registers,
  }
}

/**
 * A capture against a golden: lib/screen.mjs's tolerance, and then its
 * blocks in brightness, which is what finds one wrong glyph or one wrong cell
 * (wrongBlocks). `fineColour` is for a picture whose colour changes every card
 * pixel, which the path cannot carry (lib/screen.mjs, TOLERANCE): its
 * whole-picture error is recorded, not gated, and the blocks judge it.
 */
function judge(name, golden, frame, pictures, { fineColour = false } = {}) {
  const seen = compareCapture(golden, frame, { tolerance: TOLERANCE })
  if (pictures) writePictures(pictures, name, frame, seen)
  // Swapping red and blue changes nothing in a picture of greys, so the
  // mapping test cannot decide there: the DAC's order was settled in Phase 10
  // and is judged here on pictures with colour.
  const e = seen.expected
  let grey = true
  for (let i = 0; i < e.length && grey; i += 3) grey = e[i] === e[i + 1] && e[i + 1] === e[i + 2]
  const problems = seen.problems.filter((p) =>
    !(grey && p.includes('explains the capture')) && !(fineColour && p.includes('out on average')))
  const brightness = luma(e)
  const captured = { ...frame, rgb: luma(frame.rgb) }
  const blocks = wrongBlocks(brightness, captured, seen.offset, calibrate(brightness, captured, seen.offset, null))
  for (const b of blocks.blocks) {
    if (b.count >= BLOCK_TOLERANCE.gate) problems.push(`block ${b.bx},${b.by} (card pixels ${b.bx * 8},${b.by * 8}): ${b.count} of 64 wrong in brightness`)
  }
  return {
    offset: seen.offset,
    mae: seen.picture.mae,
    settledRate: seen.settled?.rate ?? null,
    settledCount: seen.settled?.count ?? 0,
    worst: seen.settled?.worst ?? null,
    bestWrong: seen.bestWrong,
    mappingDecidable: !grey,
    wrongCardPixels: blocks.total,
    worstBlocks: blocks.blocks.slice(0, 5),
    problems,
  }
}

/** Whether BASIC is at its prompt and answering. */
export async function alive(ace) {
  try {
    return (await ace.value('6*7')) === 42
  } catch {
    return false
  }
}

// ---- The probe (SPEC §16) -------------------------------------------------------

export async function checkProbe(ace, { version } = {}) {
  const { bytes, labels } = program('probe')
  await ace.run(bytes)
  await ace.waitFor('!', 30000)
  const at = (name) => labels.get(name)
  const result = {
    runs: await ace.peekWord(at('Runs')),
    detectVdp: await ace.peekWord(at('VdpFound')),
    detectFont: await ace.peekWord(at('FontFound')),
    statA: [...await ace.peek(at('StatA'), 3)],
    statB: [...await ace.peek(at('StatB'), 3)],
    done: (await ace.peek(at('Done')))[0] === 0xa5,
    problems: [],
  }
  if (!result.done) result.problems.push('the probe did not run to its end')
  if (result.detectVdp !== result.runs) result.problems.push(`DetectVdp returned carry set ${result.detectVdp} times in ${result.runs}`)
  if (result.detectFont !== result.runs) result.problems.push(`DetectFont returned carry set ${result.detectFont} times in ${result.runs}`)
  for (const [pair, stat] of [['A', result.statA], ['B', result.statB]]) {
    if (stat[0] !== 0xac) result.problems.push(`STAT4 through port ${pair} read $${stat[0].toString(16)}`)
    if (stat[2] !== 0xbf) result.problems.push(`STAT6 through port ${pair} read $${stat[2].toString(16)}`)
    if (version !== undefined && stat[1] !== version) result.problems.push(`STAT5 through port ${pair} read $${stat[1].toString(16)}, not $${version.toString(16)}`)
  }
  return result
}

// ---- The bus (tests/machine/bus.asm) -------------------------------------------

export const BUS_COUNTERS = ['ErrTightA', 'ErrLooseA', 'ErrAddrA', 'ErrStatA', 'ErrTightB', 'ErrLooseB', 'ErrAddrB', 'ErrStatB', 'ErrCopy']

/** What one pass of bus.asm tries, per port pair unless it says otherwise. */
export const BUS_TRIALS_PER_PASS = {
  writePairs: 2048, tightReads: 2048, looseReads: 2048, readsAfterAddress: 1024, statusReads: 2048, copied: 4096,
}

export async function checkBus(ace, { passes = 16, seed = 0xc33c, onStart } = {}) {
  const { bytes, labels } = program('bus')
  await ace.load(bytes)
  await ace.poke(labels.get('Passes'), [passes & 0xff, passes >> 8])
  await ace.poke(labels.get('Seed'), [seed & 0xff, seed >> 8])
  ace.drain()
  const started = Date.now()
  await ace.type('RUN\r')
  onStart?.()
  await ace.waitFor('!', 60000 + passes * 5000)
  const seconds = (Date.now() - started) / 1000
  const clock = await ace.peekWord(labels.get('Clock'))
  const result = {
    passes,
    seed,
    seconds,
    clockCount: clock,
    mhz: (clock * 25) / (16 * FRAME_MS * 1000),
    done: (await ace.peek(labels.get('Done')))[0] === 0xa5,
    errors: {},
    trials: Object.fromEntries(Object.entries(BUS_TRIALS_PER_PASS).map(([k, v]) => [k, v * passes])),
    problems: [],
  }
  for (const name of BUS_COUNTERS) result.errors[name] = await ace.peekWord(labels.get(name), 3)
  if (!result.done) result.problems.push('the bus test did not run to its end')
  for (const [name, count] of Object.entries(result.errors)) {
    if (count) result.problems.push(`${name}: ${count}`)
  }
  return result
}

// ---- The BIOS -------------------------------------------------------------------

/**
 * Each ROM's boot and inputs. BIOS 2.0's are the oracle's `bios` fixture, and
 * its goldens judge them. BIOS 1.6 has no golden on this card — the oracle's
 * run of it is on the TMS9918A (`tms9918a/bios`) — so its inputs are that
 * fixture's, and the emulator's run of them on the PICOVDP is the reference.
 */
const BIOS_RUNS = {
  'bios-2.0': {
    boot: 1_000_000,
    restartMs: 3000,
    inputs: [['screenful', 'FOR I=1 TO 9:PRINT "LINE";I:NEXT\r'], ['scroll', 'PRINT "SCROLLED"\r']],
  },
  'bios-1.6': {
    boot: 7_000_000,       // past the boot menu's timeout, which starts BASIC by itself
    restartMs: 9000,
    // 1.6 keeps BASIC's warm-start magic across a reset and prints only OK;
    // with it cleared a reset is a cold start, as a power-on is.
    cold: 'POKE 879,0\r',
    inputs: [['screenful', 'FOR I=1 TO 15:PRINT "LINE";I:NEXT\r'], ['scroll', 'PRINT "SCROLLED"\r']],
  },
}

/**
 * The pictures a ROM's run should show. BIOS 2.0's header names the cards the
 * Kernal found, and the oracle's machine has them all; `absent` names slots
 * this machine's Kernal did not find, and then the emulator's run of the
 * fixture on a machine without them is the reference instead.
 */
function biosReferences(rom, absent = []) {
  const run = BIOS_RUNS[rom]
  if (rom === 'bios-2.0' && !absent.length) return (checkpoint) => oracle('bios', checkpoint)
  // With no CompactFlash card the Kernal's probe waits out its timeout.
  const steps = [{ run: absent.length ? run.boot + 2_000_000 : run.boot }, { capture: 'ok' }]
  for (const [checkpoint, typed] of run.inputs) steps.push({ type: typed }, { run: 3_000_000 }, { capture: checkpoint })
  const taken = emulate(rom, steps, { absent })
  return (checkpoint) => taken.get(checkpoint)
}

/** HW_PRESENT bits ($030D, 6502-BIOS BIOS.inc) of the cards the header lists and the emulator can leave out. */
const OPTIONAL_CARDS = [['io4', 0x08, 'CF'], ['io7', 0x40, 'SID']]

/**
 * A ROM's boot and its two inputs, on the machine: restart it and type them,
 * each checkpoint's picture captured once the machine has settled and judged
 * against its reference. `restart` is false when the caller has just pressed
 * the reset button itself.
 */
export async function checkBios(ace, { rom = 'bios-2.0', device = DEFAULT_DEVICE, pictures, restart = true } = {}) {
  const run = BIOS_RUNS[rom]
  const result = { rom, checkpoints: {}, restarts: 0, sidMissed: 0, problems: [] }
  // A reset keeps a program at $0800 and BASIC counts it (Kernal.asm,
  // "A reset is a cold BASIC start"); the golden is a power-on, with none.
  if (restart) await ace.type(`NEW\r${run.cold ?? ''}`)
  // BIOS 2.0's header lists the cards the Kernal found. On this ACE the SID's
  // probe misses now and then at 1 MHz, and at 2 MHz neither the SID nor the
  // CompactFlash card is found at all — the machine's business, not the video
  // card's. So a boot whose picture fails is asked, afterwards, which cards it
  // found (asking first would print on the screen being judged), and if some
  // were missing it is booted again and judged against the emulator's run of
  // a machine without them.
  let absent = []
  let reference = biosReferences(rom, absent)
  for (;;) {
    if (restart) {
      await ace.softReset(run.restartMs)
      result.restarts++
    }
    const ok = judge(`${rom}-ok`, reference('ok'), await grabMean({ device }), pictures)
    result.checkpoints.ok = ok
    if (!ok.problems.length || !restart || rom !== 'bios-2.0' || result.restarts === 3) break
    // Which cards did this boot find? A card missing from the header is the
    // machine's business, not the video card's: judge again, after a fresh
    // boot, against a reference without it.
    result.hwPresent = await ace.value('PEEK(781)')
    const missing = OPTIONAL_CARDS.filter(([, bit]) => !(result.hwPresent & bit))
    const now = missing.map(([slot]) => slot)
    if (!missing.length || now.join() === absent.join()) break
    if (missing.some(([, , name]) => name === 'SID')) result.sidMissed++
    result.cardsNotFound = missing.map(([, , name]) => name)
    absent = now
    reference = biosReferences(rom, absent)
  }
  result.reference = rom === 'bios-2.0' && !absent.length ? 'the oracle' : `the emulator${absent.length ? `, without ${result.cardsNotFound.join(' and ')}` : ''}`
  for (const p of result.checkpoints.ok.problems) result.problems.push(`ok: ${p}`)
  for (const [checkpoint, typed] of run.inputs) {
    await ace.type(typed)
    await sleep(2000)
    const r = judge(`${rom}-${checkpoint}`, reference(checkpoint), await grabMean({ device }), pictures)
    result.checkpoints[checkpoint] = r
    for (const p of r.problems) result.problems.push(`${checkpoint}: ${p}`)
  }
  if (rom === 'bios-2.0' && !absent.length) result.scrollVram = await scrollVram(ace, result.problems)
  return result
}

/**
 * Whether BIOS 2.0 scrolled in hardware: a picture cannot say, since a scroll
 * done by moving every row in VRAM looks the same, but VRAM can. After the
 * `scroll` checkpoint the name table is read back with VPEEK, and compared
 * with the emulator's, whose Kernal scrolls through L0SCRY (the golden has it
 * at 32), at the same moment: the line that reads it is typed at both, and the
 * emulator's VRAM is taken as it sends its first byte.
 */
const NAME_TABLE = 960  // $0000-$03BF, 40 x 24 (SPEC §5's memory map)

async function scrollVram(ace, problems) {
  const statements = `FOR I=0 TO ${NAME_TABLE - 1}:PRINT VPEEK(I);:NEXT`
  const reference = emulate('bios-2.0', [
    { run: BIOS_RUNS['bios-2.0'].boot },
    ...BIOS_RUNS['bios-2.0'].inputs.flatMap(([, typed]) => [{ type: typed }, { run: 3_000_000 }]),
    { vramWhenSent: 'vram', typed: queryLine(statements) },
  ]).get('vram').subarray(0, NAME_TABLE)
  const read = (await ace.query(statements, 120000)).trim().split(/\s+/).map(Number)
  const result = { bytes: read.length, differing: [], l0scryInGolden: oracle('bios', 'scroll').registers[0x14] }
  if (read.length !== NAME_TABLE) {
    problems.push(`scroll VRAM: read ${read.length} bytes back, not ${NAME_TABLE}`)
    return result
  }
  for (let a = 0; a < NAME_TABLE; a++) {
    if (read[a] !== reference[a]) result.differing.push({ address: a, ace: read[a], emulator: reference[a] })
  }
  if (result.differing.length) {
    problems.push(`scroll VRAM: ${result.differing.length} of ${NAME_TABLE} name-table bytes differ from the emulator's, the first at $${result.differing[0].address.toString(16)}`)
  }
  return result
}

// ---- graphics-1.asm ---------------------------------------------------------------

/**
 * 6502-DOCS v1's graphics-1.asm: its picture against the emulator's, the same
 * .prg on the same ROM; then a key, and BASIC must be back.
 */
export async function checkGraphics1(ace, { rom = 'bios-2.0', device = DEFAULT_DEVICE, pictures } = {}) {
  const { bytes } = program('graphics-1')
  const reference = emulate(rom, [
    { run: BIOS_RUNS[rom].boot + 1_000_000 },
    { load: bytes },
    { type: 'RUN\r' },
    { run: 2_000_000 },
    { capture: 'drawn' },
  ]).get('drawn')
  await ace.run(bytes)
  await sleep(2500)
  const drawn = judge(`graphics-1-${rom}`, reference, await grabMean({ device }), pictures, { fineColour: true })
  await ace.type(' ')
  await sleep(1500)
  const back = await alive(ace)
  const result = { rom, drawn, backToBasic: back, problems: drawn.problems.map((p) => `drawn: ${p}`) }
  if (!back) result.problems.push('BASIC did not answer after the key')
  return result
}

// ---- The sample cartridges, from RAM --------------------------------------------

/** A cheap likeness: every fourth pixel both ways, no alignment, no calibration. */
function roughDistance(expected, rgb) {
  let sum = 0
  let n = 0
  for (let y = 8; y < 472; y += 4) {
    for (let x = 8; x < 632; x += 4) {
      const i = (y * 640 + x) * 3
      sum += Math.abs(expected[i] - rgb[i]) + Math.abs(expected[i + 1] - rgb[i + 1]) + Math.abs(expected[i + 2] - rgb[i + 2])
      n++
    }
  }
  return sum / n
}

/**
 * A sample cartridge's source, run from RAM (tests/machine/cart.inc), watched
 * at 60 frames a second for `seconds`: each of its fixture's checkpoints is
 * judged on the captured frame most like it, which is the matching visual
 * state (§18: the frame numbers will not line up). Then a byte on the serial
 * port takes the machine back to BASIC.
 */
export async function checkCart(ace, fixture, { seconds, device = DEFAULT_DEVICE, pictures, keep = 4 } = {}) {
  const { bytes } = program(fixture)
  const manifest = JSON.parse(readFileSync(join(ORACLE, 'manifest.json'), 'utf8'))
  const checkpoints = manifest.fixtures.find((f) => f.name === fixture).checkpoints.map((c) => c.name)
  const goldens = checkpoints.map((name) => {
    const golden = oracle(fixture, name)
    return { name, golden, expected: expectedFrame(golden.indices, paletteOf(golden.vram, golden.registers)), best: [] }
  })
  await ace.run(bytes)
  await sleep(500)
  const watch = () => stream({
    device,
    seconds,
    onFrame(frame) {
      for (const g of goldens) {
        const d = roughDistance(g.expected, frame.rgb)
        if (g.best.length < keep || d < g.best.at(-1).d) {
          g.best.push({ d, frame })
          g.best.sort((a, b) => a.d - b.d)
          if (g.best.length > keep) g.best.pop()
        }
      }
    },
  })
  // Once in a full run the stream reported hundreds of thousands of frames in
  // a few seconds and none of the later screens: the capture, not the card.
  // A stream that cannot be right is watched again rather than judged.
  let frames = await watch()
  let restreamed = false
  if (frames > seconds * 62) {
    restreamed = true
    for (const g of goldens) g.best = []
    frames = await watch()
  }
  const result = { fixture, seconds, frames, restreamed, checkpoints: {}, problems: [] }
  if (frames > seconds * 62) result.problems.push(`the capture stream gave ${frames} frames in ${seconds} s`)
  for (const g of goldens) {
    let chosen = null
    for (const { frame } of g.best) {
      const r = compareCapture(g.golden, frame, { tolerance: TOLERANCE })
      const score = (r.problems.length ? 1e6 : 0) + r.picture.mae
      if (!chosen || score < chosen.score) chosen = { score, frame }
    }
    const r = judge(`${fixture}-${g.name}`, g.golden, chosen.frame, pictures)
    r.streamFrame = chosen.frame.index
    result.checkpoints[g.name] = r
    for (const p of r.problems) result.problems.push(`${g.name}: ${p}`)
  }
  // The hook looks at the serial port once round Main: every frame in VDP
  // Layers, once in four screens in VDP Modes. Then the Kernal boots.
  ace.write(' ')
  await sleep(fixture === 'vdp-modes' ? 9000 : 4000)
  result.backToBasic = (await alive(ace)) || (await alive(ace))
  if (!result.backToBasic) result.problems.push('BASIC did not answer after the byte that leaves the cartridge')
  return result
}
