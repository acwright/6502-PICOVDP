// Where 6502-EMULATOR is, and what of it the tools load.
//
//   PICOVDP_EMULATOR   the emulator checkout, if not ../../NodeJS/6502-EMULATOR
//                      from this repository

import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { createRequire } from 'node:module'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

export const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')
export const EMULATOR = resolve(process.env.PICOVDP_EMULATOR ?? join(REPO, '..', '..', 'NodeJS', '6502-EMULATOR'))
/** The branch the oracle is pinned from: emulator VDP work lands on main since 3.0.0's merge. */
export const EMULATOR_BRANCH = 'main'
/** The card `fixtures.js` names with `vdp`; a fixture that names none boots on it. */
export const PICOVDP_CARD = 'picovdp'

export function emulatorGit(...args) {
  return execFileSync('git', args, { cwd: EMULATOR, encoding: 'utf8' }).trim()
}

/**
 * The emulator commit the oracle may be pinned from: any commit on origin/main
 * (VDP-PLAN.md decision 6), in a clean tree, so the commit is exactly what was
 * read. Throws if the checkout is not one; returns what the manifest records.
 */
export function emulatorPin() {
  if (!existsSync(join(EMULATOR, '.git'))) throw new Error(`no emulator checkout at ${EMULATOR} (set PICOVDP_EMULATOR)`)
  const upstream = `origin/${EMULATOR_BRANCH}`
  const commit = emulatorGit('rev-parse', 'HEAD')
  try {
    emulatorGit('merge-base', '--is-ancestor', commit, upstream)
  } catch {
    throw new Error(`the emulator's HEAD ${commit.slice(0, 12)} is not on ${upstream}`)
  }
  const dirty = emulatorGit('status', '--porcelain')
  if (dirty) throw new Error(`the emulator's tree is not clean:\n${dirty}`)
  return {
    branch: EMULATOR_BRANCH,
    commit,
    describe: emulatorGit('describe', '--tags', '--always', commit),
    committed: emulatorGit('show', '-s', '--format=%cI', commit)
  }
}

/** Whether a `fixtures.js` entry boots on the PICOVDP, the card this oracle is of. */
export function isPicovdpFixture(fixture) {
  return (fixture.vdp ?? PICOVDP_CARD) === PICOVDP_CARD
}

const require = createRequire(join(EMULATOR, 'package.json'))

/** The emulator's fixture list: plain CommonJS, no build needed. */
export function loadFixtures() {
  const path = join(EMULATOR, 'src', 'tests', 'goldens', 'fixtures.js')
  if (!existsSync(path)) throw new Error(`no emulator at ${EMULATOR} (set PICOVDP_EMULATOR)`)
  return require(path)
}

/** `Video.ts` as the emulator's `npm run build:cli` compiles it. */
export function loadVideo() {
  const path = join(EMULATOR, 'out', 'core', 'IO', 'Video.js')
  if (!existsSync(path)) {
    throw new Error(`no compiled Video at ${path} — run \`npm run build:cli\` in the emulator`)
  }
  return require(path).Video
}

/** The adapter the core is presented through, as the emulator's jest.picovdp.cjs takes it. */
export function loadAdapter() {
  const path = resolve(process.env.PICOVDP_ADDON ?? join(REPO, 'host', 'node', 'Video.cjs'))
  return createRequire(import.meta.url)(path).Video
}

