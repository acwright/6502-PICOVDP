#!/usr/bin/env node

// Check that the spec's copies are in step (PLAN.md ground rule 2).
//
//   node tools/check-spec.mjs            (CTest spec_in_step)
//
// SPEC.md here is canonical. The emulator's docs/VDP-SPEC.md must be the same
// bytes, and docs/SPEC.html, the published rendering, must name the same draft
// everywhere it names one: the masthead, the status line and the footer. The
// HTML's prose is not compared with SPEC.md's; that stays a review.
//
//   PICOVDP_EMULATOR   the emulator checkout, if not ../../NodeJS/6502-EMULATOR
//                      from this repository

import { existsSync, readFileSync } from 'node:fs'
import { join, relative } from 'node:path'
import { EMULATOR, REPO } from './lib/emulator.mjs'

const SPEC = join(REPO, 'SPEC.md')
const HTML = join(REPO, 'docs', 'SPEC.html')
const COPY = join(EMULATOR, 'docs', 'VDP-SPEC.md')

const failures = []
const fail = (message) => failures.push(message)

const spec = readFileSync(SPEC)
const status = /^\*\*Status:\*\* draft (\d+\.\d+)\./m.exec(spec.toString('utf8'))
if (!status) {
  console.error(`${relative(REPO, SPEC)}: no "**Status:** draft N.N." line`)
  process.exit(1)
}
const draft = status[1]

if (!existsSync(COPY)) {
  fail(`no ${COPY} (set PICOVDP_EMULATOR)`)
} else if (!spec.equals(readFileSync(COPY))) {
  fail(`${COPY} differs from SPEC.md: copy SPEC.md over it and commit that on main`)
}

// Each place the HTML names the draft, as the page is written today.
const html = readFileSync(HTML, 'utf8')
const places = [
  ['masthead', /<li>Status <b>draft (\d+\.\d+)<\/b><\/li>/],
  ['status line', /<strong>Status:<\/strong> draft (\d+\.\d+)\./],
  ['footer', /6502-PICOVDP &middot; draft (\d+\.\d+) &middot;/],
]
for (const [where, pattern] of places) {
  const found = pattern.exec(html)
  if (!found) fail(`${relative(REPO, HTML)}: no draft named in its ${where}`)
  else if (found[1] !== draft) fail(`${relative(REPO, HTML)}: its ${where} names draft ${found[1]}, SPEC.md draft ${draft}`)
}
if (!html.includes(`<h3>Draft ${draft}</h3>`)) {
  fail(`${relative(REPO, HTML)}: Revision History has no Draft ${draft}`)
}

if (failures.length) {
  for (const message of failures) console.error(message)
  process.exit(1)
}
console.log(`SPEC.md draft ${draft}: the emulator's docs/VDP-SPEC.md is identical, and docs/SPEC.html names draft ${draft}.`)
