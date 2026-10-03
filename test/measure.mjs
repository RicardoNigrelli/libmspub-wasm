#!/usr/bin/env node
// Sizes of every build (raw / gzip -9 / brotli q11) and load time in Node:
// each iteration creates a fresh instance (fetch from disk + compile +
// instantiate), so it measures a cold start without the wasm code cache.
//   node test/measure.mjs [--runs 10]
// SPDX-License-Identifier: MPL-2.0
import { readFileSync, existsSync } from 'node:fs';
import { gzipSync, brotliCompressSync, constants } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const runsArg = process.argv.indexOf('--runs');
const runs = runsArg > 0 ? Number(process.argv[runsArg + 1]) : 10;
const variants = ['O3', 'Oz', 'Oz-stubdata'];
const kb = (n) => (n / 1024).toFixed(1);
const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

console.log('| Build | Archivo | Bytes | KB | gzip -9 (KB) | brotli q11 (KB) |');
console.log('|---|---|---:|---:|---:|---:|');
for (const v of variants) {
  for (const f of ['libmspub.wasm', 'libmspub.js']) {
    const p = join(here, '..', 'dist', v, f);
    if (!existsSync(p)) continue;
    const buf = readFileSync(p);
    const gz = gzipSync(buf, { level: 9 }).length;
    const br = brotliCompressSync(buf, { params: { [constants.BROTLI_PARAM_QUALITY]: 11 } }).length;
    console.log(`| ${v} | ${f} | ${buf.length} | ${kb(buf.length)} | ${kb(gz)} | ${kb(br)} |`);
  }
}

console.log(`\nCarga en Node ${process.version} (${runs} instancias nuevas, mediana / mín / máx):`);
for (const v of variants) {
  const url = pathToFileURL(join(here, '..', 'dist', v, 'libmspub.js')).href;
  if (!existsSync(fileURLToPath(url))) continue;
  const { default: create } = await import(url);
  const times = [];
  for (let i = 0; i < runs; i++) {
    const t = performance.now();
    await create({ print() {}, printErr() {} });
    times.push(performance.now() - t);
  }
  console.log(`  ${v.padEnd(12)} ${median(times).toFixed(1)} ms / ${Math.min(...times).toFixed(1)} / ${Math.max(...times).toFixed(1)}`);
}
