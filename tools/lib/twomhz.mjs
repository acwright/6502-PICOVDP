// A 2 MHz 6502's tightest accesses, through the Nano's PACED runs (Phase 13,
// PLAN.md's "2 MHz"). Every access in a batch is exactly 2 us after the one
// before it — the spacing of back-to-back `lda abs` or `sta abs` at 2 MHz —
// so each of these is a trial:
//
//   back to back   a data read 2 us after a data read on the same port, which
//                  must return the prefetch the read before it fetched (§4)
//   after address  a data read 2 us after the address command that sets it,
//                  which must return the byte at that address
//   write          a data write 2 us after the last access, which must land:
//                  the next read-run over the same bytes compares every one
//
// The trials keep to a scratch window of VRAM whose contents the host knows,
// on one pair at a time, alternating. VINC must be +1 and VBANK 0 throughout,
// which prepare() sets and a load scene's program leaves alone after its setup.

import { ACCESS, PACED_MAX, PORT, scriptPort } from './nano.mjs'

/** $2100-$27FF: unused by every worst-case scene (firmware/scenes.c's layout). */
export const WINDOW = { base: 0x2100, size: 0x0700 }

const RUN = PACED_MAX - 2   // reads or writes after one address command

function rng(seed) {
  let s = seed >>> 0 || 1
  return () => {
    s ^= s << 13
    s >>>= 0
    s ^= s >>> 17
    s ^= s << 5
    s >>>= 0
    return s
  }
}

const command = (pair) => scriptPort(pair ? PORT.B_COMMAND : PORT.ADDRESS)
const data = (pair) => scriptPort(pair ? PORT.B_DATA : PORT.DATA)

/** The two writes that point `pair` at `address` in bank 0, for reading or writing. */
function address(pair, at, write) {
  return [ACCESS.WRITE | command(pair), at & 0xff, ACCESS.WRITE | command(pair), ((at >> 8) & 0x3f) | (write ? 0x40 : 0)]
}

/**
 * The trial stream: batches of up to PACED_MAX accesses, each with its counts.
 * `mirror` is the window's contents, which the write batches change.
 */
export class Trials {
  constructor({ seed = 1, window = WINDOW, pairs = [0, 1] } = {}) {
    this.random = rng(seed)
    this.window = window
    this.pairs = pairs
    this.mirror = Buffer.alloc(window.size)
    for (let i = 0; i < window.size; i++) this.mirror[i] = this.random() & 0xff
    this.turn = 0
    this.counts = { backToBack: 0, afterAddress: 0, writes: 0 }
  }

  /** What to write through the Nano before the trials: the window, and VINC and VBANK. */
  setup() {
    return { base: this.window.base, bytes: Buffer.from(this.mirror) }
  }

  /** The next batch: { pairs, counts, kind, pair }. */
  next() {
    const kind = ['write', 'read', 'address'][this.turn % 3]
    const pair = this.pairs[Math.floor(this.turn / 3) % this.pairs.length]
    this.turn++
    const { base, size } = this.window
    const ops = []
    const counts = { backToBack: 0, afterAddress: 0, writes: 0 }
    if (kind === 'write' || kind === 'read') {
      // A stretch written, then read straight back: a write run is always
      // followed by a read run over the bytes it wrote.
      if (kind === 'write') this.stretch = this.random() % (size - RUN)
      const offset = this.stretch ?? 0
      ops.push(...address(pair, base + offset, kind === 'write'))
      for (let i = 0; i < RUN; i++) {
        if (kind === 'write') {
          const value = this.random() & 0xff
          this.mirror[offset + i] = value
          ops.push(ACCESS.WRITE | data(pair), value)
          counts.writes++
        } else {
          ops.push(ACCESS.READ | data(pair), this.mirror[offset + i])
          if (i === 0) counts.afterAddress++
          else counts.backToBack++
        }
      }
    } else {
      while (ops.length + 6 <= 2 * PACED_MAX) {
        const offset = this.random() % size
        ops.push(...address(pair, base + offset, false), ACCESS.READ | data(pair), this.mirror[offset])
        counts.afterAddress++
      }
    }
    for (const k of Object.keys(counts)) this.counts[k] += counts[k]
    return { pairs: ops, counts, kind, pair }
  }
}

/**
 * Write the window and set VINC +1 and VBANK 0, through pair A with SCRIPT at
 * a safe pace: the trials are what is being tested, not their setting up.
 */
export async function prepare(nano, trials) {
  const { base, bytes } = trials.setup()
  const pair = trials.pairs[0]  // a pair the trials own: a load scene's program has the other
  const reg = (index, value) => [ACCESS.WRITE | command(pair), value, ACCESS.WRITE | command(pair), 0x80 | index]
  await nano.script([...reg(0x09, 0x01), ...reg(0x08, 0x00)])
  for (let at = 0; at < bytes.length; at += 100) {
    const chunk = bytes.subarray(at, at + 100)
    await nano.script([...address(pair, base + at, true), ...[...chunk].flatMap((v) => [ACCESS.WRITE | data(pair), v])])
  }
  const check = await nano.script([...address(pair, base, false), ...[...bytes.subarray(0, 100)].flatMap((v) => [ACCESS.READ | data(pair), v])])
  if (check.differed) throw new Error(`the trial window did not read back: ${check.differed} of 100 bytes differ`)
}
