'use strict'

// CTest: the addon loads in Node, the binding is whole, and the adapter drives
// the core through a frame. Says nothing about SPEC.md — Jest and the fuzzer do.
//
//   node host/node/smoke.cjs <path to picovdp.node>

const assert = require('node:assert/strict')

const binary = process.argv[2]
assert.ok(binary, 'usage: smoke.cjs <picovdp.node>')
process.env.PICOVDP_NODE_BINARY = binary

const core = require(binary)
for (const name of ['create', 'reset', 'read', 'write', 'lineStart', 'setHblank', 'buildLine', 'expandLine', 'intAsserted']) {
  assert.equal(typeof core[name], 'function', `the binding has no ${name}`)
}
assert.throws(() => core.read({}, 0), TypeError, 'a non-card is refused')
assert.throws(() => core.buildLine(core.create(), new Uint8Array(10)), TypeError, 'a short line is refused')

const { Video, DISPLAY_WIDTH, DISPLAY_HEIGHT } = require('./Video.cjs')
const video = new Video()
video.write(1, 0x40)
video.write(1, 0x81)
video.read(1)
let interrupts = 0
for (let cycle = 0; cycle < 1_000_000 / 60 + 100; cycle++) interrupts |= video.tick(1_000_000)
assert.equal(video.frameReady, true, 'a frame is presented within a frame')
assert.equal(video.frameIndices().length, DISPLAY_WIDTH * DISPLAY_HEIGHT)
assert.equal(interrupts, 0)
video.reset(true)

console.log('picovdp addon: binding whole, adapter presents frames')
