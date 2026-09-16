#!/usr/bin/env node

// Pin a copy of 6502-EMULATOR's oracle in tests/oracle/ (PLAN.md section 4).
//
//   node tools/sync-oracle.mjs           copy the goldens and traces, write the manifest
//   node tools/sync-oracle.mjs --check   check tests/oracle against its manifest (CTest)
//
// tests/oracle/ is written by this script and nothing else (ground rule 4). A
// sync takes every PICOVDP golden fixture's checkpoints — index frame, VRAM,
// JSON and PNG — and its trace, from an emulator whose HEAD is on origin/main
// with a clean tree, so the commit the manifest names, with its
// `git describe --tags`, is exactly what was copied. Only the `FIXTURES` entries
// on the PICOVDP card are taken: a TMS9918A golden is not this card's oracle. It
// refuses a trace that does not check against docs/TRACE.md, or whose
// checkpoints are not the fixture's, or lack a class and settle point. A re-sync
// is a commit of its own.
//
// --check needs no emulator: it recomputes every file's SHA-256 and compares
// the directory with the manifest, both ways.

import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { dirname, join, relative } from 'node:path'
import { EMULATOR, REPO, emulatorGit, emulatorPin, isPicovdpFixture, loadFixtures } from './lib/emulator.mjs'
import { TRACE_VERSION, checkpointsOf, readTrace } from './lib/trace.mjs'

const ORACLE = join(REPO, 'tests', 'oracle')
const MANIFEST = join(ORACLE, 'manifest.json')
const README = join(ORACLE, 'README.md')

/** The four files of a golden checkpoint, as the emulator names them. */
const CHECKPOINT_FILES = ['idx.bin', 'vram.bin', 'json', 'png']

function main() {
  const args = process.argv.slice(2)
  if (args.includes('--check')) return check()
  if (args.length) fail('usage: sync-oracle.mjs [--check]')
  sync()
}

function sync() {
  let pin
  try {
    pin = emulatorPin()
  } catch (error) {
    fail(error.message)
  }
  const { branch, commit, describe, committed } = pin
  const { FIXTURES, checkpointsOf: stepsOf } = loadFixtures()
  const others = FIXTURES.filter((fixture) => !isPicovdpFixture(fixture))
  for (const fixture of others) console.log(`Skipping ${fixture.name}: it boots the ${fixture.vdp}, not the PICOVDP.`)
  const goldens = join(EMULATOR, 'src', 'tests', 'goldens')

  // Everything is read and checked before anything is written.
  const files = new Map() // path in tests/oracle -> bytes
  const fixtures = []
  for (const fixture of FIXTURES.filter(isPicovdpFixture)) {
    const traceName = `${fixture.name}/${fixture.name}.vdpt.gz`
    const tracePath = join(goldens, traceName)
    if (!existsSync(tracePath)) fail(`${fixture.name} has no trace — run \`npm run record:traces\` in the emulator`)
    const traceFile = readFileSync(tracePath)
    let trace
    let checkpoints
    try {
      trace = readTrace(traceFile)
      checkpoints = checkpointsOf(trace)
    } catch (error) {
      fail(`${traceName} does not check against docs/TRACE.md: ${error.message}`)
    }
    if (trace.header.fixture !== fixture.name) fail(`${traceName} is a trace of ${trace.header.fixture}`)

    const names = stepsOf(fixture)
    if (checkpoints.map((c) => c.name).join() !== names.join()) {
      fail(`${traceName} has checkpoints ${checkpoints.map((c) => c.name).join(', ')}; the fixture has ${names.join(', ')}`)
    }
    files.set(traceName, traceFile)

    for (const checkpoint of checkpoints) {
      for (const key of ['frame', 'settle', 'window', 'class']) {
        if (checkpoint[key] === undefined) fail(`${fixture.name}/${checkpoint.name} has no ${key} in its trace`)
      }
      for (const extension of CHECKPOINT_FILES) {
        const name = `${fixture.name}/${checkpoint.name}.${extension}`
        if (!existsSync(join(goldens, name))) fail(`the emulator has no ${name}`)
        files.set(name, readFileSync(join(goldens, name)))
      }
      const json = JSON.parse(files.get(`${fixture.name}/${checkpoint.name}.json`))
      if (json.cycles !== checkpoint.cycles) {
        fail(`${fixture.name}/${checkpoint.name}: the golden is at cycle ${json.cycles}, the trace's checkpoint at ${checkpoint.cycles}`)
      }
    }

    fixtures.push({
      name: fixture.name,
      description: fixture.description,
      trace: traceName,
      events: trace.lines.length,
      recordedAt: trace.header.emulator,
      checkpoints
    })
  }

  const manifest = {
    writtenBy: 'tools/sync-oracle.mjs — do not edit tests/oracle by hand',
    emulator: {
      repository: emulatorGit('remote', 'get-url', 'origin'),
      branch,
      commit,
      describe,
      committed
    },
    traceFormat: TRACE_VERSION,
    fixtures,
    files: Object.fromEntries([...files.keys()].sort().map((name) => [name, sha256(files.get(name))]))
  }

  const before = existsSync(MANIFEST) ? JSON.parse(readFileSync(MANIFEST, 'utf8')) : null
  rmSync(ORACLE, { recursive: true, force: true })
  for (const [name, bytes] of files) {
    mkdirSync(dirname(join(ORACLE, name)), { recursive: true })
    writeFileSync(join(ORACLE, name), bytes)
  }
  writeFileSync(MANIFEST, JSON.stringify(manifest, null, 2) + '\n')
  writeFileSync(README, readme(manifest))

  const moved = before
    ? Object.keys(manifest.files).filter((name) => before.files?.[name] !== manifest.files[name])
    : Object.keys(manifest.files)
  const gone = before ? Object.keys(before.files ?? {}).filter((name) => !(name in manifest.files)) : []
  console.log(`Synced ${files.size} files from 6502-EMULATOR ${commit.slice(0, 12)} (${describe}, ${branch}) into ${relative(REPO, ORACLE)}/.`)
  for (const fixture of fixtures) {
    const classes = fixture.checkpoints.map((c) => `${c.name} ${c.class}`).join(', ')
    console.log(`  ${fixture.name}: ${fixture.events} events; ${classes}`)
  }
  console.log(
    before
      ? `${moved.length} file(s) new or changed, ${gone.length} removed since ${before.emulator.commit.slice(0, 12)}.`
      : 'First sync.'
  )
  if (before && moved.length + gone.length === 0 && before.emulator.commit === commit) {
    console.log('Nothing changed.')
  } else {
    console.log('Commit tests/oracle on its own (PLAN.md ground rule 4).')
  }
}

function check() {
  if (!existsSync(MANIFEST)) fail(`no ${relative(REPO, MANIFEST)} — run tools/sync-oracle.mjs`)
  const manifest = JSON.parse(readFileSync(MANIFEST, 'utf8'))
  const problems = []

  const present = listFiles(ORACLE).filter((name) => name !== 'manifest.json' && name !== 'README.md')
  for (const name of present) {
    if (!(name in manifest.files)) problems.push(`${name} is not in the manifest`)
  }
  for (const [name, expected] of Object.entries(manifest.files)) {
    const path = join(ORACLE, name)
    if (!existsSync(path)) problems.push(`${name} is missing`)
    else if (sha256(readFileSync(path)) !== expected) problems.push(`${name} has changed`)
  }
  if (!existsSync(README) || readFileSync(README, 'utf8') !== readme(manifest)) {
    problems.push('README.md is not the one the manifest makes')
  }
  for (const fixture of manifest.fixtures) {
    try {
      const trace = readTrace(readFileSync(join(ORACLE, fixture.trace)))
      const found = checkpointsOf(trace)
      if (JSON.stringify(found) !== JSON.stringify(fixture.checkpoints)) {
        problems.push(`${fixture.trace}'s checkpoints are not the manifest's`)
      }
    } catch (error) {
      problems.push(`${fixture.trace}: ${error.message}`)
    }
  }

  if (problems.length) fail(`tests/oracle does not match its manifest:\n  ${problems.join('\n  ')}`)
  const checkpoints = manifest.fixtures.reduce((sum, fixture) => sum + fixture.checkpoints.length, 0)
  console.log(
    `tests/oracle: ${Object.keys(manifest.files).length} files as synced from 6502-EMULATOR ` +
      `${manifest.emulator.commit.slice(0, 12)}; ${manifest.fixtures.length} fixtures, ${checkpoints} checkpoints.`
  )
}

function readme(manifest) {
  const rows = manifest.fixtures.flatMap((fixture) =>
    fixture.checkpoints.map(
      (c) => `| \`${fixture.name}\` | \`${c.name}\` | ${c.cycles} | ${c.frame} | ${c.settle} | ${c.window} | ${c.class} |`
    )
  )
  return `Oracle
======

**Written by \`tools/sync-oracle.mjs\`. Do not edit anything here by hand**
(PLAN.md ground rule 4). A golden that needs to move is re-captured in
6502-EMULATOR, in a commit of its own, and re-synced here in a commit of its own.

A pinned copy of 6502-EMULATOR's golden checkpoints and the traces of the
programs that produced them, from \`${manifest.emulator.branch}\` at
\`${manifest.emulator.commit}\`${describeOf(manifest)} (${manifest.emulator.committed}).

- \`<fixture>/<checkpoint>.idx.bin\` — the frame as 76,800 palette indices, row-major. **The oracle**: compared exactly.
- \`<fixture>/<checkpoint>.vram.bin\` — all 64 KB of VRAM at the checkpoint.
- \`<fixture>/<checkpoint>.json\` — registers, mode, \`STAT0\`, VRAM hash, text grid.
- \`<fixture>/<checkpoint>.png\` — the frame in colour, to look at.
- \`<fixture>/<fixture>.vdpt.gz\` — the fixture's port traffic, in [docs/TRACE.md](../../docs/TRACE.md)'s format, version ${manifest.traceFormat}.
- \`manifest.json\` — the emulator commit, each checkpoint's class and settle point, and every file's SHA-256.

\`node tools/sync-oracle.mjs --check\` (CTest \`oracle_pinned\`) checks this directory against the manifest.

| Fixture | Checkpoint | Cycles | Frame | Settle | Window | Class |
|---|---|--:|--:|--:|--:|---|
${rows.join('\n')}
`
}

/** The README's `, `v3.0.0`` after the commit, when the manifest records a describe. */
function describeOf(manifest) {
  return manifest.emulator.describe ? `, \`${manifest.emulator.describe}\`` : ''
}

function listFiles(root, prefix = '') {
  if (!existsSync(root)) return []
  return readdirSync(root).flatMap((name) => {
    const path = join(root, name)
    const relativeName = prefix ? `${prefix}/${name}` : name
    return statSync(path).isDirectory() ? listFiles(path, relativeName) : [relativeName]
  })
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex')
}

function fail(message) {
  console.error(`sync-oracle: ${message}`)
  process.exit(1)
}

main()
