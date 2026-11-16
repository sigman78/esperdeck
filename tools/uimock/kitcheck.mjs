// kitcheck.mjs — proves the kit.js port and gates the C kit. It rebuilds
// every frame of MO_GATE in the lab's screenshot pose. It compares cells
// and hit regions with the lab's dump. A cell is codepoint, bold and
// palette entry. It also rebuilds each dumped style table with theme.js.
// The sketch is the reference. The lab must draw every frame of
// MO_GATE, and no other mo-* frame.
//
//   node tools/uimock/kitcheck.mjs [data dir]

import { readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { drawPosed } from './kit.js';
import { MO_SKETCHES, MO_GATE } from './sketches-mo.js';
import { themeBuild, THEMES } from './theme.js';

const here = dirname(fileURLToPath(import.meta.url));
const data = process.argv[2] || join(here, 'data');

// Differences between a drawn screen and its lab frame, at most four.
function differences(ui, ref, lab) {
  const f = ui.frame, errs = [];
  for (let i = 0; i < lab.cols * lab.rows && errs.length < 3; i++) {
    const cp = ref.ov[i * 3], bold = ref.ov[i * 3 + 1] & 1, pal = ref.ov[i * 3 + 2];
    const same = f.cp[i] === cp && (!cp || (f.bold[i] === bold && f.pal[i] === pal));
    if (!same)
      errs.push(`cell ${i % lab.cols},${(i / lab.cols) | 0}: kit U+${f.cp[i].toString(16)}` +
                `/b${f.bold[i]}/p${f.pal[i]} lab U+${cp.toString(16)}/b${bold}/p${pal}`);
  }
  if (f.scrim !== ref.scrim) errs.push(`scrim ${f.scrim} vs ${ref.scrim}`);
  for (const [id, x, y, w, h] of ref.hot) {
    const k = ui.hots.filter((q) => q.id === id).pop();
    if (!k || k.x !== x || k.y !== y || k.w !== w || k.h !== h)
      errs.push(`region ${id}: kit ${k ? [k.x, k.y, k.w, k.h] : 'none'} lab ${[x, y, w, h]}`);
  }
  if (ui.hots.length !== ref.hot.length) errs.push(`regions ${ui.hots.length} vs ${ref.hot.length}`);
  return errs.slice(0, 4);
}

let total = 0, bad = 0;
const fail = (what, errs) => {
  bad++;
  console.log(`FAIL ${what}` + (errs.length ? '\n  ' + errs.join('\n  ') : ''));
};

for (const size of ['8x16', '10x20', '12x24']) {
  const labPath = join(data, `lab-${size}.json`);
  total++;
  if (!existsSync(labPath)) { fail(`${size}: no lab dump`, []); continue; }
  const lab = JSON.parse(readFileSync(labPath, 'utf8'));
  for (const [name, ref] of Object.entries(lab.themes)) {
    total++;
    const mine = themeBuild(ref.fg, ref.bg).table;
    const at = mine.findIndex((v, i) => v !== ref.table[i]);
    if (at >= 0 || mine.length !== ref.table.length)
      fail(`${size} theme ${name}`, [`entry ${at} is ${mine[at]}, lab has ${ref.table[at]}`]);
  }

  // The mo-* gate. A frame the lab does not draw fails, never skips.
  const slate = lab.themes.slate, want = THEMES.slate;
  total++;
  if (!slate || slate.fg !== want.fg || slate.bg !== want.bg)
    fail(`${size} theme slate`, [`lab has ${slate ? [slate.fg, slate.bg] : 'none'}, sketches use ${[want.fg, want.bg]}`]);
  for (const g of MO_GATE) {
    total++;
    const what = `${size} ${g.name}`;
    const ref = lab.screens.find((s) => s.name === g.name);
    if (!ref) { fail(what, ['the lab does not draw this frame']); continue; }
    const scr = MO_SKETCHES.find((s) => s.name === g.sketch);
    const pose = g.pose || {};
    const app = { state: g.state ? g.state() : {}, note: '', fg: want.fg, bg: want.bg,
                  go() {}, back() {}, home() {} };
    const ui = drawPosed({ ...scr, pose }, lab.cols, lab.rows, app);
    const errs = differences(ui, ref, lab);
    if (ref.theme !== 'slate') errs.unshift(`theme ${ref.theme}, not slate`);
    if ((ref.focus | 0) !== (pose.focus | 0) || (ref.down | 0) !== (pose.down | 0))
      errs.unshift(`pose focus ${ref.focus} down ${ref.down}, gate has ${pose.focus | 0} and ${pose.down | 0}`);
    for (const d of ui.issues.filter((q) => q.kind === 'drop')) errs.unshift(`no room for ${d.text}`);
    if (errs.length) fail(what, errs.slice(0, 5));
  }
  for (const s of lab.screens) {
    if (!s.name.startsWith('mo-') || MO_GATE.some((g) => g.name === s.name)) continue;
    total++;
    fail(`${size} ${s.name}`, ['the lab draws a frame that MO_GATE does not list']);
  }
}
console.log(`${total - bad}/${total} dumps, screens and themes match the lab`);
process.exit(bad || !total ? 1 : 0);
