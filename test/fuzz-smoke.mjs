#!/usr/bin/env node
// Quick, deterministic robustness check (NOT a real fuzzer): mutates corpus
// files (random byte flips and truncations) and feeds them to toJSON().
// Counts clean rejections, partial parses, wasm traps and hangs.
//   node test/fuzz-smoke.mjs [--n 200] [--variant Oz] [--seed 1]
// SPDX-License-Identifier: MPL-2.0
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import { readFileSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const SEEDS = ['Sample.pub', 'Sample98.pub', 'SampleBrochure.pub', 'SampleNewsletter.pub', '60685.pub'];

function rng(seed) { // mulberry32
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function mutant(buf, r) {
  const b = new Uint8Array(buf);
  if (r() < 0.2) return b.slice(0, Math.floor(r() * b.length)); // truncation
  const flips = 1 + Math.floor(r() * 64);
  // skip the first 512 bytes (OLE header) most of the time, so the file is
  // still recognised and the mutation reaches libmspub's own parsers
  for (let i = 0; i < flips; i++) {
    const pos = r() < 0.9 ? 512 + Math.floor(r() * (b.length - 512)) : Math.floor(r() * b.length);
    b[pos] = Math.floor(r() * 256);
  }
  return b;
}

if (isMainThread) {
  const args = process.argv.slice(2);
  const opt = (k, d) => (args.includes(`--${k}`) ? args[args.indexOf(`--${k}`) + 1] : d);
  const n = Number(opt('n', 200));
  const variant = opt('variant', 'Oz');
  const seed = Number(opt('seed', 1));
  const corpus = process.argv[2] ? resolve(process.argv[2]) : join(here, '..', 'corpus');
  const tally = { unsupported: 0, ok: 0, partial: 0, trap: 0, hang: 0 };
  const traps = [];
  let next = 0;
  const startAll = performance.now();
  while (next < n) {
    // one worker per batch; if it hangs we know which mutant (last reported)
    await new Promise((resolve) => {
      const w = new Worker(fileURLToPath(import.meta.url), { workerData: { start: next, n, seed, variant, corpus } });
      let current = next;
      let timer = setTimeout(onHang, 10000);
      function onHang() {
        tally.hang++;
        traps.push({ i: current, kind: 'hang (>10 s)' });
        next = current + 1;
        w.terminate();
      }
      w.on('message', (m) => {
        clearTimeout(timer);
        if (m.type === 'start') { current = m.i; timer = setTimeout(onHang, 10000); return; }
        tally[m.result]++;
        if (m.result === 'trap') traps.push({ i: m.i, kind: m.error, seedFile: m.seedFile });
        next = m.i + 1;
        if (next < n) timer = setTimeout(onHang, 10000);
      });
      w.on('exit', () => { clearTimeout(timer); resolve(); });
      w.on('error', (e) => { traps.push({ i: current, kind: `worker error ${e.message}` }); next = current + 1; });
    });
  }
  console.log(`${n} mutants (seed ${seed}, build ${variant}) in ${((performance.now() - startAll) / 1000).toFixed(1)} s`);
  console.log(tally);
  for (const t of traps) console.log('  ', t);
  // --dump <dir>: write the problematic mutants to disk to reproduce them
  const dump = opt('dump', null);
  if (dump && traps.length) {
    const { mkdirSync, writeFileSync } = await import('node:fs');
    mkdirSync(dump, { recursive: true });
    const seeds = SEEDS.map((f) => [f, readFileSync(join(corpus, f))]);
    const r = rng(seed);
    const wanted = new Set(traps.map((t) => t.i));
    for (let i = 0; i < n; i++) {
      const [name, buf] = seeds[Math.floor(r() * seeds.length)];
      const m = mutant(buf, r);
      if (wanted.has(i)) writeFileSync(join(dump, `mutant-${seed}-${i}-${name}`), m);
    }
    console.log(`dumped ${wanted.size} mutants to ${dump}`);
  }
} else {
  const { loadLibmspub, LibmspubCrash } = await import('../js/libmspub.mjs');
  const { start, n, seed, variant, corpus } = workerData;
  const seeds = SEEDS.map((f) => [f, readFileSync(join(corpus, f))]);
  const r = rng(seed);
  // replay the PRNG up to `start` so mutants are identical across batches
  const all = [];
  for (let i = 0; i < n; i++) {
    const [name, buf] = seeds[Math.floor(r() * seeds.length)];
    const m = mutant(buf, r);
    if (i >= start) all.push([i, name, m]);
  }
  const pub = await loadLibmspub({ variant });
  for (const [i, name, m] of all) {
    parentPort.postMessage({ type: 'start', i });
    await pub.ensure();
    let result;
    let error;
    try {
      if (!pub.isSupported(m)) result = 'unsupported';
      else {
        try {
          const doc = pub.toJSON(m);
          result = doc.parseOk ? 'ok' : 'partial';
        } catch (e) {
          if (e instanceof LibmspubCrash) throw e;
          result = 'partial';
        }
      }
    } catch (e) {
      result = 'trap';
      error = e.message;
    }
    parentPort.postMessage({ type: 'done', i, result, error, seedFile: name });
  }
  process.exit(0);
}
