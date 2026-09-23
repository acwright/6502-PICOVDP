// The picture on the monitor, against the golden (PLAN.md Phase 10).
//
// The injection proves the frame digitally: all 76,800 indices of the golden,
// as they went to VGA (docs/DEBUGLINK.md's SNAPSHOT). What it cannot prove is
// the rest of the way — the DAC's twelve pins, the ×2 across, the two VGA
// lines a card line is sent as, and §11's palette expansion. That is what a
// capture is for, and it is an analog path: a resistor DAC, an FFC, a
// VGA-to-HDMI dongle and a capture card, none of which carry a 4-bit level
// unchanged.
//
// So the comparison is made in the terms the path allows:
//
//   1. **Where it sits.** The golden is searched against the capture over ±8
//      pixels. The offset is the dongle's, not the card's, and it is the same
//      for every picture on one bench.
//   2. **Black level and gain.** One scale and one offset per channel, fitted
//      over the pixels being scored. A wrong bit order is not a scale: red and
//      blue swapped, or a reversed nibble, leaves a residual that no affine fit
//      removes, which is what `MAPPINGS` measures.
//   3. **Settled pixels.** A colour change takes the path about eight capture
//      pixels to settle (docs/results/phase-10.md). Pixels that far from any
//      change in the golden are compared one for one; everything nearer is
//      counted and left out. A picture with detail at every pixel — the
//      oracle's densest — has few of them or none, and says so.
//   4. **The whole picture.** Mean absolute error over every pixel, with the
//      edges' ringing in it, and the same for each wrong mapping.

import { WIDTH, HEIGHT } from './capture.mjs'

/** §11: the palette entry an index draws, from a VRAM image and PALBASE. */
export function paletteOf(vram, registers) {
  const base = (registers[0x0c] & 0x3f) << 10
  const entries = []
  for (let entry = 0; entry < 256; entry++) {
    const at = (base + 2 * entry) & 0xffff
    entries.push([vram[at] & 0x0f, (vram[(at + 1) & 0xffff] >> 4) & 0x0f, vram[(at + 1) & 0xffff] & 0x0f])
  }
  return entries
}

/** What a golden index frame should look like on a 640 x 480 capture: each pixel 2 x 2, each level x 17. */
export function expectedFrame(indices, palette, map = (rgb) => rgb) {
  const rgb = Buffer.alloc(WIDTH * HEIGHT * 3)
  for (let y = 0; y < 240; y++) {
    for (let x = 0; x < 320; x++) {
      const [r, g, b] = map(palette[indices[y * 320 + x]])
      for (let dy = 0; dy < 2; dy++) {
        for (let dx = 0; dx < 2; dx++) {
          const at = ((y * 2 + dy) * WIDTH + x * 2 + dx) * 3
          rgb[at] = r * 17
          rgb[at + 1] = g * 17
          rgb[at + 2] = b * 17
        }
      }
    }
  }
  return rgb
}

const reverse = (v) => ((v & 1) << 3) | ((v & 2) << 1) | ((v & 4) >> 1) | ((v & 8) >> 3)

/** Ways the twelve DAC pins could be wrong. The golden's own mapping must beat all of them. */
export const MAPPINGS = {
  'red and blue swapped': ([r, g, b]) => [b, g, r],
  'green and blue swapped': ([r, g, b]) => [r, b, g],
  'channels rotated': ([r, g, b]) => [g, b, r],
  'every nibble reversed': ([r, g, b]) => [reverse(r), reverse(g), reverse(b)],
  'red nibble reversed': ([r, g, b]) => [reverse(r), g, b],
}

const pixel = (frame, stride, x, y, channel) => frame[(y * WIDTH + x) * stride + channel]

/** Where the capture sits against the golden: the offset with the least mean error, over ±range. */
export function align(expected, capture, range = 8) {
  let best = { dx: 0, dy: 0, mae: Infinity }
  for (let dy = -range; dy <= range; dy++) {
    for (let dx = -range; dx <= range; dx++) {
      let sum = 0
      let n = 0
      for (let y = 16; y < HEIGHT - 16; y += 3) {
        for (let x = 16; x < WIDTH - 16; x += 3) {
          const sx = x + dx
          const sy = y + dy
          if (sx < 0 || sy < 0 || sx >= WIDTH || sy >= HEIGHT) continue
          for (let c = 0; c < 3; c++) sum += Math.abs(pixel(expected, 3, x, y, c) - pixel(capture.rgb, 3, sx, sy, c))
          n += 3
        }
      }
      const mae = sum / n
      if (mae < best.mae) best = { dx, dy, mae }
    }
  }
  return best
}

/**
 * How far from a colour change the path has settled, in capture pixels. Eight
 * across is the analog bandwidth — about 300 ns. Down, a change of brightness
 * alone costs nothing (one-pixel black and white stripes come back two exact
 * rows at a time), but a change of colour takes several rows, because the
 * capture card carries chroma at half resolution both ways; measured on this
 * bench, a picture is within 8 levels 99.8% of the way at three rows and
 * 99.9% at six, so the gate takes eight both ways.
 */
export const SETTLES = { x: 8, y: 8 }

/**
 * A swatch eight card pixels tall — the palette card's, which is what fits 256
 * of them on one screen — has nothing left at `SETTLES`. Its mean colour is
 * the same measured two rows in as five (12.14 against 12.31 levels over the
 * 256 entries), so `entryColours` is given this reach instead, and it is a
 * measurement rather than a gate.
 */
export const ENTRY_REACH = { x: 8, y: 2 }

/**
 * Pixels at least `reach` from any colour change in the golden, and inside the
 * frame once the offset is applied. Eroding a change mask, which is separable,
 * rather than searching a neighbourhood per pixel.
 */
export function settled(expected, offset, reach = SETTLES) {
  const changed = new Uint8Array(WIDTH * HEIGHT)
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const at = (y * WIDTH + x) * 3
      let edge = x === 0 || y === 0 || x === WIDTH - 1 || y === HEIGHT - 1
      if (!edge) {
        for (const other of [at - 3, at + 3, at - WIDTH * 3, at + WIDTH * 3]) {
          if (expected[other] !== expected[at] || expected[other + 1] !== expected[at + 1] || expected[other + 2] !== expected[at + 2]) {
            edge = true
            break
          }
        }
      }
      changed[y * WIDTH + x] = edge ? 1 : 0
    }
  }
  // Dilate the change mask by the reach, in x then in y.
  const spread = (source, stride, length, lines, lineStride, radius) => {
    const out = new Uint8Array(source.length)
    for (let line = 0; line < lines; line++) {
      let since = length
      for (let i = 0; i < length; i++) {
        const at = line * lineStride + i * stride
        if (source[at]) since = 0
        else since++
        if (since <= radius) out[at] = 1
      }
      since = length
      for (let i = length - 1; i >= 0; i--) {
        const at = line * lineStride + i * stride
        if (source[at]) since = 0
        else since++
        if (since <= radius) out[at] = 1
      }
    }
    return out
  }
  const wide = spread(changed, 1, WIDTH, HEIGHT, WIDTH, reach.x)
  const both = spread(wide, WIDTH, HEIGHT, WIDTH, 1, reach.y)

  const mask = new Uint8Array(WIDTH * HEIGHT)
  let count = 0
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const sx = x + offset.dx
      const sy = y + offset.dy
      if (sx < 0 || sy < 0 || sx >= WIDTH || sy >= HEIGHT) continue
      if (both[y * WIDTH + x]) continue
      mask[y * WIDTH + x] = 1
      count++
    }
  }
  return { mask, count }
}

/** One scale and one black level per channel, by least squares over the mask. */
export function calibrate(expected, capture, offset, mask) {
  const gain = []
  const black = []
  for (let c = 0; c < 3; c++) {
    let sx = 0
    let sy = 0
    let sxx = 0
    let sxy = 0
    let n = 0
    for (let y = 0; y < HEIGHT; y++) {
      for (let x = 0; x < WIDTH; x++) {
        if (mask && !mask[y * WIDTH + x]) continue
        const sxp = x + offset.dx
        const syp = y + offset.dy
        if (sxp < 0 || syp < 0 || sxp >= WIDTH || syp >= HEIGHT) continue
        const e = pixel(expected, 3, x, y, c)
        const a = pixel(capture.rgb, 3, sxp, syp, c)
        sx += e
        sy += a
        sxx += e * e
        sxy += e * a
        n++
      }
    }
    const spread = n * sxx - sx * sx
    // A picture of one colour cannot say what the gain is; take it as 1.
    const g = Math.abs(spread) < 1e-6 ? 1 : (n * sxy - sx * sy) / spread
    gain.push(g)
    black.push((sy - g * sx) / n)
  }
  return { gain, black }
}

/** Error per pixel after calibration: the largest of the three channels. */
export function measure(expected, capture, offset, mask, fit) {
  let n = 0
  let sum = 0
  let worst = 0
  const histogram = new Uint32Array(256)
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      if (mask && !mask[y * WIDTH + x]) continue
      const sx = x + offset.dx
      const sy = y + offset.dy
      if (sx < 0 || sy < 0 || sx >= WIDTH || sy >= HEIGHT) continue
      let error = 0
      for (let c = 0; c < 3; c++) {
        const want = fit.gain[c] * pixel(expected, 3, x, y, c) + fit.black[c]
        error = Math.max(error, Math.abs(want - pixel(capture.rgb, 3, sx, sy, c)))
      }
      sum += error
      if (error > worst) worst = error
      histogram[Math.min(255, Math.round(error))]++
      n++
    }
  }
  const within = (levels) => {
    let held = 0
    for (let i = 0; i <= levels; i++) held += histogram[i]
    return n ? held / n : 1
  }
  const rates = { 4: within(4), 8: within(8), 16: within(16), 32: within(32) }
  return { n, mae: n ? sum / n : 0, worst, within, rates }
}

/**
 * The capture tolerance: what this bench was measured to deliver in Phase 10,
 * with margin. A level is 1/255 and a step of the DAC is 17 of them.
 *
 * - **Where the picture sits.** −1 to 4 pixels across and 0 to 3 lines up, over
 *   the twenty pictures of Phase 10. Two things move it, neither of them the
 *   firmware: the dongle resamples at some fraction of a pixel, so an integer
 *   search lands differently depending on what the picture is made of, and
 *   where it locks sync changes when the board is reset or reflashed — the
 *   whole set shifted by a pixel and a line after one reflash. A picture the
 *   firmware put in the wrong place would be out by tens.
 * - **Settled pixels.** Every oracle checkpoint: 99.9% or better within 8
 *   levels, the worst pixel 15. The gate is 99% within 8. A settled pixel is
 *   one at least `SETTLES` from any colour change in the golden.
 * - **The whole picture, edges and all.** 4 to 27 levels, worst on the frames
 *   whose detail is two pixels wide everywhere. The gate is 32.
 * - **The mapping.** The golden's own 0x0BGR beat all five wrong mappings on
 *   all twenty pictures. Only that ordering is required here — by how much is
 *   a property of the picture, and a picture with three colours in it cannot
 *   say. The DAC card settles it outright (`dacResponse`).
 */
export const TOLERANCE = {
  offset: { dx: 1.5, dy: -1.5, slack: 5 },  // where the dongle puts the picture
  settledLevels: 8,                        // half a DAC step
  settledRate: 0.99,
  pictureMae: 32,                          // 2 steps, the edges' ringing included
}

/**
 * A bench card's, which is the same but for the colour of a settled pixel.
 * The DAC card is made of saturated primaries at all sixteen levels, which is
 * where the path's own S-curve is worst — red $C reads 226 where $F reads 243
 * — and no single gain and black level fits that to half a step. The card is
 * the instrument that measures the curve; it is not measured by it.
 */
export const CARD_TOLERANCE = { ...TOLERANCE, settledLevels: 32 }

/**
 * Compare one capture with one golden. `golden` is { indices, vram, registers }.
 * Returns the measurements and the problems, against TOLERANCE.
 */
export function compareCapture(golden, capture, { reach = SETTLES, tolerance = TOLERANCE } = {}) {
  const palette = paletteOf(golden.vram, golden.registers)
  const expected = expectedFrame(golden.indices, palette)
  const offset = align(expected, capture)
  const { mask, count } = settled(expected, offset, reach)

  const whole = calibrate(expected, capture, offset, null)
  const picture = measure(expected, capture, offset, null, whole)

  let settledResult = null
  if (count >= 1000) {
    const fit = calibrate(expected, capture, offset, mask)
    const m = measure(expected, capture, offset, mask, fit)
    settledResult = { ...m, count, fit, levels: tolerance.settledLevels, rate: m.within(tolerance.settledLevels) }
  }

  const mappings = {}
  for (const [name, map] of Object.entries(MAPPINGS)) {
    const wrong = expectedFrame(golden.indices, palette, map)
    const fit = calibrate(wrong, capture, offset, null)
    mappings[name] = measure(wrong, capture, offset, null, fit).mae
  }
  const bestWrong = Math.min(...Object.values(mappings))

  const problems = []
  if (Math.abs(offset.dx - tolerance.offset.dx) > tolerance.offset.slack ||
      Math.abs(offset.dy - tolerance.offset.dy) > tolerance.offset.slack) {
    problems.push(`the picture sits at ${offset.dx},${offset.dy}, not ${tolerance.offset.dx}±${tolerance.offset.slack},${tolerance.offset.dy}±${tolerance.offset.slack}`)
  }
  if (settledResult && settledResult.rate < tolerance.settledRate) {
    problems.push(`${(100 * settledResult.rate).toFixed(2)}% of ${count} settled pixels are within ${tolerance.settledLevels} levels, not ${(100 * tolerance.settledRate).toFixed(0)}%`)
  }
  if (picture.mae > tolerance.pictureMae) {
    problems.push(`the picture is ${picture.mae.toFixed(1)} levels out on average, past ${tolerance.pictureMae}`)
  }
  if (bestWrong <= picture.mae) {
    const name = Object.entries(mappings).find(([, mae]) => mae === bestWrong)[0]
    problems.push(`"${name}" explains the capture at least as well (${bestWrong.toFixed(1)} against ${picture.mae.toFixed(1)})`)
  }

  return { offset, mask, picture, settled: settledResult, mappings, bestWrong, expected, palette, problems }
}

/**
 * What each palette entry actually reached the monitor as: the mean captured
 * colour over that entry's settled pixels, for every entry with enough of
 * them. On a card of flat swatches this is the path's response, measured.
 */
export function entryColours(golden, capture, offset, mask, least = 100) {
  const sums = new Map()
  for (let y = 0; y < 240; y++) {
    for (let x = 0; x < 320; x++) {
      const entry = golden.indices[y * 320 + x]
      for (let dy = 0; dy < 2; dy++) {
        for (let dx = 0; dx < 2; dx++) {
          const px = x * 2 + dx
          const py = y * 2 + dy
          if (!mask[py * WIDTH + px]) continue
          const seen = sums.get(entry) ?? { n: 0, sum: [0, 0, 0] }
          for (let c = 0; c < 3; c++) seen.sum[c] += pixel(capture.rgb, 3, px + offset.dx, py + offset.dy, c)
          seen.n++
          sums.set(entry, seen)
        }
      }
    }
  }
  const palette = paletteOf(golden.vram, golden.registers)
  const found = []
  for (const [entry, seen] of sums) {
    if (seen.n < least) continue
    found.push({ entry, pixels: seen.n, want: palette[entry], got: seen.sum.map((v) => v / seen.n) })
  }
  return found.sort((a, b) => a.entry - b.entry)
}

/**
 * The DAC card's answer (tools/lib/cards.mjs): its entries $00-$0F are red
 * 0-15 with nothing else on, $10-$1F green, $20-$2F blue, $30-$3F grey. So
 * each channel's sixteen levels are measured on their own, and the two
 * channels that should be dark say how much of one pin reaches another.
 *
 * A pin out of order cannot survive this: a swapped channel puts the level on
 * the wrong output, and a reversed nibble makes a ramp that falls.
 */
export function dacResponse(entries) {
  const channels = ['red', 'green', 'blue', 'grey']
  const ramps = channels.map((name, channel) => {
    const levels = []
    for (let level = 0; level < 16; level++) {
      const found = entries.find((e) => e.entry === channel * 16 + level)
      levels.push(found ? found.got : null)
    }
    return { name, channel, levels }
  })
  const problems = []
  for (const ramp of ramps) {
    if (ramp.levels.some((level) => level === null)) {
      problems.push(`${ramp.name}: ${ramp.levels.filter((l) => !l).length} of the sixteen levels have too few settled pixels`)
      continue
    }
    const own = ramp.channel === 3 ? [0, 1, 2] : [ramp.channel]
    const others = [0, 1, 2].filter((c) => !own.includes(c))
    for (let level = 1; level < 16; level++) {
      const here = Math.max(...own.map((c) => ramp.levels[level][c]))
      const before = Math.max(...own.map((c) => ramp.levels[level - 1][c]))
      if (here <= before) problems.push(`${ramp.name}: level $${level.toString(16).toUpperCase()} reads ${here.toFixed(1)}, no brighter than $${(level - 1).toString(16).toUpperCase()}'s ${before.toFixed(1)}`)
    }
    const leak = Math.max(0, ...ramp.levels.flatMap((level) => others.map((c) => level[c])))
    if (leak > 4) problems.push(`${ramp.name}: ${leak.toFixed(1)} levels of it reach another channel`)
    ramp.leak = leak
  }
  return { ramps, problems }
}

/** A line for the log. */
export function describeCapture(result) {
  const parts = [`picture at ${result.offset.dx},${result.offset.dy}`]
  parts.push(`MAE ${result.picture.mae.toFixed(1)}`)
  if (result.settled) {
    parts.push(`${result.settled.count} settled px ${(100 * result.settled.rate).toFixed(2)}% within ${result.settled.levels} (worst ${result.settled.worst.toFixed(0)})`)
  } else {
    parts.push('no settled pixels')
  }
  parts.push(`best wrong mapping ${result.bestWrong.toFixed(1)}`)
  return parts.join(', ')
}
