// The capture card, through ffmpeg's avfoundation input (docs/BENCH.md
// section 6). Frames come back as raw RGB so nothing has to decode a PNG.
//
// The card is exclusive: QuickTime or anything else holding it will make this
// fail. The first run also triggers macOS's camera permission prompt for
// whatever is calling ffmpeg.

import { execFile, execFileSync } from 'node:child_process'

export const DEFAULT_DEVICE = '0'
export const WIDTH = 640
export const HEIGHT = 480

/**
 * Grab one frame, as { width, height, rgb }. Several frames are pulled and the
 * last kept, because the first out of a capture card is often half a field.
 */
export function grab({ device = DEFAULT_DEVICE, width = WIDTH, height = HEIGHT, skip = 8 } = {}) {
  const frame = width * height * 3
  let out
  try {
    out = execFileSync('ffmpeg', [
      '-hide_banner', '-loglevel', 'error',
      '-f', 'avfoundation',
      '-framerate', '30',
      '-video_size', `${width}x${height}`,
      '-i', String(device),
      '-frames:v', String(skip),
      '-pix_fmt', 'rgb24',
      '-f', 'rawvideo', '-',
    ], { maxBuffer: frame * (skip + 2), stdio: ['ignore', 'pipe', 'pipe'] })
  } catch (error) {
    const why = (error.stderr?.toString() ?? '').trim().split('\n').slice(-3).join('; ')
    throw new Error(`ffmpeg could not read the capture card: ${why || error.message}`)
  }
  if (out.length < frame) throw new Error(`ffmpeg returned ${out.length} bytes, expected at least ${frame}`)
  return { width, height, rgb: out.subarray(out.length - frame) }
}

/**
 * The same without blocking the process, so a serial port it is also serving
 * goes on being read while ffmpeg runs (Phase 13's load run).
 */
export function grabAsync({ device = DEFAULT_DEVICE, width = WIDTH, height = HEIGHT, skip = 8 } = {}) {
  const frame = width * height * 3
  return new Promise((resolve, reject) => {
    execFile('ffmpeg', [
      '-hide_banner', '-loglevel', 'error',
      '-f', 'avfoundation',
      '-framerate', '30',
      '-video_size', `${width}x${height}`,
      '-i', String(device),
      '-frames:v', String(skip),
      '-pix_fmt', 'rgb24',
      '-f', 'rawvideo', '-',
    ], { encoding: 'buffer', maxBuffer: frame * (skip + 2) }, (error, out, err) => {
      if (error) return reject(new Error(`ffmpeg could not read the capture card: ${(err?.toString() ?? '').trim().split('\n').slice(-3).join('; ') || error.message}`))
      if (out.length < frame) return reject(new Error(`ffmpeg returned ${out.length} bytes, expected at least ${frame}`))
      resolve({ width, height, rgb: out.subarray(out.length - frame) })
    })
  })
}

/** Threshold to ink/paper on luminance. */
export function toBits({ width, height, rgb }, threshold = 96) {
  const bits = new Uint8Array(width * height)
  for (let i = 0; i < width * height; i++) {
    const luma = (rgb[i * 3] * 299 + rgb[i * 3 + 1] * 587 + rgb[i * 3 + 2] * 114) / 1000
    bits[i] = luma >= threshold ? 1 : 0
  }
  return bits
}

/** The smallest box holding every set bit, or null if none is set. */
export function inkBounds(width, height, bits) {
  let x0 = width, y0 = height, x1 = -1, y1 = -1
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (!bits[y * width + x]) continue
      if (x < x0) x0 = x
      if (x > x1) x1 = x
      if (y < y0) y0 = y
      if (y > y1) y1 = y
    }
  }
  return x1 < 0 ? null : { x0, y0, x1, y1, width: x1 - x0 + 1, height: y1 - y0 + 1 }
}
