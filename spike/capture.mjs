#!/usr/bin/env node
// Phase 1 timing spike: runs the suite on a Pico 2 flashed with
// picovdp_spike and saves what it prints.
// Usage: node spike/capture.mjs <out-file> [port]

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';

const out = process.argv[2];
const port = process.argv[3] ?? fs.readdirSync('/dev').filter((f) => f.startsWith('cu.usbmodem')).map((f) => `/dev/${f}`)[0];
if (!out || !port) {
  console.error('usage: capture.mjs <out-file> [port]');
  process.exit(2);
}

execFileSync('stty', ['-f', port, '115200', 'raw', '-echo']);
const fd = fs.openSync(port, fs.constants.O_RDWR | fs.constants.O_NOCTTY);
const input = fs.createReadStream(null, { fd, autoClose: false });
let text = '';
let begun = false;
const timeout = setTimeout(() => {
  console.error('timed out; got:\n' + text.slice(-2000));
  process.exit(1);
}, 300_000);

input.on('data', (chunk) => {
  text += chunk.toString('latin1');
  if (!begun && text.includes('BEGIN')) {
    begun = true;
    text = text.slice(text.indexOf('BEGIN'));
  }
  if (!begun && text.includes('ready')) {
    fs.writeSync(fd, 'r');
    text = '';
  }
  if (begun && /^END\r?$/m.test(text)) {
    clearTimeout(timeout);
    fs.writeFileSync(out, text.replace(/\r/g, '').slice(0, text.replace(/\r/g, '').indexOf('END\n') + 4));
    console.log(`saved ${out}`);
    process.exit(0);
  }
});
fs.writeSync(fd, 'r');
