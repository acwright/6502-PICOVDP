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
/** PLAN.md ground rule 3: the emulator's VDP work lives on this branch only. */
export const EMULATOR_BRANCH = 'v3-vdp'

export function emulatorGit(...args) {
  return execFileSync('git', args, { cwd: EMULATOR, encoding: 'utf8' }).trim()
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

