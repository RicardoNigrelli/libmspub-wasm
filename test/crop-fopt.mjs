#!/usr/bin/env node
// Picture crop (patch 0003) end to end, on bytes built according to [MS-ODRAW].
//
// None of the Apache POI files has a cropped picture, so this test makes one: it takes 51318.pub
// (one PNG on the page), finds the picture's OfficeArtFOPT record (recType 0xF00B, with the pib
// property 0x4104) and overwrites four simple properties that libmspub ignores (the wrap distances
// dxWrapDistLeft/Top/Right/Bottom, 0x0384-0x0387) with cropFromTop/Bottom/Left/Right
// (0x0100-0x0103), as signed 16.16 fixed point values. The record keeps its length, so the OLE
// container stays valid. Then it checks that:
//   - the unmodified file has no crop anywhere;
//   - the modified file brings fill.crop = {top, right, bottom, left} with the written values
//     (negative = padding, passed through as stored);
//   - nothing else in the JSON changes (apart from the raw libmspub:crop-* style properties).
//
//   node test/crop-fopt.mjs <dir with 51318.pub> [--variant Oz]     (exit code 1 on failure)
// SPDX-License-Identifier: MPL-2.0
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { loadLibmspub } from '../js/libmspub.mjs';

const args = process.argv.slice(2);
const dir = args.find((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--'))) || 'corpus';
const vi = args.indexOf('--variant');
const variant = vi >= 0 ? args[vi + 1] : 'Oz';

const failures = [];
const check = (cond, msg) => {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${msg}`);
  if (!cond) failures.push(msg);
};

const fixed = (x) => Math.round(x * 65536) | 0; // signed 16.16
const CROP = { top: 0.25, bottom: 0.125, left: 0.1, right: -0.05 };
const REPLACE = { 0x0384: [0x0100, CROP.top], 0x0385: [0x0101, CROP.bottom], 0x0386: [0x0102, CROP.left], 0x0387: [0x0103, CROP.right] };

/** Finds the FOPT records that carry a picture id (pib, 0x4104) by scanning the raw file. */
function findPictureFopts(b) {
  const out = [];
  for (let i = 0; i + 8 <= b.length; i++) {
    if (b[i + 2] !== 0x0b || b[i + 3] !== 0xf0) continue; // recType 0xF00B
    const verInst = b.readUInt16LE(i);
    if ((verInst & 0xf) !== 3) continue; // recVer 3
    const n = verInst >> 4;
    const len = b.readUInt32LE(i + 4);
    if (n === 0 || len < n * 6 || i + 8 + len > b.length) continue;
    const props = [];
    for (let k = 0; k < n; k++) props.push({ at: i + 8 + 6 * k, id: b.readUInt16LE(i + 8 + 6 * k), value: b.readUInt32LE(i + 10 + 6 * k) });
    if (props.some((p) => p.id === 0x4104)) out.push({ offset: i, props });
  }
  return out;
}

const original = readFileSync(join(dir, '51318.pub'));
const fopts = findPictureFopts(original);
check(fopts.length === 1, `51318.pub has one picture FOPT record (found ${fopts.length})`);
const modified = Buffer.from(original);
let replaced = 0;
for (const p of fopts[0]?.props ?? []) {
  const r = REPLACE[p.id];
  if (!r) continue;
  modified.writeUInt16LE(r[0], p.at);
  modified.writeInt32LE(fixed(r[1]), p.at + 2);
  replaced++;
}
check(replaced === 4, `4 ignored properties (0x0384-0x0387) replaced by cropFromTop/Bottom/Left/Right (got ${replaced})`);

const pub = await loadLibmspub({ variant });
const bitmapFills = (doc) => {
  const out = [];
  const walk = (els) => {
    for (const e of els || []) {
      if (e.fill && e.fill.kind === 'bitmap') out.push(e.fill);
      if (e.elements) walk(e.elements);
    }
  };
  for (const p of doc.pages) walk(p.elements);
  return out;
};

const before = pub.parse(original);
const after = pub.parse(modified);
const fillsBefore = bitmapFills(before.doc);
const fillsAfter = bitmapFills(after.doc);
check(fillsBefore.length === 1 && !('crop' in fillsBefore[0]), 'unmodified file: one picture, no crop');
check(fillsAfter.length === 1, 'modified file: still one picture');
const crop = fillsAfter[0]?.crop;
console.log('     fill.crop =', JSON.stringify(crop));
const near = (a, b) => typeof a === 'number' && Math.abs(a - b) < 1e-4;
check(!!crop && near(crop.top, CROP.top) && near(crop.bottom, CROP.bottom) && near(crop.left, CROP.left) && near(crop.right, CROP.right),
  `fill.crop = ${JSON.stringify(CROP)} (fractions, negative = padding)`);
check(after.blobs.length === 1 && Buffer.compare(after.blobs[0], before.blobs[0]) === 0, 'the picture bytes are the same');

// Everything else is identical: drop the crop and the raw crop props and compare.
const strip = (doc) =>
  JSON.stringify(doc, (k, v) => (k === 'crop' || k.startsWith('libmspub:crop-') || k === 'jsonBytes' ? undefined : v));
check(strip(before.doc) === strip(after.doc), 'the rest of the JSON is identical');

if (failures.length) {
  console.error(`\n${failures.length} check(s) failed`);
  process.exit(1);
}
console.log('\ncrop test ok');
