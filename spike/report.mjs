#!/usr/bin/env node
// Phase 1 timing spike: turns a capture (spike/capture.mjs) into the Markdown
// tables in docs/results/phase-01.md.
// Usage: node spike/report.mjs <capture> [first-pass capture]

import fs from 'node:fs';

function parse(file) {
  const scenes = new Map(); // `${hz}/${name}` → fields
  const micro = new Map();
  const clocks = [];
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const [kind, ...rest] = line.split(' ');
    const f = Object.fromEntries(rest.map((kv) => kv.split('=')).map(([k, v]) => [k, /^\d+$/.test(v) ? Number(v) : v]));
    if (kind === 'CLOCK') clocks.push(f);
    if (kind === 'SCENE') scenes.set(`${f.hz}/${f.name}`, f);
    if (kind === 'MICRO') micro.set(`${f.hz}/${f.name}`, f);
  }
  return { scenes, micro, clocks };
}

const cap = parse(process.argv[2]);
const first = process.argv[3] ? parse(process.argv[3]) : null;
const [slow, fast] = cap.clocks;
const n = (x) => x.toLocaleString('en-GB');
const pct = (used, budget) => `${Math.round((100 * (budget - used)) / budget)}%`;
const S = (name, hz = slow.hz) => {
  const s = cap.scenes.get(`${hz}/${name}`);
  if (!s) throw new Error(`no scene ${name} at ${hz}`);
  return s;
};

// The counter counts the same cycles whatever the clock: check, then use one.
let worstDiff = 0;
for (const [key, s] of cap.scenes) {
  if (!key.startsWith(`${slow.hz}/`)) continue;
  const t = S(s.name, fast.hz);
  worstDiff = Math.max(worstDiff, Math.abs(t.total_max - s.total_max) / s.total_max);
}

const busPerAccess = cap.micro.get(`${slow.hz}/bus-data-write`).max;
const bus = 32 * busPerAccess; // back-to-back sta abs at 2 MHz: 2 µs each, 63.56 µs a line
const lineStart = cap.micro.get(`${slow.hz}/line-start-32`).max;
const overhead = bus + lineStart;

const out = [];
const p = (s = '') => out.push(s);

p(`Clock-invariance: worst difference in total cycles between ${slow.hz / 1e6} and ${fast.hz / 1e6} MHz is ${(100 * worstDiff).toFixed(2)}%.`);
let worstCheck = 0;
for (const s of cap.scenes.values()) {
  worstCheck = Math.max(worstCheck, Math.abs(s.check_cyc_us1000 - s.check_wall_us1000) / s.check_wall_us1000);
}
p(`Cycles against wall time, uninstrumented: worst disagreement ${(100 * worstCheck).toFixed(4)}%.`);
p();

// §18's table
const g = S('graphics-4bpp-16'), gd = S('graphics-4bpp-16-det'), gm = S('graphics-4bpp-32'), gmd = S('graphics-4bpp-32-det');
const f = S('full-4bpp-16'), fd = S('full-4bpp-16-det'), fm = S('full-4bpp-32'), fmd = S('full-4bpp-32-det');
p('| Work | §18 estimate | Graphics, 256 px | Full, 320 px |');
p('|---|--:|--:|--:|');
p(`| Layer 0, 4bpp | ~800 | ${n(g.l0_max)} | ${n(f.l0_max)} |`);
p(`| Layer 1, 4bpp with transparency merge | ~1,600 | ${n(g.l1_max)} | ${n(f.l1_max)} |`);
p(`| Sprite evaluation, 64 slots | ~500 | ${n(g.eval_max)} | ${n(f.eval_max)} |`);
p(`| Sprite composite, 32 × 16 px | ~2,600 | ${n(g.spr_max)} | ${n(f.spr_max)} |`);
p(`| Border and palette expansion, 320 px | ~1,300 | ${n(g.exp_max)} | ${n(f.exp_max)} |`);
p(`| Bus interrupt service, 32 accesses (2 MHz, one display line) | ~1,200 | ${n(bus)} | ${n(bus)} |`);
p(`| **Total** | **~8,000** | **${n(g.total_max + bus)}** | **${n(f.total_max + bus)}** |`);
p(`| *plus* detailed collision | *~500* | *${n(gd.spr_max - g.spr_max)}* | *${n(fd.spr_max - f.spr_max)}* |`);
p(`| *plus* magnified sprites | *~2,600* | *${n(gm.spr_max - g.spr_max)}* | *${n(fm.spr_max - f.spr_max)}* |`);
p(`| Not in §18: line start, 32 journal entries all in the palette window | — | ${n(lineStart)} | ${n(lineStart)} |`);
p();
p(`Line budget: ${n(slow.budget)} cycles at ${slow.hz / 1e6} MHz, ${n(fast.budget)} at ${fast.hz / 1e6} MHz.`);
p();

// Whole-line worst cases
p('| Scene | Build | + bus and line start | Margin @ 302.4 | Margin @ 352 |');
p('|---|--:|--:|--:|--:|');
for (const s of [g, gd, gm, gmd, f, fd, fm, fmd]) {
  const t = s.total_max + overhead;
  p(`| ${s.name} | ${n(s.total_max)} | ${n(t)} | ${pct(t, slow.budget)} | ${pct(t, fast.budget)} |`);
}
p();

// Depths
p('| Depth | Geometry | Layer 0 | Layer 1 | Sprites 16 | Sprites 16 det | Sprites 32 | Sprites 32 det | Line, worst (32 det) |');
p('|---|---|--:|--:|--:|--:|--:|--:|--:|');
for (const geom of ['graphics', 'full']) {
  for (const d of ['1bpp', '2bpp', '4bpp', '8bpp']) {
    const a = S(`${geom}-${d}-16`), b = S(`${geom}-${d}-16-det`), c = S(`${geom}-${d}-32`), e = S(`${geom}-${d}-32-det`);
    p(`| ${d} | ${geom} | ${n(a.l0_max)} | ${n(a.l1_max)} | ${n(a.spr_max)} | ${n(b.spr_max)} | ${n(c.spr_max)} | ${n(e.spr_max)} | ${n(e.total_max + overhead)} |`);
  }
}
p();

// Still Open 2
p('| 4bpp layers | Graphics L0 | Graphics L1 | Full L0 | Full L1 |');
p('|---|--:|--:|--:|--:|');
p(`| Table (8 KB) | ${n(g.l0_max)} | ${n(g.l1_max)} | ${n(f.l0_max)} | ${n(f.l1_max)} |`);
const gn = S('graphics-4bpp-16-notab'), fn = S('full-4bpp-16-notab');
p(`| Arithmetic | ${n(gn.l0_max)} | ${n(gn.l1_max)} | ${n(fn.l0_max)} | ${n(fn.l1_max)} |`);
p();

// Remedies
p('| Full mode, 4bpp | Sprites | Layers | SPRLIMIT | Line, worst | Margin @ 302.4 | Margin @ 352 |');
p('|---|---|---|--:|--:|--:|--:|');
for (const spr of ['16', '16-det', '32', '32-det']) {
  const rows = [
    [`full-4bpp-${spr}`, 'two', 32],
    [`full-4bpp-${spr}-lim24`, 'two', 24],
    [`full-4bpp-${spr}-lim16`, 'two', 16],
    [`full-4bpp-${spr}-lim8`, 'two', 8],
    [`full-4bpp-${spr}-l0only`, 'one', 32],
    [`full-4bpp-${spr}-lim16-l0only`, 'one', 16],
  ];
  for (const [name, layers, lim] of rows) {
    const s = S(name);
    const t = s.total_max + overhead;
    p(`| ${name} | ${{ '16': '16 × 16', '16-det': '16 × 16, detailed', '32': 'magnified', '32-det': 'magnified, detailed' }[spr]} | ${layers} | ${lim} | ${n(t)} | ${pct(t, slow.budget)} | ${pct(t, fast.budget)} |`);
  }
}
p();

// Pass 1 against this pass
if (first) {
  p('| Scene | Stage | First pass | Final |');
  p('|---|---|--:|--:|');
  for (const name of ['full-4bpp-16', 'full-4bpp-32-det']) {
    const a = first.scenes.get(`${slow.hz}/${name}`), b = S(name);
    for (const st of ['eval', 'l0', 'l1', 'spr', 'exp', 'total']) p(`| ${name} | ${st} | ${n(a[`${st}_max`])} | ${n(b[`${st}_max`])} |`);
  }
  p();
}

// Bus stand-in
p('| Bus interrupt stand-in | Max | Mean |');
p('|---|--:|--:|');
for (const op of ['data-write', 'data-read', 'reg-write', 'status-read']) {
  const m = cap.micro.get(`${slow.hz}/bus-${op}`);
  p(`| ${op} | ${m.max} | ${m.mean} |`);
}
console.log(out.join('\n'));
