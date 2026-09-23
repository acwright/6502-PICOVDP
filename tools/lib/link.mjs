// The debug link, host side (docs/DEBUGLINK.md): framed packets over the
// firmware's USB CDC port, and the decoders for what each command returns.
//
// No serialport dependency: the port is a tty, opened non-blocking, set raw
// with stty and read through Node's tty stream on its file descriptor.

import { execFileSync } from 'node:child_process'
import { constants, openSync, readdirSync, writeSync } from 'node:fs'
import { ReadStream } from 'node:tty'

export const CMD = {
  INFO: 0x01,
  STATS: 0x02,
  SNAPSHOT: 0x03,
  VRAM: 0x04,
  INJECT: 0x05,
  RESET: 0x06,
  REBOOT: 0x07,
  FAULT: 0x08,
  SCENE: 0x09,
  LOAD: 0x0a,
  SCENE_LOG: 0x0b,
  PROFILE: 0x0c,
}
const RESPONSE = 0x80
const LOG = 0x7f
const ERROR = 0xff
const HEADER = 8

const CRC_TABLE = new Uint32Array(256).map((_, n) => {
  let c = n
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
  return c >>> 0
})

export function crc32(bytes, crc = 0) {
  crc = ~crc >>> 0
  for (const b of bytes) crc = (CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8)) >>> 0
  return ~crc >>> 0
}

export function findPort() {
  if (process.env.PICOVDP_PORT) return process.env.PICOVDP_PORT
  const ports = readdirSync('/dev').filter((name) => name.startsWith('cu.usbmodem')).sort()
  return ports.length ? `/dev/${ports[0]}` : null
}

export class LinkError extends Error {}

export class Link {
  /** Open the firmware's port: PICOVDP_PORT, or the first /dev/cu.usbmodem*. */
  static async open(path = findPort()) {
    if (!path) throw new LinkError('no /dev/cu.usbmodem* port: is the board plugged in and running a debug build?')
    const link = new Link(path)
    await link.ready
    return link
  }

  constructor(path) {
    this.path = path
    this.fd = openSync(path, constants.O_RDWR | constants.O_NOCTTY | constants.O_NONBLOCK)
    // 115200 is only nominal on USB CDC; 1200 would reboot the board into BOOTSEL.
    execFileSync('stty', ['-f', path, '115200', 'raw', '-echo', '-echoe', '-echok', '-icanon', '-opost'])
    this.buffer = Buffer.alloc(0)
    this.sequence = 0
    this.waiting = new Map()
    this.logs = []
    this.stream = new ReadStream(this.fd)
    this.stream.on('data', (chunk) => this.receive(chunk))
    this.stream.on('error', (error) => this.fail(error))
    this.ready = Promise.resolve()
  }

  close() {
    this.fail(new LinkError('closed'))
    this.stream.destroy()
  }

  fail(error) {
    for (const { reject } of this.waiting.values()) reject(error)
    this.waiting.clear()
  }

  receive(chunk) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk
    for (;;) {
      const start = this.buffer.indexOf('PV')
      if (start < 0) {
        this.buffer = this.buffer.subarray(Math.max(0, this.buffer.length - 1))
        return
      }
      if (start) this.buffer = this.buffer.subarray(start)
      if (this.buffer.length < HEADER) return
      const length = this.buffer.readUInt32LE(4)
      if (length > 1 << 24) {
        this.buffer = this.buffer.subarray(1)
        continue
      }
      if (this.buffer.length < HEADER + length + 4) return
      const packet = this.buffer.subarray(0, HEADER + length + 4)
      this.buffer = this.buffer.subarray(HEADER + length + 4)
      const crc = packet.readUInt32LE(HEADER + length)
      if (crc32(packet.subarray(2, HEADER + length)) !== crc) continue
      const type = packet[2]
      const sequence = packet[3]
      const payload = Buffer.from(packet.subarray(HEADER, HEADER + length))
      if (type === LOG) {
        this.logs.push(payload.toString('utf8'))
        continue
      }
      const waiter = this.waiting.get(sequence)
      if (!waiter) continue
      this.waiting.delete(sequence)
      clearTimeout(waiter.timer)
      if (type === ERROR) waiter.reject(new LinkError(payload.toString('utf8')))
      else if (type !== (waiter.command | RESPONSE)) waiter.reject(new LinkError(`answered ${type} to ${waiter.command}`))
      else waiter.resolve(payload)
    }
  }

  /** Send a command; resolves with the response's payload. */
  request(command, payload = Buffer.alloc(0), timeout = 5000) {
    const sequence = (this.sequence = (this.sequence + 1) & 0xff)
    const header = Buffer.alloc(HEADER)
    header.write('PV', 0, 'latin1')
    header[2] = command
    header[3] = sequence
    header.writeUInt32LE(payload.length, 4)
    const crc = Buffer.alloc(4)
    crc.writeUInt32LE(crc32(payload, crc32(header.subarray(2))))
    const promise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(sequence)
        reject(new LinkError(`no answer to command ${command} in ${timeout} ms`))
      }, timeout)
      this.waiting.set(sequence, { command, resolve, reject, timer })
    })
    this.write(Buffer.concat([header, payload, crc]))
    return promise
  }

  // The descriptor is non-blocking: a full tty buffer is EAGAIN, and waited out.
  write(packet) {
    for (let at = 0; at < packet.length; ) {
      try {
        at += writeSync(this.fd, packet, at, Math.min(4096, packet.length - at))
      } catch (error) {
        if (error.code !== 'EAGAIN') throw error
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1)
      }
    }
  }
}

// ---- decoding ----

export class Reader {
  constructor(buffer) {
    this.buffer = buffer
    this.at = 0
  }
  u8() {
    return this.buffer.readUInt8(this.at++)
  }
  u16() {
    const v = this.buffer.readUInt16LE(this.at)
    this.at += 2
    return v
  }
  u32() {
    const v = this.buffer.readUInt32LE(this.at)
    this.at += 4
    return v
  }
  u64() {
    const v = this.buffer.readBigUInt64LE(this.at)
    this.at += 8
    return v
  }
  bytes(n) {
    const v = this.buffer.subarray(this.at, this.at + n)
    this.at += n
    return v
  }
  text(n) {
    const raw = this.bytes(n)
    const end = raw.indexOf(0)
    return raw.subarray(0, end < 0 ? n : end).toString('utf8')
  }
}

export const RESET_REASONS = ['power-on', 'fault', 'watchdog']
export const FAULT_KINDS = ['none', 'hardfault', 'panic', 'watchdog']

export function decodeInfo(payload) {
  const r = new Reader(payload)
  const info = {
    protocol: r.u8(),
    safeMode: r.u8() === 1,
    resetReason: RESET_REASONS[r.u8()] ?? 'unknown',
    version: r.u8(),
    clockHz: r.u32(),
    uptimeUs: Number(r.u64()),
    build: r.text(32),
    board: r.text(16),
    id: r.text(24),
    fault: null,
  }
  if (r.u8()) {
    const f = { kind: FAULT_KINDS[r.u32()] ?? 'unknown', core: r.u32() }
    for (const name of ['r0', 'r1', 'r2', 'r3', 'r12', 'lr', 'pc', 'xpsr', 'excReturn', 'sp', 'cfsr', 'hfsr', 'mmfar', 'bfar', 'sfsr', 'sfar']) {
      f[name] = r.u32()
    }
    f.stack = Array.from({ length: 32 }, () => r.u32())
    f.uptimeUs = Number(r.u64())
    f.heartbeatCore0 = r.u32()
    f.heartbeatCore1 = r.u32()
    f.message = r.text(64)
    f.build = r.text(32)
    info.fault = f
  }
  return info
}

function timing(r) {
  return { max: r.u32(), p999: r.u32(), mean: r.u32() }
}

export function decodeStats(payload) {
  const r = new Reader(payload)
  const s = {
    uptimeUs: Number(r.u64()),
    clockHz: r.u32(),
    budget: r.u32(),
    rowsBuilt: r.u32(),
    linesTaken: r.u32(),
    lateRows: r.u32(),
    latchesMerged: r.u32(),
    missedBells: r.u32(),
    journalOverflows: r.u32(),
    rasterSlips: r.u32(),
    latency: timing(r),
    build: timing(r),
  }
  for (const name of ['catchUp', 'half', 'expand', 'wait', 'publish', 'core0Half']) s[`${name}Max`] = r.u32()
  s.splitMean = r.u32()
  s.splitRows = r.u32()
  s.latchIsrMax = r.u32()
  s.lineIsrMax = r.u32()
  s.busStandins = r.u32()
  const bins = r.u16()
  s.histogramShift = r.u8()
  s.histogram = Array.from({ length: bins }, () => r.u16())
  // The bus's counts, appended in Phase 11 (docs/DEBUGLINK.md). Absent from an
  // image that predates it.
  if (r.at < payload.length) {
    s.bus = {}
    for (const name of BUS_COUNTS) s.bus[name] = r.u32()
  }
  return s
}

/** STATS' bus block, in order (firmware/bus.h's bus_stats_t). */
export const BUS_COUNTS = [
  'writes', 'reads', 'staleData', 'staleStatus', 'coincident', 'writeOverruns', 'readOverruns', 'stagingWaits', 'resets', 'isrMax', 'intLevel',
]

export function decodeState(r) {
  const state = { registers: Buffer.from(r.bytes(128)), ports: [] }
  for (let pair = 0; pair < 2; pair++) {
    state.ports.push({ pointer: r.u16(), prefetch: r.u8(), payload: r.u8(), readMode: r.u8() === 1, second: r.u8() === 1 })
  }
  state.screenLine = r.u16()
  state.displayLine = r.u16()
  state.stat0 = r.u8()
  state.irqLatch = r.u8()
  state.frameEvents = r.u8()
  state.overflowSprite = r.u8()
  state.collisionMap = Buffer.from(r.bytes(8))
  state.fontPending = r.u8()
  state.fontId = [r.u8(), r.u8()]
  state.fontBase = [r.u16(), r.u16()]
  state.interrupt = r.u8() === 1
  state.frame = r.u32()
  state.sceneFrame = r.u32()
  return state
}

export function decodeSnapshot(payload) {
  const r = new Reader(payload)
  const snapshot = { frame: r.u32(), stateMatches: r.u8() === 1 }
  snapshot.state = decodeState(r)
  snapshot.source = Buffer.from(r.bytes(240))
  snapshot.late = Buffer.from(r.bytes(240))
  snapshot.rows = Buffer.from(r.bytes(240 * 320))
  return snapshot
}

export const INJECT_STATES = ['idle', 'armed', 'running', 'ended', 'failed']
export const MISMATCH_KINDS = ['read', '/INT after an operation', '/INT after the latch', 'record after its line', 'bad record']

export function decodeInjectStatus(payload) {
  const r = new Reader(payload)
  const s = {
    state: INJECT_STATES[r.u8()],
    baseFrame: r.u32(),
    frame: r.u16(),
    screenLine: r.u16(),
    ops: r.u32(),
    reads: r.u32(),
    stat5Reads: r.u32(),
    mismatches: r.u32(),
    space: r.u32(),
    buffered: r.u32(),
    taken: r.u32(),
  }
  s.endState = decodeState(r)
  const logged = r.u8()
  s.log = Array.from({ length: logged }, () => ({
    op: r.u32(),
    frame: r.u16(),
    screenLine: r.u16(),
    kind: MISMATCH_KINDS[r.u8()],
    port: r.u8(),
    expected: r.u8(),
    got: r.u8(),
    select: r.u8(),
  }))
  return s
}

export function u8(...values) {
  return Buffer.from(values)
}

export function packLoad({ busRate = 0, first = 0, last = 0, every = 1, cycles = 0, fonts = false } = {}) {
  const b = Buffer.alloc(15)
  b.writeUInt32LE(busRate, 0)
  b.writeUInt16LE(first, 4)
  b.writeUInt16LE(last, 6)
  b.writeUInt16LE(every, 8)
  b.writeUInt32LE(cycles, 10)
  b[14] = fonts ? 1 : 0
  return b
}

export function packSnapshot(mode, frame = 0, timeout = 2000) {
  const b = Buffer.alloc(9)
  b[0] = mode
  b.writeUInt32LE(frame >>> 0, 1)
  b.writeUInt32LE(timeout, 5)
  return b
}

export const PROFILE_STAGES = ['evaluate', 'layer0', 'layer1', 'row', 'left', 'right', 'expand', 'splitChoose']

export function decodeProfile(payload) {
  const r = new Reader(payload)
  const p = { row: r.u16(), sprites: r.u8(), probe: r.u32(), stages: {} }
  const count = r.u8()
  for (let i = 0; i < count; i++) p.stages[PROFILE_STAGES[i] ?? `stage${i}`] = { min: r.u32(), max: r.u32() }
  return p
}
