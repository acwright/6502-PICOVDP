// The Arduino Nano bus harness, host side (docs/BENCH.md section 5): framed
// packets over the Nano's USB serial bridge at 1,000,000 baud.
//
// macOS stty cannot set 1 Mbaud -- it needs the IOSSIOSPEED ioctl -- so unlike
// the debug link in lib/link.mjs this one goes through `serialport`.
//
// One wrinkle, measured on Node 26 with serialport 13 and a CH340 bridge: the
// binding's reader does not wake the event loop by itself. Bytes are delivered
// only when the loop is woken for some other reason, so a reply can sit unseen
// for as long as the next timer is away -- exactly as long as the request's own
// timeout, which is how it first showed up. `nudge` keeps a 1 ms timer running
// while a request is in flight, which brings round trips to about 4 ms. pyserial
// on the same port is prompt, so this is the binding, not the driver.

import { SerialPort } from 'serialport'

export const BAUD = 1000000

export const CMD = {
  PING: 0x01,
  PROFILE: 0x02,
  WRITE: 0x03,
  READ: 0x04,
  WRITE_BLOCK: 0x05,
  READ_BLOCK: 0x06,
  RESET: 0x07,
  IDLE: 0x08,
  INT: 0x09,
  INT_PERIOD: 0x0a,
  SCRIPT: 0x0b,
  READ_RUN: 0x0c,
  PACED: 0x0d,
  INT_RUN: 0x0e,
  TRAFFIC: 0x0f,
}

/**
 * SCRIPT's access kinds, in b7:6 of each access's op byte, or'd with scriptPort.
 * A control does something else instead: RESET pulses /RESET low for the
 * value's microseconds, WAIT waits that long.
 */
export const ACCESS = { WRITE: 0x00, READ: 0x40, READ_IGNORED: 0x80, RESET: 0xc0, WAIT: 0xc4 }

/** A port as a SCRIPT op carries it: MODE1:MODE in b3:2, where PORTC has them. */
export const scriptPort = (port) => (port & 3) << 2

/** The port a SCRIPT op names. */
export const opPort = (op) => (op >> 2) & 3

/** The most accesses one SCRIPT carries: two bytes each, inside BLOCK_MAX. */
export const SCRIPT_MAX = 120

const ANSWER = 0x80
const ERROR = 0xff
const HEADER = 5

const ERRORS = {
  1: 'bad CRC',
  2: 'unknown command',
  3: 'wrong payload length',
  4: 'timed out reading the payload',
}

/** The four ports, as MODE1:MODE — which is the CPU's A1:A0 (SPEC §4). */
export const PORT = { DATA: 0, ADDRESS: 1, B_DATA: 2, B_COMMAND: 3 }

/**
 * Timing profiles. `setup`, `width` and `hold` are loop counts on the Nano and
 * `gap` is microseconds between accesses.
 *
 * A count of n costs 3n cycles, but the strobe also carries the loop's own test
 * and the closing `out`, so the measured widths (counted from the compiled code,
 * at 16 MHz) are:
 *
 *   /CSW low   width 0: 4 cycles = 250 ns;  width n: 3n + 2 cycles
 *   /CSR low   the data is sampled 3n + 3 cycles after the falling edge
 *
 * 250 ns is the floor: the Nano cannot make a strobe narrower than a 2 MHz
 * 6502's, which is why `fastest` and `6502-2mhz` share a width and differ only
 * in setup, hold and the gap between accesses.
 */
export const PROFILES = {
  '6502-1mhz': { setup: 2, width: 2, hold: 2, gap: 4 },   // 500 ns strobe
  '6502-2mhz': { setup: 1, width: 0, hold: 1, gap: 2 },   // 250 ns strobe
  fastest: { setup: 0, width: 0, hold: 0, gap: 0 },       // 250 ns, back to back
}

/** The /CSW low time, in nanoseconds, for a width count. */
export const strobeNs = (width) => (width === 0 ? 4 : 3 * width + 2) * 62.5

/** How long after /CSR falls the data is sampled, in nanoseconds. */
export const sampleNs = (width) => (3 * width + 3) * 62.5

/**
 * The most data one block may carry. The Nano's receive ring is 256 bytes and
 * cannot be raised -- past 256 the Arduino core widens its ring indices and
 * serial stops working -- so the whole packet, 5 header + payload + 2 CRC, has
 * to fit inside it.
 */
export const BLOCK_MAX = 240

export class NanoError extends Error {}

function crc16(bytes, crc = 0xffff) {
  for (const b of bytes) {
    crc ^= b << 8
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff
  }
  return crc
}

/** The Nano's port: PICOVDP_NANO_PORT, or the one CH340/FTDI/Arduino bridge found. */
export async function findPort() {
  if (process.env.PICOVDP_NANO_PORT) return process.env.PICOVDP_NANO_PORT
  const BRIDGES = new Set(['1a86', '0403', '2341', '1a86'.toUpperCase()])
  const ports = (await SerialPort.list()).filter(
    (p) => p.path.includes('usbserial') || p.path.includes('usbmodem'),
  )
  const bridges = ports.filter((p) => p.vendorId && BRIDGES.has(p.vendorId.toLowerCase()))
  const chosen = (bridges.length ? bridges : ports).map((p) => p.path.replace('/tty.', '/cu.'))
  const unique = [...new Set(chosen)].sort()
  if (unique.length > 1) {
    throw new NanoError(`more than one candidate port (${unique.join(', ')}): set PICOVDP_NANO_PORT`)
  }
  return unique[0] ?? null
}

export class Nano {
  /** Open the harness and wait out the DTR reset the open provokes. */
  static async open(path = null, { settle = 1800 } = {}) {
    const chosen = path ?? (await findPort())
    if (!chosen) throw new NanoError('no Nano found: is it plugged in? (set PICOVDP_NANO_PORT to override)')
    const nano = new Nano(chosen)
    await nano.opened
    // Wait out the DTR reset, then drain. The stirring matters: without it the
    // reader hands over whatever the last session left in the OS buffer only
    // when the loop next wakes, which would be *after* the clear below -- and a
    // stale reply whose sequence happens to match would be read as this
    // session's answer.
    nano.stir()
    await new Promise((resolve) => setTimeout(resolve, settle))
    await new Promise((resolve) => nano.port.flush(() => resolve()))
    await new Promise((resolve) => setTimeout(resolve, 50))
    nano.settle()
    nano.buffer = Buffer.alloc(0)
    return nano
  }

  constructor(path) {
    this.path = path
    // Start somewhere random so a packet left over from a previous session
    // cannot be mistaken for an answer in this one.
    this.sequence = (Math.random() * 256) | 0
    this.buffer = Buffer.alloc(0)
    this.waiting = null
    this.nudge = null
    this.port = new SerialPort({ path, baudRate: BAUD, autoOpen: false })
    this.opened = new Promise((resolve, reject) => {
      this.port.open((error) => {
        if (error) return reject(new NanoError(`${path}: ${error.message}`))
        // Only now: a 'data' listener attached before the port is open puts the
        // stream in flowing mode with nothing to read, and its poller stalls.
        this.port.on('data', (chunk) => this.receive(chunk))
        this.port.on('error', (failure) => this.fail(new NanoError(failure.message)))
        resolve()
      })
    })
  }

  async close() {
    this.settle()
    await new Promise((resolve) => this.port.close(() => resolve()))
  }

  /** Keep the event loop turning so the binding's reader is drained promptly. */
  stir() {
    if (!this.nudge) this.nudge = setInterval(() => {}, 1)
  }

  settle() {
    if (this.nudge) {
      clearInterval(this.nudge)
      this.nudge = null
    }
  }

  fail(error) {
    if (this.waiting) {
      const { reject, timer } = this.waiting
      this.waiting = null
      clearTimeout(timer)
      this.settle()
      reject(error)
    }
  }

  receive(chunk) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk
    for (;;) {
      const start = this.buffer.indexOf('NB')
      if (start < 0) {
        this.buffer = this.buffer.subarray(Math.max(0, this.buffer.length - 1))
        return
      }
      if (start > 0) this.buffer = this.buffer.subarray(start)
      if (this.buffer.length < HEADER) return
      const length = this.buffer[4]
      const total = HEADER + length + 2
      if (this.buffer.length < total) return
      const packet = this.buffer.subarray(0, total)
      this.buffer = this.buffer.subarray(total)
      this.deliver(packet, length)
    }
  }

  deliver(packet, length) {
    const type = packet[2]
    const sequence = packet[3]
    const payload = packet.subarray(HEADER, HEADER + length)
    const want = packet.readUInt16LE(HEADER + length)
    const waiting = this.waiting
    if (!waiting || waiting.sequence !== sequence) return
    this.waiting = null
    clearTimeout(waiting.timer)
    this.settle()
    if (crc16(packet.subarray(2, HEADER + length)) !== want) {
      waiting.reject(new NanoError('the Nano’s answer failed its CRC'))
    } else if (type === ERROR) {
      const code = payload[0]
      waiting.reject(new NanoError(`the Nano rejected the command: ${ERRORS[code] ?? `code ${code}`}`))
    } else if (type !== (waiting.type | ANSWER)) {
      waiting.reject(new NanoError(`expected an answer to $${waiting.type.toString(16)}, got $${type.toString(16)}`))
    } else {
      waiting.resolve(Buffer.from(payload))
    }
  }

  /**
   * Send a command and wait for its answer, resynchronising once if nothing
   * comes back. A dropped byte leaves the Nano hunting for the next `NB`, so
   * one clean retry recovers where a second would only pile on.
   */
  async request(type, payload = [], { timeout = 4000, retry = true } = {}) {
    try {
      return await this.send(type, payload, timeout)
    } catch (error) {
      if (!retry || !(error instanceof NanoError) || !error.message.includes('did not answer')) throw error
      await new Promise((resolve) => setTimeout(resolve, 100))
      this.buffer = Buffer.alloc(0)
      return this.send(type, payload, timeout)
    }
  }

  send(type, payload = [], timeout = 4000) {
    if (this.waiting) return Promise.reject(new NanoError('a request is already in flight'))
    const body = Buffer.from(payload)
    if (body.length > 255) return Promise.reject(new NanoError(`payload of ${body.length} bytes: the limit is 255`))
    const sequence = (this.sequence = (this.sequence + 1) & 0xff)
    const packet = Buffer.alloc(HEADER + body.length + 2)
    packet.write('NB', 0, 'ascii')
    packet[2] = type
    packet[3] = sequence
    packet[4] = body.length
    body.copy(packet, HEADER)
    packet.writeUInt16LE(crc16(packet.subarray(2, HEADER + body.length)), HEADER + body.length)
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting = null
        this.settle()
        reject(new NanoError(`the Nano did not answer $${type.toString(16)} within ${timeout} ms`))
      }, timeout)
      this.waiting = { type, sequence, resolve, reject, timer }
      this.stir()
      this.port.write(packet, (error) => {
        if (error) this.fail(new NanoError(error.message))
      })
    })
  }

  // ---- the primitives ----

  async ping() {
    const out = await this.request(CMD.PING)
    return { firmware: out[0], protocol: out[1] }
  }

  async profile(name) {
    const p = typeof name === 'string' ? PROFILES[name] : name
    if (!p) throw new NanoError(`no such profile: ${name}`)
    await this.request(CMD.PROFILE, [p.setup, p.width, p.hold, p.gap])
    return p
  }

  async write(port, value) {
    await this.request(CMD.WRITE, [port & 3, value & 0xff])
  }

  async read(port) {
    const out = await this.request(CMD.READ, [port & 3])
    return out[0]
  }

  /** Up to BLOCK_MAX bytes to one port, back to back at the current profile. */
  async writeBlock(port, bytes) {
    const body = Buffer.from(bytes)
    if (body.length > BLOCK_MAX) throw new NanoError(`block of ${body.length} bytes: the limit is ${BLOCK_MAX}`)
    await this.request(CMD.WRITE_BLOCK, [port & 3, ...body])
  }

  /**
   * Up to BLOCK_MAX bytes from one port. `retry: false` for a caller that must
   * know whether a block whose answer was lost was read: a data read moves the
   * pointer, so a blind retry reads the next block instead.
   */
  async readBlock(port, count, { retry = true } = {}) {
    if (count < 1 || count > BLOCK_MAX) throw new NanoError(`block of ${count} bytes: the range is 1 to ${BLOCK_MAX}`)
    return this.request(CMD.READ_BLOCK, [port & 3, count], { retry })
  }

  /**
   * Up to SCRIPT_MAX accesses, back to back at the current profile, as pairs of
   * (op, value) bytes (ACCESS). Reads are compared on the Nano. Returns how many
   * differed and the first few, each as { index, expected, got }, and how long
   * the accesses took on the Nano's Timer 1, in nanoseconds.
   */
  async script(pairs, { retry = true } = {}) {
    const body = Buffer.from(pairs)
    if (!body.length || body.length & 1 || body.length > 2 * SCRIPT_MAX) {
      throw new NanoError(`script of ${body.length} bytes: 2 to ${2 * SCRIPT_MAX}, in pairs`)
    }
    const out = await this.request(CMD.SCRIPT, body, { retry })
    const differed = out[0]
    const ns = out.readUInt32LE(1) * TICK_NS
    const log = []
    for (let at = 5; at + 3 <= out.length; at += 3) log.push({ index: out[at], expected: out[at + 1], got: out[at + 2] })
    return { differed, ns, log }
  }

  /**
   * `count` reads of one port back to back, 1 to 120, `extra` spacing each by
   * 3 more Nano cycles: 1.125 us apart at 0, 16 + 3 x extra cycles otherwise
   * (READ_RUN_PERIOD). Returns the bytes and the run's length in nanoseconds.
   */
  async readRun(port, count, extra = 0) {
    const out = await this.request(CMD.READ_RUN, [port & 3, count, extra])
    return { bytes: Buffer.from(out.subarray(4)), ns: out.readUInt32LE(0) * TICK_NS }
  }

  /**
   * Up to PACED_MAX accesses as (op, value) pairs, SCRIPT's ops less its
   * controls, each exactly pacedCycles(extra) after the last — 2 us at 0 —
   * with reads compared on the Nano. `bytes` returns what every read returned
   * instead of the log.
   */
  async paced(pairs, { extra = 0, bytes = false, retry = true } = {}) {
    const body = Buffer.from(pairs)
    if (!body.length || body.length & 1 || body.length > 2 * PACED_MAX) {
      throw new NanoError(`paced run of ${body.length} bytes: 2 to ${2 * PACED_MAX}, in pairs`)
    }
    const out = await this.request(CMD.PACED, [(extra & 0x7f) | (bytes ? 0x80 : 0), ...body], { retry })
    const result = { differed: out[0], ns: out.readUInt32LE(1) * TICK_NS }
    if (bytes) result.bytes = Buffer.from(out.subarray(5))
    else {
      result.log = []
      for (let at = 5; at + 3 <= out.length; at += 3) result.log.push({ index: out[at], expected: out[at + 1], got: out[at + 2] })
    }
    return result
  }

  /**
   * For each of `edges` falling edges of /INT, play the next of `sequences`
   * (each a list of (op, value) pairs) at PACED's spacing, `delayTicks` after
   * the edge. `times` returns each edge's Timer 1 count and the ticks from it
   * to the run; `reads` each run's read bytes. The Nano's interrupts are off
   * throughout, so nothing else may be asked of it meanwhile.
   */
  async intRun(sequences, { edges = 1, timeout = 0.1, times = false, reads = false, delayTicks = 0, extra = 0 } = {}) {
    const overflows = Math.min(255, Math.max(1, Math.ceil((timeout * 1e3) / 4.096)))
    const body = [edges & 0xff, (edges >> 8) & 0xff, overflows, (times ? 1 : 0) | (reads ? 2 : 0),
      delayTicks & 0xff, (delayTicks >> 8) & 0xff, extra, sequences.length]
    for (const pairs of sequences) body.push(pairs.length / 2, ...pairs)
    const out = await this.request(CMD.INT_RUN, body, { timeout: edges * (timeout * 1000 + 20) + 4000, retry: false })
    const result = { edges: out.readUInt16LE(0), timedOut: out[2] === 1, differed: out.readUInt16LE(3), records: [] }
    const readsPer = sequences.map((pairs) => pairs.filter((v, i) => i % 2 === 0 && (v & 0xc0)).length)
    let at = 5
    for (let e = 0; e < result.edges; e++) {
      const size = (times ? 6 : 0) + (reads ? readsPer[e % sequences.length] : 0)
      if (at + size > out.length) break   // the Nano keeps whole records, while they fit
      const record = {}
      if (times) {
        record.ticks = out.readUInt32LE(at)
        record.offset = out.readUInt16LE(at + 4)
        at += 6
      }
      if (reads) {
        const n = readsPer[e % sequences.length]
        record.bytes = Buffer.from(out.subarray(at, at + n))
        at += n
      }
      result.records.push(record)
    }
    return result
  }

  /**
   * A PACED batch with a scanline handler served before and after it while
   * /INT is low: IRQLINE moved on by `step` below `wrap`, and STAT1 read, on
   * `pair` (whose STATSEL must select STAT1). Returns PACED's answer and the
   * handlers served since the handler was set up, with the longest wait for
   * one in nanoseconds.
   */
  async traffic(pairs, { step = 0, wrap = 256, pair = 1, extra = 0, retry = true } = {}) {
    const body = Buffer.from(pairs)
    if (!body.length || body.length & 1 || body.length > 2 * PACED_MAX) {
      throw new NanoError(`traffic batch of ${body.length} bytes: 2 to ${2 * PACED_MAX}, in pairs`)
    }
    const out = await this.request(CMD.TRAFFIC, [step, wrap & 0xff, pair, extra, ...body], { retry })
    const log = []
    const logged = Math.min(out[0], 8)
    for (let i = 0; i < logged; i++) log.push({ index: out[5 + 3 * i], expected: out[6 + 3 * i], got: out[7 + 3 * i] })
    const at = 5 + 3 * logged
    return { differed: out[0], ns: out.readUInt32LE(1) * TICK_NS, log, serviced: out.readUInt32LE(at), latencyNs: out.readUInt16LE(at + 4) * TICK_NS }
  }

  async reset(microseconds = 100) {
    await this.request(CMD.RESET, [microseconds & 0xff, (microseconds >> 8) & 0xff])
  }

  async idle() {
    await this.request(CMD.IDLE)
  }

  /** The /INT line now, the edges counted, and Timer 1's count at the last one. */
  async int() {
    const out = await this.request(CMD.INT)
    return { level: out[0], edges: out.readUInt16LE(1), ticks: out.readUInt32LE(3) }
  }

  /**
   * Catch `count` falling edges of /INT, clearing the card's flag after each so
   * none is missed, and return the span between the first and the last.
   * `budget` is how long to wait, in seconds, and caps the whole run.
   */
  async intPeriod(count, budget = 10) {
    const centiseconds = Math.min(255, Math.max(1, Math.round(budget * 100)))
    const out = await this.request(CMD.INT_PERIOD, [count & 0xff, centiseconds], {
      timeout: budget * 1000 + 2000,
    })
    const edges = out[0]
    const first = out.readUInt32LE(1)
    const last = out.readUInt32LE(5)
    const ticks = (last - first + 0x1_0000_0000) % 0x1_0000_0000
    return {
      edges,
      ticks,
      milliseconds: edges > 1 ? (ticks * TICK_NS) / 1e6 / (edges - 1) : null,
    }
  }
}

/** The most accesses one PACED, INT_RUN or TRAFFIC carries (the Nano's RAM). */
export const PACED_MAX = 100

/** PACED's spacing, in Nano cycles, for an `extra`: 2 us at 0, 4 us at 11. */
export const pacedCycles = (extra) => (extra ? 31 + 3 * extra : 32)

/** READ_RUN's spacing, in Nano cycles, for an `extra`. */
export const readRunCycles = (extra) => (extra ? 16 + 3 * extra : 18)

/** Timer 1 runs at F_CPU, so one tick is 62.5 ns. */
export const TICK_NS = 1000 / 16
