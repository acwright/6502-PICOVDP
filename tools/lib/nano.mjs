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
}

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

  /** Up to BLOCK_MAX bytes from one port. */
  async readBlock(port, count) {
    if (count < 1 || count > BLOCK_MAX) throw new NanoError(`block of ${count} bytes: the range is 1 to ${BLOCK_MAX}`)
    return this.request(CMD.READ_BLOCK, [port & 3, count])
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

/** Timer 1 runs at F_CPU, so one tick is 62.5 ns. */
export const TICK_NS = 1000 / 16
