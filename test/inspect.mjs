#!/usr/bin/env node
// Inspects one document.json produced by run-corpus.mjs and prints what the
// README "qué se pierde" section is based on: text boxes and their text,
// repeated stories, elements repeated on every page, fills, colours, images.
//   node test/inspect.mjs out/Oz/SampleNewsletter.pub/document.json [--text]
// SPDX-License-Identifier: MPL-2.0
import { readFileSync } from 'node:fs';

const file = process.argv[2];
const showText = process.argv.includes('--text');
const doc = JSON.parse(readFileSync(file, 'utf8'));

const fills = {}, strokes = {}, colors = {}, fonts = {}, mime = {};
const boxes = [];
const pageSigs = [];
const count = (o, k) => { o[k] = (o[k] || 0) + 1; };

function textOf(paras) {
  return (paras || []).map((p) => (p.spans || []).map((s) => s.text).join('')).join('\n');
}

function walk(els, page, depth, sigs) {
  for (const e of els || []) {
    // leaf elements only: layers/groups carry no geometry of their own
    if (!e.elements) {
      const extra = e.type === 'text' ? textOf(e.paragraphs).slice(0, 40) : '';
      sigs.push(`${e.type}:${JSON.stringify(e.geometry || [e.x, e.y, e.width, e.height])}${extra}`);
    }
    if (e.fill) {
      count(fills, e.fill.kind);
      if (e.fill.color) count(colors, e.fill.color);
      if (e.fill.kind === 'bitmap') count(mime, e.fill.mimeType || '?');
    }
    if (e.stroke) {
      count(strokes, e.stroke.kind);
      if (e.stroke.color) count(colors, e.stroke.color);
    }
    if (e.type === 'image') count(mime, e.mimeType);
    if (e.type === 'text') {
      for (const p of e.paragraphs || []) for (const s of p.spans || []) {
        if (s.font) count(fonts, `${s.font} ${s.size}pt${s.bold ? ' B' : ''}${s.italic ? ' I' : ''}`);
        if (s.color) count(colors, s.color);
      }
      boxes.push({ page, x: e.x, y: e.y, w: e.width, h: e.height, cols: e.columns, text: textOf(e.paragraphs) });
    }
    if (e.elements) walk(e.elements, page, depth + 1, sigs);
  }
}

doc.pages.forEach((p, i) => {
  const sigs = [];
  walk(p.elements, i, 0, sigs);
  pageSigs.push(sigs);
  console.log(`page ${i}: ${p.width} x ${p.height} pt, ${p.elements.length} top-level elements`);
});

// elements present (same geometry) on every page => master page content flattened
if (pageSigs.length > 1) {
  const common = pageSigs[0].filter((s) => pageSigs.every((ps) => ps.includes(s)));
  console.log(`\nelements with identical geometry on ALL ${pageSigs.length} pages: ${common.length}`);
  common.slice(0, 10).forEach((s) => console.log('   ', s.slice(0, 140)));
}

console.log('\ncalls:', JSON.stringify(doc.stats.calls));
console.log('fill kinds:', JSON.stringify(fills));
console.log('stroke kinds:', JSON.stringify(strokes));
console.log('image mime types:', JSON.stringify(mime));
console.log('colours:', JSON.stringify(colors));
console.log('fonts:', JSON.stringify(fonts));
console.log('embedded fonts:', doc.embeddedFonts.length, 'metadata:', JSON.stringify(doc.metadata));

const byText = new Map();
for (const b of boxes) {
  const k = b.text.trim();
  if (k.length < 30) continue;
  if (!byText.has(k)) byText.set(k, []);
  byText.get(k).push(b);
}
const dups = [...byText.entries()].filter(([, v]) => v.length > 1);
console.log(`\ntext boxes: ${boxes.length}; stories repeated in more than one box: ${dups.length}`);
for (const [t, v] of dups) {
  console.log(`  x${v.length} (${t.length} chars): "${t.slice(0, 70).replace(/\n/g, ' / ')}…"`);
  for (const b of v) console.log(`      page ${b.page} at (${b.x?.toFixed(0)}, ${b.y?.toFixed(0)}) ${b.w?.toFixed(0)}x${b.h?.toFixed(0)}`);
}
if (showText) {
  console.log('\n--- text boxes ---');
  for (const b of boxes) console.log(`[p${b.page} (${b.x?.toFixed(0)},${b.y?.toFixed(0)}) ${b.w?.toFixed(0)}x${b.h?.toFixed(0)} cols=${b.cols}] ${b.text.slice(0, 160).replace(/\n/g, ' / ')}`);
}
