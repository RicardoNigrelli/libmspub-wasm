#!/usr/bin/env node
// Runs every .pub in a folder through the wasm build and reports, per file:
// supported, pages, times, JSON size and errors/crashes.
// Each file runs in its own worker thread with a timeout, so a crash (wasm
// trap) or a hang in libmspub is reported instead of killing the run.
//
//   node test/run-corpus.mjs [corpusDir=./corpus] [--variant Oz|O3] [--timeout 30000]
//                            [--repeats 3] [--out out]
// SPDX-License-Identifier: MPL-2.0
import { Worker } from 'node:worker_threads';
import { readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : def;
};
const positional = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));
const corpus = resolve(positional[0] || join(here, '..', 'corpus'));
const variant = opt('variant', 'Oz');
const timeoutMs = Number(opt('timeout', 30000));
const repeats = Number(opt('repeats', 3));
const outDir = resolve(opt('out', join(here, '..', 'out', variant)));

function runOne(file) {
  return new Promise((res) => {
    const started = performance.now();
    const w = new Worker(join(here, 'worker.mjs'), {
      workerData: { file, variant, outDir, repeats },
      resourceLimits: { maxOldGenerationSizeMb: 2048 },
    });
    let done = false;
    const finish = (r) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      r.wallMs = performance.now() - started;
      res(r);
    };
    const timer = setTimeout(() => {
      w.terminate();
      finish({ file: file.split(/[\\/]/).pop(), fatal: `timeout after ${timeoutMs} ms` });
    }, timeoutMs);
    w.on('message', finish);
    w.on('error', (e) => finish({ file: file.split(/[\\/]/).pop(), fatal: `worker error: ${e.message}` }));
    w.on('exit', (code) => finish({ file: file.split(/[\\/]/).pop(), fatal: `worker exited with code ${code}` }));
  });
}

const files = readdirSync(corpus).filter((f) => f.toLowerCase().endsWith('.pub')).sort();
console.log(`corpus: ${corpus} (${files.length} files), variant ${variant}, ${repeats} repeats, timeout ${timeoutMs} ms\n`);

const results = [];
for (const f of files) {
  const r = await runOne(join(corpus, f));
  results.push(r);
  const status = r.fatal ? `FATAL ${r.fatal}`
    : !r.supported ? 'not supported'
    : (r.svgError || r.jsonError) ? `ERROR svg=${r.svgError || 'ok'} json=${r.jsonError || 'ok'}`
    : 'ok';
  console.log(`${f.padEnd(70)} ${status}`);
}

const fmt = (x, d = 1) => (x === undefined || x === null ? '' : Number(x).toFixed(d));
const kb = (x) => (x === undefined ? '' : (x / 1024).toFixed(1));
const lines = [
  '| Archivo | KB | ¿Soportado? | Páginas | parse JSON (ms) | parse SVG (ms) | JSON (KB) | SVG (KB) | Texto (cajas / caracteres) | Formas | Imágenes | Tablas | Resultado |',
  '|---|---:|:---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|',
];
for (const r of results) {
  const s = r.summary || {};
  const c = s.counts || {};
  const shapes = ['rect', 'ellipse', 'polygon', 'polyline', 'path', 'connector'].reduce((n, k) => n + (c[k] || 0), 0);
  const status = r.fatal ? `**crash/fatal**: ${r.fatal}`
    : r.supported === false ? 'no soportado (rechazado limpio)'
    : (r.svgError || r.jsonError) ? `error: ${r.jsonError || r.svgError}`
    : r.parseOk === false ? 'parcial (parse() = false)'
    : r.pages === 0 ? 'vacío (parse() = true, 0 páginas)'
    : 'ok';
  lines.push(`| ${r.file} | ${kb(r.bytes)} | ${r.supported === undefined ? '?' : r.supported ? 'sí' : 'no'} | ${r.pages ?? ''} | ${fmt(r.jsonMs)} | ${fmt(r.svgMs)} | ${kb(r.jsonBytes)} | ${kb(r.svgBytes)} | ${s.textBoxes ?? ''} / ${s.textChars ?? ''} | ${r.summary ? shapes : ''} | ${s.images ? `${s.images} (${Object.entries(s.imageTypes).map(([k, v]) => `${v} ${String(k).replace('image/', '')}`).join(', ')})` : s.images ?? ''} | ${s.tables ?? ''} | ${status} |`);
}
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, 'results.json'), JSON.stringify(results, null, 2));
writeFileSync(join(outDir, 'results.md'), lines.join('\n') + '\n');
console.log('\n' + lines.join('\n'));
console.log(`\nresults: ${join(outDir, 'results.json')}`);

// Exit code for CI: a crash/hang/timeout is always a failure. A file that is not a fuzzer-minimized
// test case ("clusterfuzz-*") must also parse with at least one page.
const expectedBroken = (f) => /^clusterfuzz-/i.test(f);
const failures = results.filter((r) =>
  r.fatal || (!expectedBroken(r.file) && (r.supported === false || r.svgError || r.jsonError || !(r.pages > 0))));
if (failures.length) {
  console.error(`\n${failures.length} file(s) failed: ${failures.map((r) => r.file).join(', ')}`);
  process.exit(1);
}
console.log(`\nall ${results.length} files ok (${results.filter((r) => expectedBroken(r.file)).length} broken-on-purpose rejected cleanly)`);
