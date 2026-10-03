#!/usr/bin/env node
// Memory and size of reading ONE .pub through js/libmspub.mjs, in a fresh process:
// peak RSS of the process, size of the wasm linear memory after the parse (it only grows, so it
// is the peak of the wasm heap), JSON size, picture bytes and times.
//
//   node --expose-gc test/measure-memory.mjs <file.pub> [--variant Oz] [--api parse|toJSON]
//
// --api parse  (default): pub.parse() -> { doc, blobs }, pictures as binary blobs (JSON v2).
// --api toJSON: the v1-compatible shim (pictures inlined as base64), for comparison.
// Run it once per process: the peak RSS (process.resourceUsage().maxRSS) cannot be reset.
// SPDX-License-Identifier: MPL-2.0
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadLibmspub } from '../js/libmspub.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : def;
};
const file = args.find((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));
if (!file) {
  console.error('usage: node --expose-gc test/measure-memory.mjs <file.pub> [--variant Oz] [--api parse|toJSON]');
  process.exit(2);
}
const variant = opt('variant', 'Oz');
const MB = 1024 * 1024;
const mb = (n) => +(n / MB).toFixed(1);
const mem = () => {
  const m = process.memoryUsage();
  return { rss: mb(m.rss), heapUsed: mb(m.heapUsed), external: mb(m.external), arrayBuffers: mb(m.arrayBuffers) };
};

// Keep a handle on the wasm memory to read its size (instantiateWasm is a standard emscripten hook).
let wasmMemory = null;
const wasmBinary = readFileSync(join(here, '..', 'dist', variant, 'libmspub.wasm'));
const instantiateWasm = (imports, receive) => {
  WebAssembly.instantiate(wasmBinary, imports).then(({ instance }) => {
    wasmMemory = Object.values(instance.exports).find((v) => v instanceof WebAssembly.Memory) ?? null;
    receive(instance);
  });
  return {};
};

const before = mem();
const pub = await loadLibmspub({ variant, moduleOptions: { instantiateWasm } });
const api = opt('api', typeof pub.parse === 'function' ? 'parse' : 'toJSON');
const bytes = readFileSync(file);
const initialWasm = wasmMemory ? wasmMemory.buffer.byteLength : 0;

const t0 = performance.now();
let doc;
let blobBytes = 0;
let blobCount = 0;
if (api === 'parse') {
  const r = pub.parse(bytes);
  doc = r.doc;
  blobCount = r.blobs.length;
  blobBytes = r.blobs.reduce((n, b) => n + b.byteLength, 0);
} else {
  doc = pub.toJSON(bytes);
}
const parseMs = performance.now() - t0;
const wasmAfter = wasmMemory ? wasmMemory.buffer.byteLength : 0;
const peakRss = process.resourceUsage().maxRSS / 1024;
globalThis.gc?.();

console.log(JSON.stringify({
  api,
  variant,
  fileMB: mb(bytes.length),
  pages: doc.pages.length,
  version: doc.version,
  jsonBytes: doc.jsonBytes,
  blobs: blobCount,
  blobMB: mb(blobBytes),
  parseMs: Math.round(parseMs),
  wasmMemoryMB: { initial: mb(initialWasm), afterParse: mb(wasmAfter) },
  peakRssMB: Math.round(peakRss),
  memoryMB: { before, after: mem() },
}));
