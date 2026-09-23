// A minimal PNG writer, the same shape as the emulator's in
// src/tests/goldens/fixtures.js: 8-bit RGBA, no interlacing, every row filtered
// "none". Enough to save a grab, a reference render and a difference.

import { deflateSync } from 'node:zlib'

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])

const CRC_TABLE = new Uint32Array(256).map((_, n) => {
  let c = n
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
  return c >>> 0
})

function crc32(bytes) {
  let crc = 0xffffffff
  for (const b of bytes) crc = (CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8)) >>> 0
  return (crc ^ 0xffffffff) >>> 0
}

function chunk(type, payload) {
  const length = Buffer.alloc(4)
  length.writeUInt32BE(payload.length, 0)
  const typed = Buffer.concat([Buffer.from(type, 'ascii'), payload])
  const crc = Buffer.alloc(4)
  crc.writeUInt32BE(crc32(typed), 0)
  return Buffer.concat([length, typed, crc])
}

/** `rgba` is width * height * 4 bytes. */
export function encodePng(width, height, rgba) {
  const stride = width * 4
  if (rgba.length !== stride * height) {
    throw new Error(`encodePng: expected ${stride * height} bytes, got ${rgba.length}`)
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0)
  ihdr.writeUInt32BE(height, 4)
  ihdr[8] = 8 // bit depth
  ihdr[9] = 6 // colour type: RGBA
  const raw = Buffer.alloc((stride + 1) * height)
  for (let y = 0; y < height; y++) {
    Buffer.from(rgba.buffer, rgba.byteOffset + y * stride, stride).copy(raw, y * (stride + 1) + 1)
  }
  return Buffer.concat([
    SIGNATURE,
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ])
}

/** Turn a width x height bitmap of 0/1 into RGBA, ink on paper. */
export function bitmapToRgba(width, height, bits, ink = [0xff, 0xff, 0xff], paper = [0, 0, 0]) {
  const rgba = new Uint8Array(width * height * 4)
  for (let i = 0; i < width * height; i++) {
    const c = bits[i] ? ink : paper
    rgba[i * 4] = c[0]
    rgba[i * 4 + 1] = c[1]
    rgba[i * 4 + 2] = c[2]
    rgba[i * 4 + 3] = 0xff
  }
  return rgba
}
