// shot.mjs — render screens to PNG files and print their findings.
// Findings: clipped text, missing glyphs, parts with no room, small regions.
//
//   node shot.mjs [options] [name ...]
//
// With no names it renders every sketch. README.md lists the options.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateSync, crc32 } from 'node:zlib';
import { Font, render, toRGBA, PANELS, rectMM } from './model.js';
import { drawPosed } from './kit.js';
import { MO_SKETCHES, MO_GATE } from './sketches-mo.js';
import { THEMES } from './theme.js';

const here = dirname(fileURLToPath(import.meta.url));
const opt = { font: '12x24', panel: '800x480', theme: '', mono: '0',
              out: join(here, 'dist', 'shots'), regions: false, state: '{}', tag: '',
              focus: '', down: '' };
const names = [];
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '--regions') opt.regions = true;
  else if (a.startsWith('--')) opt[a.slice(2)] = process.argv[++i];
  else names.push(a);
}

function png(w, h, rgba) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0, o = 0; y < h; y++) {
    raw[o++] = 0;
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      raw[o++] = rgba[i]; raw[o++] = rgba[i + 1]; raw[o++] = rgba[i + 2];
    }
  }
  const chunk = (type, body) => {
    const len = Buffer.alloc(4), crc = Buffer.alloc(4);
    const td = Buffer.concat([Buffer.from(type), body]);
    len.writeUInt32BE(body.length);
    crc.writeUInt32BE(crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

const panel = PANELS[opt.panel];
const font = new Font(JSON.parse(readFileSync(join(here, 'data', `font-${opt.font}.json`), 'utf8')));
const lab = JSON.parse(readFileSync(join(here, 'data', `lab-${opt.font}.json`), 'utf8'));
const themes = { ...lab.themes, ...THEMES };
if (opt.theme && !themes[opt.theme]) { console.log(`no theme '${opt.theme}'`); process.exit(1); }
const cols = Math.floor(panel.width / font.w), rows = Math.floor(panel.height / font.h);
const all = MO_SKETCHES;
// A gate frame name draws its sketch with the gate's state and pose.
const gateFrame = (n) => {
  const g = MO_GATE.find((q) => q.name === n), base = g && all.find((s) => s.name === g.sketch);
  return base && { ...base, name: g.name, pose: g.pose, seed: g.state ? g.state() : null };
};
const list = names.length ? names.map((n) => gateFrame(n) || all.find((s) => s.name === n) || n) : MO_SKETCHES;
mkdirSync(opt.out, { recursive: true });

for (const scr of list) {
  if (typeof scr === 'string') { console.log(`no screen '${scr}'`); process.exitCode = 1; continue; }
  const theme = themes[opt.theme || scr.theme || 'default'];
  const app = { state: { ...scr.seed, ...JSON.parse(opt.state) }, note: '', fg: theme.fg, bg: theme.bg, go() {}, back() {}, home() {} };
  const posed = opt.focus || opt.down
    ? { ...scr, pose: { focus: +opt.focus || 0, down: +opt.down || 0 } } : scr;
  const ui = drawPosed(posed, cols, rows, app);
  const { px, missing } = render(ui.frame, font, theme.table,
    { width: panel.width, height: panel.height, mono: +opt.mono, strict: true });
  if (opt.regions)
    for (const h of ui.hots) {
      const x0 = h.x * font.w, y0 = h.y * font.h, x1 = x0 + h.w * font.w - 1, y1 = y0 + h.h * font.h - 1;
      for (let x = x0; x <= x1; x++) { px[y0 * panel.width + x] = 0xF81F; px[y1 * panel.width + x] = 0xF81F; }
      for (let y = y0; y <= y1; y++) { px[y * panel.width + x0] = 0xF81F; px[y * panel.width + x1] = 0xF81F; }
    }
  const file = join(opt.out, `${scr.name}-${opt.font}${opt.panel === '800x480' ? '' : '-' + opt.panel}${opt.tag ? '-' + opt.tag : ''}.png`);
  writeFileSync(file, png(panel.width, panel.height, toRGBA(px)));

  const notes = [];
  for (const m of missing) notes.push(`missing glyph U+${m.cp.toString(16)} at ${m.col},${m.row}`);
  for (const c of ui.issues) {
    if (c.kind === 'clip') notes.push(`clipped "${c.text}" to ${c.w} cells`);
    if (c.kind === 'drop') notes.push(`no room for "${c.text}"`);
  }
  let small = null;
  for (const h of ui.hots) {
    const [w, hh] = rectMM(panel, font, h.w, h.h), side = Math.min(w, hh);
    if (!small || side < small.side) small = { side, id: h.id, w, hh };
  }
  if (small && small.side < 7)
    notes.push(`region ${small.id} is ${small.w.toFixed(1)} x ${small.hh.toFixed(1)} mm (under 7 mm)`);
  console.log(`${file}  ${cols}x${rows}  ${ui.hots.length} regions` +
              (small ? `, smallest side ${small.side.toFixed(1)} mm` : ''));
  for (const n of [...new Set(notes)]) console.log(`  ${n}`);
}
