// check.mjs — the parity gate. Renders every frame of every lab dump
// through model.js and compares it with the lab's own screenshot. Any
// differing pixel fails: a design judged on the model is only worth
// something while this passes.
//
//   node tools/uimock/check.mjs [data dir]

import { readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Font, Frame, render } from './model.js';

const here = dirname(fileURLToPath(import.meta.url));
const data = process.argv[2] || join(here, 'data');

function readBmp565(path) {
  const b = readFileSync(path);
  const off = b.readUInt32LE(10), W = b.readInt32LE(18), H = b.readInt32LE(22);
  const stride = (W * 3 + 3) & ~3;
  const px = new Uint16Array(W * H);
  for (let y = 0; y < H; y++) {
    let p = off + (H - 1 - y) * stride;
    for (let x = 0; x < W; x++, p += 3)
      px[y * W + x] = ((b[p + 2] >> 3) << 11) | ((b[p + 1] >> 2) << 5) | (b[p] >> 3);
  }
  return { W, H, px };
}

let frames = 0, bad = 0;
for (const size of ['8x16', '10x20', '12x24']) {
  const labPath = join(data, `lab-${size}.json`);
  if (!existsSync(labPath)) { console.log(`${size}: no dump, skipped`); continue; }
  const lab = JSON.parse(readFileSync(labPath, 'utf8'));
  const font = new Font(JSON.parse(readFileSync(join(data, `font-${size}.json`), 'utf8')));
  for (const scr of lab.screens) {
    // A screen names its theme. A screen without one uses the default.
    const table = lab.themes[scr.theme || 'default'].table;
    const ref = readBmp565(join(data, `ref-${size}`, `${scr.name}.bmp`));
    const { px } = render(Frame.fromDump(lab, scr), font, table,
                          { width: ref.W, height: ref.H });
    let diff = 0, first = -1;
    for (let i = 0; i < px.length; i++)
      if (px[i] !== ref.px[i]) { diff++; if (first < 0) first = i; }
    frames++;
    if (diff) {
      bad++;
      const x = first % ref.W, y = (first / ref.W) | 0;
      console.log(`FAIL ${size} ${scr.name}: ${diff} px differ, first at ${x},${y} ` +
                  `(cell ${(x / font.w) | 0},${(y / font.h) | 0}) ` +
                  `model ${px[first].toString(16)} lab ${ref.px[first].toString(16)}`);
    }
  }
}
console.log(`${frames - bad}/${frames} frames identical`);
process.exit(bad || !frames ? 1 : 0);
