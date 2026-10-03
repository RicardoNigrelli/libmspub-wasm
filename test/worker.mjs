// Processes ONE .pub file inside a worker thread, so that a wasm trap or an
// infinite loop in libmspub cannot take down the corpus runner.
// SPDX-License-Identifier: MPL-2.0
import { parentPort, workerData } from 'node:worker_threads';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { loadLibmspub, LibmspubCrash } from '../js/libmspub.mjs';

const { file, variant, outDir, repeats } = workerData;

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
}

function summarize(doc) {
  const counts = {};
  let textChars = 0, textBoxes = 0, images = 0, imageBytes = 0, tables = 0;
  const imageTypes = {};
  const fonts = new Set(), colors = new Set();
  const boxTexts = [];
  const walkParas = (paras) => {
    let t = '';
    for (const p of paras || []) {
      for (const s of p.spans || []) {
        t += s.text;
        if (s.font) fonts.add(s.font);
        if (s.color) colors.add(s.color);
      }
      t += '\n';
    }
    return t;
  };
  const walk = (els) => {
    for (const e of els || []) {
      counts[e.type] = (counts[e.type] || 0) + 1;
      if (e.type === 'text') {
        textBoxes++;
        const t = walkParas(e.paragraphs);
        textChars += t.replace(/\s/g, '').length;
        boxTexts.push(t.trim());
      } else if (e.type === 'image') {
        images++;
        imageBytes += e.data ? Math.floor(e.data.length * 3 / 4) : 0;
        imageTypes[e.mimeType] = (imageTypes[e.mimeType] || 0) + 1;
      }
      if (e.fill && e.fill.kind === 'bitmap') {
        // libmspub emits pictures as shapes with a bitmap fill
        images++;
        imageBytes += e.fill.data ? Math.floor(e.fill.data.length * 3 / 4) : 0;
        imageTypes[e.fill.mimeType] = (imageTypes[e.fill.mimeType] || 0) + 1;
      } else if (e.type === 'table') {
        tables++;
        for (const r of e.rows || []) for (const c of r.cells || []) textChars += walkParas(c.paragraphs).replace(/\s/g, '').length;
      }
      if (e.elements) walk(e.elements);
    }
  };
  for (const p of doc.pages) walk(p.elements);
  // identical non-trivial text in several boxes = a story repeated (linked frames?)
  const seen = new Map();
  for (const t of boxTexts) if (t.length > 20) seen.set(t, (seen.get(t) || 0) + 1);
  const duplicatedStories = [...seen.values()].filter((n) => n > 1).length;
  return {
    counts, textBoxes, textChars, images, imageBytes, imageTypes, tables,
    fonts: [...fonts], colors: [...colors], duplicatedStories,
    pageSizes: doc.pages.map((p) => `${p.width}x${p.height}`),
  };
}

const result = { file: basename(file), bytes: 0 };
try {
  const bytes = readFileSync(file);
  result.bytes = bytes.length;
  const tLoad = performance.now();
  const pub = await loadLibmspub({ variant });
  result.instantiateMs = performance.now() - tLoad;

  let t = performance.now();
  result.supported = pub.isSupported(bytes);
  result.isSupportedMs = performance.now() - t;

  if (result.supported) {
    // SVG
    try {
      const times = [];
      let pages;
      for (let i = 0; i < repeats; i++) {
        t = performance.now();
        pages = pub.toSVG(bytes);
        times.push(performance.now() - t);
      }
      result.svgMs = median(times);
      result.svgPages = pages.length;
      result.svgBytes = pages.reduce((n, s) => n + s.length, 0);
      if (outDir) {
        const d = join(outDir, basename(file));
        mkdirSync(d, { recursive: true });
        pages.forEach((s, i) => writeFileSync(join(d, `page-${i + 1}.svg`), s));
      }
    } catch (e) {
      result.svgError = `${e.name}: ${e.message}`;
      if (e instanceof LibmspubCrash) await pub.ensure();
    }
    // JSON
    try {
      const times = [];
      let doc;
      for (let i = 0; i < repeats; i++) {
        t = performance.now();
        doc = pub.toJSON(bytes);
        times.push(performance.now() - t);
      }
      result.jsonMs = median(times);
      result.pages = doc.pages.length;
      result.jsonBytes = doc.jsonBytes;
      result.parseOk = doc.parseOk;
      if (doc.error) result.jsonError = doc.error;
      result.summary = summarize(doc);
      result.calls = doc.stats.calls;
      result.metadata = doc.metadata;
      if (outDir) {
        const d = join(outDir, basename(file));
        mkdirSync(d, { recursive: true });
        writeFileSync(join(d, 'document.json'), JSON.stringify(doc, null, 1));
      }
    } catch (e) {
      result.jsonError = `${e.name}: ${e.message}`;
    }
  }
} catch (e) {
  result.fatal = `${e.name}: ${e.message}`;
}
parentPort.postMessage(result);
