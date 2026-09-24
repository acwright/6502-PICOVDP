// The AC6502 itself, from the Mac (PLAN.md Phase 14): its serial console at
// 19200 8-N-1 through a USB serial bridge, BASIC typed at it, and XMODEM for
// .prg files.
//
// BIOS 2.0 prints only to the video console while a card is fitted, so nothing
// typed comes back on the wire. What does: output printed with IO_MODE ($0306)
// set to serial for one line, which is how `query` reads a value back, XMODEM,
// which switches it itself, and what a program sends with SerialChrout.
//
// Like lib/link.mjs, no serialport dependency: the tty is opened non-blocking,
// set raw with stty, and read through Node's tty stream on its descriptor.

import { execFileSync } from 'node:child_process'
import { constants, openSync, readdirSync, readFileSync, writeSync } from 'node:fs'
import { ReadStream } from 'node:tty'

export const BAUD = 19200
const IO_MODE = 0x0306

/** Milliseconds between typed characters: BASIC echoes each to the screen. */
export const PACE_MS = 12

const SOH = 0x01
const EOT = 0x04
const ACK = 0x06
const NAK = 0x15
const CAN = 0x18
const SUB = 0x1a

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

const QUERY_END = '#END#'

/** The line `query` types: its statements, printing to the serial port. */
export function queryLine(statements) {
  return `POKE ${IO_MODE},1:${statements}:PRINT "${QUERY_END}":POKE ${IO_MODE},0\r`
}

export class AceError extends Error {}

/** ACE_PORT, or the first /dev/cu.usbserial-*. */
export function findAcePort() {
  if (process.env.ACE_PORT) return process.env.ACE_PORT
  const ports = readdirSync('/dev').filter((name) => name.startsWith('cu.usbserial')).sort()
  return ports.length ? `/dev/${ports[0]}` : null
}

/** A label file from ld65 -Ln, as a Map of name to address. */
export function readLabels(path) {
  const labels = new Map()
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    const m = /^al ([0-9A-F]+) \.(\S+)$/.exec(line.trim())
    if (m) labels.set(m[2], parseInt(m[1], 16))
  }
  return labels
}

export class Ace {
  static open(path = findAcePort()) {
    if (!path) throw new AceError('no /dev/cu.usbserial-* port: is the AC6502\'s serial cable plugged in?')
    return new Ace(path)
  }

  constructor(path) {
    this.path = path
    this.fd = openSync(path, constants.O_RDWR | constants.O_NOCTTY | constants.O_NONBLOCK)
    // No hardware flow control from this side: the ACE's RTS says when its
    // buffer is filling, and everything here is paced well below that.
    execFileSync('stty', ['-f', path, String(BAUD), 'raw', '-echo', '-crtscts', 'clocal', 'cs8', '-cstopb', '-parenb'])
    this.rx = Buffer.alloc(0)
    this.waiters = new Set()
    this.replays = []
    this.stream = new ReadStream(this.fd)
    this.stream.on('data', (chunk) => {
      this.rx = Buffer.concat([this.rx, chunk])
      for (const wake of this.waiters) wake()
    })
    this.stream.on('error', () => {})
  }

  close() {
    this.stream.destroy()
  }

  /** Forget whatever has arrived. */
  drain() {
    this.rx = Buffer.alloc(0)
  }

  write(bytes) {
    const data = typeof bytes === 'string' ? Buffer.from(bytes, 'latin1') : Buffer.from(bytes)
    for (let at = 0; at < data.length; ) {
      try {
        at += writeSync(this.fd, data, at, data.length - at)
      } catch (error) {
        if (error.code !== 'EAGAIN') throw error
      }
    }
  }

  /** Type at BASIC, a character at a time. */
  async type(text, pace = PACE_MS) {
    for (const ch of text) {
      this.write(ch)
      await sleep(pace)
    }
  }

  /**
   * Wait until what has arrived satisfies `test` (a function of the buffer, a
   * RegExp or a string), and return the match. Throws on timeout.
   */
  async waitFor(test, timeoutMs = 5000) {
    const check = typeof test === 'function'
      ? test
      : (buf) => {
          const text = buf.toString('latin1')
          if (typeof test === 'string') return text.includes(test) ? text : null
          const m = test.exec(text)
          return m ?? null
        }
    const found = check(this.rx)
    if (found) return found
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiters.delete(wake)
        reject(new AceError(`timed out after ${timeoutMs} ms waiting for ${test}; had ${JSON.stringify(this.rx.toString('latin1').slice(-80))}`))
      }, timeoutMs)
      const wake = () => {
        const result = check(this.rx)
        if (result) {
          clearTimeout(timer)
          this.waiters.delete(wake)
          resolve(result)
        }
      }
      this.waiters.add(wake)
    })
  }

  /**
   * Run one line of BASIC with its PRINT output sent to the serial port, and
   * return what it printed, up to the line that ends it. The screen shows the
   * line typed and OK, and nothing it printed.
   */
  async query(statements, timeoutMs = 5000) {
    const end = QUERY_END
    this.drain()
    await this.type(queryLine(statements))
    const text = await this.waitFor(`${end}\r\n`, timeoutMs)
    let out = text.slice(0, text.lastIndexOf(`${end}\r\n`))
    // With no video card the console is the serial port, and the line typed
    // comes back first: what it printed starts after the echo.
    const echo = out.lastIndexOf(`"${end}"`)
    if (echo >= 0) out = out.slice(out.indexOf('\n', echo) + 1)
    return out.replace(/\r/g, '')
  }

  /** A BASIC expression's value, as a number. */
  async value(expression) {
    const out = await this.query(`PRINT ${expression}`)
    const n = Number(out.trim())
    if (!Number.isFinite(n)) throw new AceError(`${expression} printed ${JSON.stringify(out)}`)
    return n
  }

  /** `count` bytes of memory from `address`. */
  async peek(address, count = 1) {
    const out = await this.query(`FOR I=0 TO ${count - 1}:PRINT PEEK(${address}+I):NEXT`, 5000 + count * 40)
    const bytes = out.trim().split(/\s+/).map(Number)
    if (bytes.length !== count || bytes.some((b) => !Number.isInteger(b) || b < 0 || b > 255)) {
      throw new AceError(`PEEK of ${count} bytes at ${address} printed ${JSON.stringify(out)}`)
    }
    return Uint8Array.from(bytes)
  }

  /** Little-endian unsigned value of `size` bytes at `address`. */
  async peekWord(address, size = 2) {
    const bytes = await this.peek(address, size)
    return bytes.reduceRight((v, b) => v * 256 + b, 0)
  }

  async poke(address, bytes) {
    const list = [...bytes]
    for (let i = 0; i < list.length; i += 8) {
      const chunk = list.slice(i, i + 8).map((b, j) => `POKE ${address + i + j},${b}`).join(':')
      await this.query(chunk)
    }
  }

  /**
   * Restart the machine through its reset vector, as `SYS` to the address the
   * vector holds. The Kernal sets the card up again from its registers; the
   * card's own RST pin is not pulsed, so its VRAM is not cleared (§15).
   */
  async softReset(settleMs = 2500) {
    const vector = await this.value('PEEK(65532)+256*PEEK(65533)')
    await this.type(`SYS ${vector}\r`)
    await sleep(settleMs)
    this.drain()
    return vector
  }

  /**
   * BASIC's bare LOAD: the ACE sends NAK and takes a checksum XMODEM stream
   * into $0800. `data` is the .prg's bytes.
   */
  async load(data, { onBlock } = {}) {
    this.drain()
    await this.type('LOAD\r')
    await this.waitFor((buf) => buf.includes(NAK), 10000)
    await this.xmodemSend(data, onBlock)
    await this.watchForReplay()
  }

  /**
   * After a load the line should stay quiet: BIOS 2.0 prints only to the
   * screen, and so does 1.6 with a card fitted. On this ACE at 2 MHz under
   * BIOS 1.6 the input ring's read pointer sometimes jumps back after a
   * transfer, and BASIC runs old input again (docs/results/phase-14.md, "The
   * replay"). Anything heard here is that: it is recorded in `replays`, and a
   * LOAD it started is cancelled so that the machine comes back to BASIC.
   */
  async watchForReplay(ms = 1500) {
    this.drain()
    await sleep(ms)
    const heard = this.rx.toString('latin1')
    if (!heard.length) return
    this.replays.push({ at: new Date().toISOString(), heard: heard.slice(0, 80) })
    if (heard.includes('XMODEM')) {
      this.write([CAN, CAN, CAN])
      await sleep(3000)
    }
    await sleep(2000)
    this.write('\r')
    await sleep(500)
    this.drain()
  }

  async xmodemSend(data, onBlock) {
    const blocks = Math.ceil(data.length / 128)
    for (let n = 0; n < blocks; n++) {
      const block = Buffer.alloc(132, SUB)
      block[0] = SOH
      block[1] = (n + 1) & 0xff
      block[2] = 0xff - block[1]
      data.subarray(n * 128, n * 128 + 128).forEach((b, i) => { block[3 + i] = b })
      let sum = 0
      for (let i = 3; i < 131; i++) sum = (sum + block[i]) & 0xff
      block[131] = sum
      for (let attempt = 0; ; attempt++) {
        if (attempt === 10) throw new AceError(`XMODEM block ${n + 1} refused ten times`)
        this.drain()
        this.write(block)
        const reply = await this.waitFor((buf) => buf.find((b) => b === ACK || b === NAK || b === CAN), 5000)
        if (reply === ACK) break
        if (reply === CAN) throw new AceError(`XMODEM cancelled by the ACE at block ${n + 1}`)
      }
      onBlock?.(n + 1, blocks)
    }
    for (let attempt = 0; ; attempt++) {
      if (attempt === 10) throw new AceError('XMODEM: the ACE never acknowledged EOT')
      this.drain()
      this.write([EOT])
      const reply = await this.waitFor((buf) => buf.find((b) => b === ACK || b === NAK), 5000).catch(() => null)
      if (reply === ACK) return
    }
  }

  /**
   * BASIC's bare BSAVE, `address` and `length`: the ACE sends a checksum
   * XMODEM stream once this end asks with NAK. Returns the bytes, the last
   * block's padding cut off.
   */
  async bsave(address, length) {
    this.drain()
    await this.type(`BSAVE ${address},${length}\r`)
    await this.waitFor('XMODEM', 10000).catch(() => {})
    await sleep(200)
    const blocks = []
    let expect = 1
    this.drain()
    this.write([NAK])
    for (;;) {
      const first = await this.waitFor((buf) => buf.length >= 1, 10000).then(() => this.rx[0])
      if (first === EOT) {
        this.write([ACK])
        break
      }
      if (first !== SOH) {
        this.drain()
        continue
      }
      await this.waitFor((buf) => buf.length >= 132, 10000)
      const block = this.rx.subarray(0, 132)
      this.rx = this.rx.subarray(132)
      let sum = 0
      for (let i = 3; i < 131; i++) sum = (sum + block[i]) & 0xff
      if (block[1] + block[2] !== 0xff || sum !== block[131]) {
        this.drain()
        this.write([NAK])
        continue
      }
      if (block[1] === (expect & 0xff)) {
        blocks.push(Buffer.from(block.subarray(3, 131)))
        expect++
      }
      this.write([ACK])
    }
    await sleep(300)
    return Buffer.concat(blocks).subarray(0, length)
  }

  /** LOAD a .prg, then RUN it. */
  async run(data, options) {
    await this.load(data, options)
    this.drain()
    await this.type('RUN\r')
  }
}
