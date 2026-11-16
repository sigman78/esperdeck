// sketches-mo.js — round 2: modern tiles. No frames. A part is a plain
// slab with half-cell top and bottom edges on a slate screen. Tone
// shows depth. Colour is an accent, not a wall.
//
// Rules the helpers enforce:
//   - A label sits on the centre row of its part. An off-centre label
//     is a draw error.
//   - A helper reports a part that does not fit. It never drops one in silence.
//   - Chosen and focused are different looks.
//   - An edged part sits on the screen background, never on a panel.

import {
  S, C, G, BOLD, LEFT, CENTER, RIGHT, WIDE, COMPACT, ID_BACK,
  rect, cutTop, cutBottom, cutLeft, cutRight, inset, center,
  grid, gridCell, gridCapacity, pageCount, pageClamp, spinnerGlyph, profAccent,
  statusBar, toast, SB, NOTE,
  entryFirst, codePlan, qrW, qrH, qrRows, qrGlyph, QR_ZONE, fakeQr, wrapLines, stepLines,
} from './kit.js';
import { PROFILES, PROFILES32, TERM_LINES, WIFI_UP, WIFI_DOWN, WIFI_FULL } from './fixtures.js';

const MO_ID = {
  HOST: 2000, PILL: 2100, PREV: 2200, NEXT: 2201, HUB: 2300, OPT: 2400,
  BTN: 2600, KEY: 2700, FIELD: 2800, ROW: 2900, MORE: 3000,
};
const MO_TERM_FG = 0x57EA;
const moLen = (s) => Array.from(String(s)).length;
// Key focus is one colour on every part. No other state uses it.
const MO_FOCUS = C.AMBER;
// The hue of a label beside its value.
const MO_LABEL = C.CYAN;

// Text with its units dimmed: a word right after a number is a unit.
// A number is digits with at most one `.` or `:` part, so an address is not one.
// `value` and `unit` are styles. On a one-colour surface pass the same.
function moUnits(ui, r, text, value, unit, align = LEFT) {
  ui.text(r, text, value, align);
  if (value === unit) return;
  const cs = Array.from(String(text)), n = Math.min(cs.length, r.w);
  const x0 = r.x + (align === CENTER ? (r.w - n) >> 1 : align === RIGHT ? r.w - n : 0);
  const last = cs.length > r.w ? n - 1 : n;
  let i = 0, afterNumber = false;
  while (i < cs.length) {
    if (cs[i] === ' ') { i++; continue; }
    let j = i;
    while (j < cs.length && cs[j] !== ' ') j++;
    const word = cs.slice(i, j).join(''), number = /^[-+]?\d+(?:[.:]\d+)?$/.test(word);
    if (!number && afterNumber)
      for (let c = i; c < j && c < last; c++) ui.putch(x0 + c, r.y, cs[c], unit);
    afterNumber = number;
    i = j;
  }
}

// Paint the whole screen in the theme background.
function moScreen(ui) {
  ui.pen(C.DEFAULT);
  ui.fill(rect(0, 0, ui.cols, ui.rows), ' ', S.TEXT);
}

// Styles of one part in its present state. o:
//   - tone: 'card' neutral panel, 'tint' dark accent, 'solid' bright accent.
//   - accent, off (disabled), lit (held in the pressed look).
//   - pose: 'focus' | 'down', for a states sheet.
function moLook(ui, id, o) {
  const pose = o.pose || '';
  const focus = pose === 'focus' || (!pose && !!id && ui.focused(id));
  const down = pose === 'down' || o.lit || (!pose && !!id && ui.pressed(id));
  const a = o.accent | 0;
  if (o.off)
    return { pen: C.DEFAULT, fill: S.PANEL, edge: S.PANEL_EDGE, title: S.PANEL_MUTED,
             detail: S.PANEL_MUTED, card: true, off: true };
  if (focus || down)
    return { pen: focus ? MO_FOCUS : a, fill: S.FOCUS, edge: S.FOCUS_EDGE, title: S.FOCUS | BOLD,
             detail: S.FOCUS, hot: true, focus };
  if (o.tone === 'solid')
    return { pen: a, fill: S.BAR, edge: S.BAR_EDGE, title: S.BAR | BOLD, detail: S.BAR };
  if (o.tone === 'tint')
    return { pen: a, fill: S.WELL, edge: S.WELL_EDGE, title: S.WELL | BOLD, detail: S.WELL };
  return { pen: C.DEFAULT, fill: S.PANEL, edge: S.PANEL_EDGE,
           title: S.PANEL | (o.plain ? 0 : BOLD), detail: S.PANEL_MUTED, card: true };
}

// A slab: half-cell top and bottom edges, square corners. Needs 3 rows.
function moSlab(ui, r, k) {
  const x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  ui.pen(k.pen);
  for (let x = r.x; x <= x1; x++) {
    ui.putch(x, r.y, G.HALF_DN, k.edge);
    ui.putch(x, y1, G.HALF_UP, k.edge);
  }
  ui.fill(rect(r.x, r.y + 1, r.w, r.h - 2), ' ', k.fill);
}

// A colour stripe: the first column of a slab, at the slab's full height.
function moStripe(ui, r, accent) {
  ui.pen(accent);
  ui.putch(r.x, r.y, G.HALF_DN, S.BAR_EDGE);
  for (let y = r.y + 1; y < r.y + r.h - 1; y++) ui.putch(r.x, y, ' ', S.BAR);
  ui.putch(r.x, r.y + r.h - 1, G.HALF_UP, S.BAR_EDGE);
  ui.pen(C.DEFAULT);
}

// Key-focus bars: a half-cell sand bar and a dark slit at each end.
// A striped part keeps its stripe as the left bar; the slit moves in one.
function moBarCol(ui, x, r, top, mid, bot, k, ms = k.fill) {
  ui.putch(x, r.y, top, k.edge);
  for (let y = r.y + 1; y < r.y + r.h - 1; y++) ui.putch(x, y, mid, ms);
  ui.putch(x, r.y + r.h - 1, bot, k.edge);
}
function moFocusBars(ui, r, k, striped) {
  if (!k.focus) return;
  ui.pen(k.pen);
  if (striped) moBarCol(ui, r.x + 1, r, G.QUAD_LR, G.LHALF, G.QUAD_UR, k);
  else moBarCol(ui, r.x, r, G.QUAD_LL, G.RHALF, G.QUAD_UL, k);
  moBarCol(ui, r.x + r.w - 1, r, G.QUAD_LR, G.LHALF, G.QUAD_UR, k);
}

// Top row of `lines` text rows centred in `h` rows.
function moCentre(y, h, lines, what) {
  if ((h - lines) & 1) throw new Error(`${what}: ${lines} line(s) cannot centre in ${h} rows`);
  return y + ((h - lines) >> 1);
}

// The one tappable part. o adds to moLook's fields:
//   - title, detail (second line), align.
//   - right: text at the right end of the title row; rightBold.
//   - units: the right-hand text is a number with a unit; dim the unit.
//   - reserve: cells kept free at the right end for a control.
//   - pad: side padding in cells, else 2 for a wide part and 1 for a narrow one.
//   - stripe: accent of a colour stripe at the left edge (card tone).
//   - detailPen: colour of a state mark in the detail, bold, on a plain card only.
// Returns { k, y, inn } for a caller that draws a control, or null.
function moPart(ui, r, id, o, act) {
  if (r.w < 4 || r.h < 3) { ui.issues.push({ kind: 'drop', text: o.title || `#${id}` }); return null; }
  if (!o.off) ui.hot(r, id, act);
  const k = moLook(ui, id, o);
  moSlab(ui, r, k);
  if (!k.off && o.stripe) moStripe(ui, r, o.stripe);
  moFocusBars(ui, r, k, !k.off && !!o.stripe);
  ui.pen(k.pen);
  const pad = o.pad || (r.w >= 10 ? 2 : 1);
  const inn = rect(r.x + pad, r.y + 1, r.w - 2 * pad, r.h - 2);
  const y = moCentre(inn.y, inn.h, o.detail ? 2 : 1, o.title);
  let w = inn.w - (o.reserve | 0);
  if (o.right) {
    const n = moLen(o.right);
    if (o.units) moUnits(ui, rect(inn.x + w - n, y, n, 1), o.right, k.title & ~BOLD, k.detail, RIGHT);
    else ui.text(rect(inn.x + w - n, y, n, 1), o.right, o.rightBold ? k.title : k.detail, RIGHT);
    w -= n + 1;
  }
  ui.text(rect(inn.x, y, w, 1), o.title, k.title, o.align || LEFT);
  if (o.detail) {
    const mark = o.detailPen && k.card && !k.off;
    if (mark) ui.pen(o.detailPen);
    ui.text(rect(inn.x, y + 1, inn.w - (o.reserve | 0), 1), o.detail, mark ? k.title : k.detail, o.align || LEFT);
  }
  ui.pen(C.DEFAULT);
  return { k, y, inn };
}

// A button: one centred label. `tone` and `accent` pick its weight.
function moBtn(ui, r, id, label, o, act) {
  return moPart(ui, r, id, { ...o, title: label, align: CENTER }, act);
}

// Own widths. A button is as wide as its label needs, never as wide as
// its row. Ten cells is the narrowest part a finger hits at the small font.
const moBtnW = (label) => Math.max(10, moLen(label) + 6);
const moActW = (title, right) => Math.max(10, moLen(title) + (right ? moLen(right) + 2 : 0) + 6);
const moToggleW = (label) => moLen(label) + 17;

// A flow packs parts from the left in rows of `h`, `gap` cells apart.
// A part with no room left starts the next row.
function moFlow(area, gap, h) {
  return { x0: area.x, w: area.w, gap, h, x: area.x, y: area.y };
}
function moFlowNext(f, w) {
  if (f.x > f.x0 && f.x + w > f.x0 + f.w) moFlowBreak(f);
  const r = rect(f.x, f.y, Math.min(w, f.w), f.h);
  f.x += w + f.gap;
  return r;
}
function moFlowBreak(f) {
  if (f.x > f.x0) { f.x = f.x0; f.y += f.h; }
}

// A switch tile. The state shows three ways: the word, the side of the
// diamond knob on its line, the colour.
function moToggle(ui, r, id, label, on, o, act) {
  const p = moPart(ui, r, id, { ...o, title: label, reserve: 9 }, act);
  if (!p) return;
  const x = p.inn.x + p.inn.w - 4, k = p.k, live = on && !k.off;
  ui.pen(live && k.card ? C.GREEN : k.pen);
  ui.text(rect(x - 4, p.y, 3, 1), on ? 'On' : 'Off', live ? k.title : k.detail, RIGHT);
  // The knob is two cells: a left and a right triangle make one diamond.
  const kx = on ? x + 2 : x, lx = on ? x : x + 2;
  ui.puts(lx, p.y, '━━', k.detail);
  ui.putch(kx, p.y, G.POINT_L, live ? k.title : k.detail);
  ui.putch(kx + 1, p.y, G.ARROW, live ? k.title : k.detail);
  ui.pen(C.DEFAULT);
}

// The caption column of a choice: the caption, two cells each side.
const moCapW = (caption) => moLen(caption) + 4;

// One choice of several: a caption, then a chip per option. The chosen
// chip has a filled face and the check mark. Focus is the light look.
// The chips follow the caption column. o.capW sets that column's width,
// so the chips of several choices line up.
function moChips(ui, r, idBase, caption, labels, value, pick, o = {}) {
  // Seven cells is the narrowest chip a finger hits at the small font.
  const cw = (l) => Math.max(7, moLen(l) + 4);
  const need = labels.reduce((n, l) => n + cw(l) + 1, 0);
  const capW = o.capW || moCapW(caption);
  const y = moCentre(r.y, r.h, 1, caption);
  ui.pen(C.DEFAULT);
  ui.text(rect(r.x + 2, y, capW - 4, 1), caption, S.TEXT, LEFT);
  if (capW + need > r.w) { ui.issues.push({ kind: 'drop', text: `${caption} chips` }); return; }
  let x = r.x + capW;
  labels.forEach((l, i) => {
    const w = cw(l), chosen = i === value;
    const po = {
      title: chosen ? `✓ ${l}` : l, align: CENTER, plain: !chosen, pad: 1,
      tone: chosen ? 'solid' : 'card', accent: chosen ? (o.accent || C.BLUE) : 0,
      pose: o.poses ? o.poses[i] : '', off: o.off,
    };
    moPart(ui, rect(x, r.y, w, r.h), idBase + i, po, { tap: () => pick(i) });
    if (chosen && !o.off) {
      // The gap cells either side show half blocks: the chip sticks out.
      const k = moLook(ui, idBase + i, po);
      ui.pen(k.pen);
      if (x > r.x) moBarCol(ui, x - 1, r, G.QUAD_LR, G.RHALF, G.QUAD_UR, k, k.edge);
      moBarCol(ui, x + w, r, G.QUAD_LL, G.LHALF, G.QUAD_UL, k, k.edge);
      ui.pen(C.DEFAULT);
    }
    x += w + 1;
  });
}

// `< 2/3 >` cut from the right end of a header rect. Returns the rest.
function moPager(ui, header, page, pages, acts, o = {}) {
  if (pages <= 1) return header;
  const r = cutRight(header, 19), y = moCentre(r.y, r.h, 1, 'pager');
  const prev = cutLeft(r, 6), next = cutRight(r, 6);
  moBtn(ui, prev, MO_ID.PREV, '◄', { off: page <= 0, pose: o.posePrev }, acts.prev);
  moBtn(ui, next, MO_ID.NEXT, '►', { off: page >= pages - 1, pose: o.poseNext }, acts.next);
  ui.pen(C.DEFAULT);
  ui.text(rect(r.x, y, r.w, 1), `${page + 1}/${pages}`, S.TEXT | BOLD, CENTER);
  cutRight(header, 1);
  return header;
}

// Position rail at the side of a paged list: a thumb on a faint track.
function moRail(ui, r, page, pages) {
  if (pages <= 1 || r.h < 2) return;
  const thumb = Math.max(1, Math.round(r.h / pages));
  const at = Math.round((r.h - thumb) * page / (pages - 1));
  ui.pen(C.DEFAULT);
  for (let i = 0; i < r.h; i++)
    ui.putch(r.x, r.y + i, G.RHALF, i >= at && i < at + thumb ? S.MUTED : S.PANEL_EDGE);
}


// The chips of the real bar, as app_statusbar.c polls them. One cell
// each; a lit lock sits inside a plain chip.
const SB_STAIR = [0x2582, 0x2584, 0x2586, 0x2588];
const sbCell = (state, cp, accent, keep) => ({ state, cp, accent, keep });
const sbText = (s, st) => [...Array.from(s), ' '].map((ch) => sbCell(st, ch.codePointAt(0), 0, true));

function sbNet(st, level) {
  const cells = [];
  for (let i = 0; i < 4; i++) cells.push(sbCell(st, i < level ? SB_STAIR[i] : G.LOW_1, 0, i > 0));
  if (st === SB.OFF) cells[0].cp = G.CROSS;
  return { accent: C.GREEN, cells };
}

// The name, then (keyboard up) every lock word: lit amber, unlit dim.
function sbKbd(st, locks) {
  const word = (s, stt, accent) => Array.from(s).map((ch) => sbCell(stt, ch.codePointAt(0), accent, true));
  let cells = word('KBD', st, 0);
  if (st === SB.ON) {
    ['CAPS', 'NUM'].forEach((w, i) => {
      const lit = !!locks[i];
      cells = cells.concat(word(' ', st, 0), word(w, lit ? SB.ON : SB.DIM, lit ? C.AMBER : 0));
    });
  }
  return { accent: C.CYAN, cells };
}

function sbSync(n) {
  const cells = sbText('SYNC', SB.ALERT);
  cells.push(sbCell(SB.ALERT, 0x30 + n, 0, true));
  return { accent: C.AMBER, cells };
}

// The bar of a screen: the fixed chips and clock, plus its hints. `v`
// names a variant of the data for the mo-status-* gate frames.
function moBarData(touch, keys, v) {
  const s = { chips: [sbNet(SB.ON, 3), sbKbd(SB.ON, [true, false])], clock: '14:07',
              hintTouch: touch, hintKeys: keys, frame: 3 };
  switch (v) {
    case 'busy': s.chips = [sbNet(SB.BUSY, 0), sbKbd(SB.OFF, [])]; break;
    case 'busy-blink': s.chips = [sbNet(SB.BUSY, 0), sbKbd(SB.OFF, [])]; s.frame = 11; break;
    case 'alert': s.chips = [sbNet(SB.ALERT, 0), sbKbd(SB.ON, [true, false]), sbSync(2)]; break;
    case 'off': s.chips = [sbNet(SB.OFF, 0), sbKbd(SB.OFF, [])]; break;
    case 'locks-none': s.chips = [sbNet(SB.ON, 1), sbKbd(SB.ON, [false, false])]; break;
    case 'locks-both': s.chips = [sbNet(SB.ON, 4), sbKbd(SB.ON, [true, true])]; break;
    case 'note': Object.assign(s, { note: 'Host saved', tone: NOTE.PLAIN }); break;
    case 'note-good': Object.assign(s, { note: 'Connected to homelab', tone: NOTE.GOOD }); break;
    case 'note-alert': Object.assign(s, { note: 'Could not save.', tone: NOTE.ALERT }); break;
    case 'hint': s.hintTouch = 'Tap a host to connect. Hold a host for its options and more.'; break;
    case 'net-up': Object.assign(s, { xchip: 0, xtext: 'HomeAP 192.168.1.7', xw: 19 }); break;
    case 'kbd-up': Object.assign(s, { xchip: 1, xtext: 'Logitech K380', xw: 14 }); break;
    case 'net-lost':
      s.chips = [sbNet(SB.BUSY, 0), sbKbd(SB.ON, [true, false])];
      Object.assign(s, { xchip: 0, xtext: 'link lost', xw: 10 });
      break;
    case 'collapse': Object.assign(s, { xchip: 0, xtext: 'HomeAP 192.168.1.7', xw: 9 }); break;
    case 'up-note':
      Object.assign(s, { note: 'Host saved', tone: NOTE.PLAIN, xchip: 0,
                         xtext: 'Homelab-5GHz 192.168.100.200', xw: 29 });
      break;
    default: break;
  }
  return s;
}

const moBarRow = (ui, r, touch, keys, v) => statusBar(ui, r, moBarData(touch, keys, v));

// Page header. With `back` the whole slab is the back target.
// o: { path, title, note, back }. Cut pills and the pager off first.
function moHeader(ui, r, o) {
  const y = moCentre(r.y, r.h, 1, 'header');
  if (!o.back) {
    const tl = moLen(o.title);
    ui.pen(C.DEFAULT);
    ui.text(rect(r.x + 1, y, tl, 1), o.title, S.TEXT | BOLD, LEFT);
    if (o.note && r.w - tl - 3 >= moLen(o.note))
      ui.text(rect(r.x + tl + 3, y, r.w - tl - 3, 1), o.note, S.MUTED, LEFT);
    return;
  }
  const slab = r;
  ui.hot(slab, ID_BACK, o.back);
  const k = moLook(ui, ID_BACK, { tone: 'tint', accent: C.BLUE });
  moSlab(ui, slab, k);
  moFocusBars(ui, slab, k, false);
  let x = slab.x + 2;
  ui.putch(x, y, G.POINT_L, k.title);
  x += 3;
  if (o.path) {
    ui.puts(x, y, `${o.path} /`, k.detail);
    x += moLen(o.path) + 3;
  }
  const room = slab.x + slab.w - 2 - x;
  const nl = o.note ? moLen(o.note) : 0;
  ui.text(rect(x, y, room - (nl ? nl + 2 : 0), 1), o.title, k.title, LEFT);
  if (nl) ui.text(rect(slab.x + slab.w - 2 - nl, y, nl, 1), o.note, k.detail, RIGHT);
  ui.pen(C.DEFAULT);
}

// Every part in its states: normal, finger down, key focus, off.
function moParts(ui) {
  moScreen(ui);
  const POSES = ['', 'down', 'focus', ''];
  const cw = Math.trunc((ui.cols - 2 - 3) / 4);
  const col = (i, y, h) => rect(1 + i * (cw + 1), y, cw, h);
  ui.pen(C.DEFAULT);
  ['normal', 'finger down', 'key focus', 'off'].forEach((t, i) =>
    ui.text(rect(1 + i * (cw + 1) + 1, 0, cw - 1, 1), t, S.MUTED, LEFT));
  let y = 1;
  POSES.forEach((pose, i) => moPart(ui, col(i, y, 4), 1 + i, {
    title: 'nas', detail: 'pi@nas.lan', stripe: C.GREEN, pose, off: i === 3 }));
  y += 4;
  POSES.forEach((pose, i) => moBtn(ui, col(i, y, 3), 11 + i, 'Connect',
    { tone: 'solid', accent: C.GREEN, pose, off: i === 3 }));
  y += 3;
  POSES.forEach((pose, i) => moToggle(ui, col(i, y, 3), 21 + i, 'FX', i !== 0,
    { pose, off: i === 3 }));
  y += 3;
  if (ui.rows - y >= 6) {
    const half = Math.trunc((ui.cols - 3) / 2);
    moChips(ui, rect(1, y, half, 3), 31, 'Tint', ['color', 'green'], 0, () => {}, { poses: ['', 'focus'] });
    moChips(ui, rect(2 + half, y, half, 3), 35, 'Tint', ['color', 'green'], 0, () => {}, { poses: ['focus', 'down'] });
    y += 3;
  }
  if (ui.rows - y >= 3) {
    const row = rect(1, y, ui.cols - 2, 3);
    const rest = moPager(ui, row, 1, 3, {}, {});
    const h = Math.trunc((rest.w - 1) / 2);
    moPart(ui, rect(rest.x, y, h, 3), 41, { title: 'HomeAP', right: '-55 dBm', units: true });
    moBtn(ui, rect(rest.x + h + 1, y, rest.w - h - 1, 3), 42, 'Delete', { tone: 'tint', accent: C.RED });
    y += 3;
  }
  if (ui.rows - y >= 1) moBarRow(ui, rect(0, ui.rows - 1, ui.cols, 1), 'Hold a host for options', '');
}

// Slate screen, then header, body and status rects. Header and body
// share the side margin.
function moPage(ui) {
  moScreen(ui);
  const m = ui.metrics(), r = rect(0, 0, ui.cols, ui.rows);
  const status = cutBottom(r, 1), body = inset(r, m.margin, 0), header = cutTop(body, 3);
  return { header, body, status, m };
}

// Columns of a tile grid: as many tiles of `min` cells as fit, two at least.
const moCols = (w, min, gap) => Math.max(2, Math.trunc((w + gap) / (min + gap)));

// The first key focus of a screen: its main part, not a header button.
function moFirst(ui, id) {
  if (!ui.inter.focus) ui.inter.focus = id;
}

// Buttons cut from the right end of a header rect. Returns the rest.
function moPills(ui, header, pills) {
  for (const p of [...pills].reverse()) {
    moBtn(ui, cutRight(header, moLen(p.label) + 4), p.id, p.label, p.o || {}, p.act);
    cutRight(header, 1);
  }
  return header;
}

// A text panel: label and value lines. Not a touch target.
// A row is [label, value, valuePen, labelPen, tail, tailPen]. A pen
// makes its text bold: colour marks a state, never decoration.
// A plain label takes the label hue. A plain value dims its units.
function moInfo(ui, r, rows, stripe) {
  if (r.h !== rows.length + 2) throw new Error(`info: ${rows.length} lines need ${rows.length + 2} rows`);
  moSlab(ui, r, moLook(ui, 0, {}));
  if (stripe) moStripe(ui, r, stripe);
  const lw = rows.reduce((n, [l]) => Math.max(n, moLen(l)), 0) + 2;
  rows.forEach(([label, value, pen, labelPen, tail, tailPen], i) => {
    const y = r.y + 1 + i, vx = r.x + 2 + lw, vw = r.w - 4 - lw;
    ui.pen(labelPen || MO_LABEL);
    ui.text(rect(r.x + 2, y, lw, 1), label, labelPen ? S.PANEL | BOLD : S.PANEL_MUTED, LEFT);
    ui.pen(pen | 0);
    if (pen) ui.text(rect(vx, y, vw, 1), value, S.PANEL | BOLD, LEFT);
    else moUnits(ui, rect(vx, y, vw, 1), value, S.PANEL, S.PANEL_MUTED);
    if (tail) {
      ui.pen(tailPen | 0);
      ui.text(rect(vx + moLen(value) + 2, y, vw - moLen(value) - 2, 1), tail, S.PANEL | BOLD, LEFT);
    }
  });
  ui.pen(C.DEFAULT);
}

// A form field: label, then the value. Editing lights it and puts a caret
// on character o.caret (default: after the last). A long value scrolls to
// keep the caret in view, as ui_entry_at does.
// o.bad marks an invalid value: red stripe, red text, a mark at the end.
function moField(ui, r, id, label, value, o, act) {
  const p = moPart(ui, r, id, { title: '', accent: C.CYAN, lit: o.edit, stripe: o.bad ? C.RED : 0 }, act);
  if (!p) return;
  const k = p.k, lw = o.lw || 10, vx = p.inn.x + lw, vw = p.inn.w - lw;
  const cs = Array.from(value), n = cs.length, room = vw - (o.bad ? 2 : 0);
  const caret = Math.max(0, Math.min(o.caret === undefined ? n : o.caret, n));
  const first = o.edit && n > room - 1 ? entryFirst(n, caret, room) : 0;
  const shown = o.mask ? '•'.repeat(n - first) : cs.slice(first).join('');
  ui.pen(k.card ? MO_LABEL : k.pen);
  ui.text(rect(p.inn.x, p.y, lw - 1, 1), label, k.detail, LEFT);
  ui.pen(o.bad && k.card ? C.RED : k.pen);
  ui.text(rect(vx, p.y, room, 1), shown, k.title, LEFT);
  if (o.bad) ui.putch(p.inn.x + p.inn.w - 1, p.y, '!', k.title);
  ui.pen(k.pen);
  if (o.edit) {
    const under = caret < n ? (o.mask ? '•' : cs[caret]) : ' ';
    ui.pen(C.WHITE);
    ui.putch(Math.min(vx + caret - first, vx + vw - 1), p.y, under, S.BAR);
  }
  ui.pen(C.DEFAULT);
}

// "user@host:port  key" in parts.
function moParse(where) {
  const m = /^([^@]+)@([^:]+):(\d+)(\s+key)?/.exec(where);
  return m ? { user: m[1], host: m[2], port: m[3], key: !!m[4] }
           : { user: '', host: where, port: '22', key: false };
}
const moHostOf = (st) => (st.hosts || PROFILES)[st.host | 0] || PROFILES[0];

// HOME: a host per tile. A tap connects. A hold opens the host page.
// New, Setup and Lock sit in the header, so no host count hides them.
function moHome(ui, app) {
  const st = app.state, pg = moPage(ui), hosts = st.hosts || PROFILES;
  const g = grid(pg.body, moCols(pg.body.w, 30, pg.m.gutter), 4, pg.m.gutter, 0);
  const per = gridCapacity(g), pages = pageCount(hosts.length, per);
  st.page = pageClamp(st.page | 0, hosts.length, per);
  moFirst(ui, MO_ID.HOST + st.page * per);
  const head = moPills(ui, pg.header, [
    ...(st.pair ? [{ id: MO_ID.PILL + 3, label: 'Pair', o: { tone: 'tint', accent: C.CYAN } }] : []),
    { id: MO_ID.PILL, label: '+ New', act: { tap: () => { st.host = -1; app.go('mo-edit'); } } },
    { id: MO_ID.PILL + 1, label: 'Setup', act: { tap: () => app.go('mo-setup') } },
    { id: MO_ID.PILL + 2, label: 'Lock', act: { tap: () => app.go('mo-unlock') } },
  ]);
  const rest = moPager(ui, head, st.page, pages,
    { prev: { tap: () => st.page-- }, next: { tap: () => st.page++ } });
  moHeader(ui, rest, { title: 'Hosts', note: `${hosts.length} of ${hosts.length > 8 ? 32 : 8}` });
  const first = st.page * per, shown = Math.min(per, hosts.length - first);
  // Arrow keys turn the page at its edges (pageStep); the viewer reads this.
  ui.paged = { base: MO_ID.HOST, count: hosts.length, cols: g.ncols, per,
               go: (to) => { st.page = Math.trunc(to / per); } };
  for (let i = 0; i < shown; i++) {
    const n = first + i, p = hosts[n], w = moParse(p.where);
    const act = { tap: () => { st.host = n; app.go('mo-connect'); },
                  hold: () => { st.host = n; app.go('mo-host'); } };
    const last = st.lastRow && i === Math.trunc((shown - 1) / g.ncols) * g.ncols;
    moPart(ui, gridCell(g, i), MO_ID.HOST + n, {
      title: p.name, detail: `${w.user}@${w.host}${w.port === '22' ? '' : `:${w.port}`}`,
      stripe: profAccent(p.name), right: i < 9 ? String(i + 1) : '',
      pose: last ? 'focus' : '' }, act);
    if (i < 9) ui.key(String(i + 1), act.tap);
  }
  moBarRow(ui, pg.status, 'Tap to connect. Hold for options.', '↵ connect  Space options  1-9',
    st.bar);
}

// The host page: what a hold on a tile opens.
function moHost(ui, app) {
  const st = app.state, pg = moPage(ui), p = moHostOf(st), w = moParse(p.where);
  moHeader(ui, pg.header, { path: 'Hosts', title: p.name, back: { tap: () => app.back() } });
  const body = pg.body;
  moInfo(ui, cutTop(body, 6), [
    ['Address', `${w.host}  port ${w.port}`], ['User', w.user],
    ['Sign-in', w.key ? 'key deck-ed25519' : 'password, stored'],
    ['Host key', 'SHA256:9fK2..Qm4c', 0, 0, '✓ pinned', C.GREEN],
  ], profAccent(p.name));
  // One row for what you do with the host, one for its place in the list.
  const f = moFlow(body, pg.m.gutter, 3);
  const btn = (n, label, o, tap) => moBtn(ui, moFlowNext(f, moBtnW(label)), MO_ID.BTN + n, label, o, { tap });
  btn(0, 'Connect', { tone: 'solid', accent: C.GREEN }, () => app.go('mo-connect'));
  btn(1, 'Edit', {}, () => app.go('mo-edit'));
  btn(2, 'Delete', { tone: 'tint', accent: C.RED }, () => { st.ask = null; app.go('mo-confirm'); });
  moFlowBreak(f);
  btn(3, '↑ Move earlier', {}, () => { app.note = 'moved'; });
  btn(4, '↓ Move later', {}, () => { app.note = 'moved'; });
  moBarRow(ui, pg.status,app.note || 'The bar at the top goes back.', '↵ choose  Esc back');
}

// A question as its own page: a text panel, then the two answers.
// The safe answer comes first, so it holds the first key focus.
// st.ask: { title, lines, verb, accent, yes() }. Without it: delete a host.
function moConfirm(ui, app) {
  const st = app.state, pg = moPage(ui), p = moHostOf(st);
  const ask = st.ask || { title: `Delete ${p.name}?`, verb: 'Delete',
    lines: ['The entry and its stored password go.', 'The pinned host key stays.'] };
  const accent = ask.accent || C.RED, yes = ask.yes || (() => app.home());
  const area = rect(pg.body.x, pg.header.y, pg.body.w, pg.header.h + pg.body.h);
  const box = center(area, Math.min(area.w, 46), 9), card = cutTop(box, 6);
  moSlab(ui, card, moLook(ui, 0, {}));
  moStripe(ui, card, accent);
  ui.text(rect(card.x + 2, card.y + 1, card.w - 4, 1), ask.title, S.PANEL | BOLD, LEFT);
  ask.lines.forEach((l, i) =>
    ui.text(rect(card.x + 2, card.y + 3 + i, card.w - 4, 1), l, S.PANEL_MUTED, LEFT));
  const f = moFlow(box, pg.m.gutter, 3);
  moBtn(ui, moFlowNext(f, moBtnW('Cancel')), MO_ID.BTN, 'Cancel', {}, { tap: () => app.back() });
  moBtn(ui, moFlowNext(f, moBtnW(ask.verb)), MO_ID.BTN + 1, ask.verb, { tone: 'solid', accent }, { tap: yes });
  moBarRow(ui, pg.status,'', '←→ move  ↵ choose  Esc cancel');
}

const MO_FIELDS = [
  { key: 'name', label: 'Name' }, { key: 'user', label: 'User' },
  { key: 'host', label: 'Host' }, { key: 'port', label: 'Port', lw: 6 },
  { key: 'pass', label: 'Password', mask: true },
];

// Keys stored on the deck, as the host form lists them.
const MO_KEYS = [{ id: 'deck-ed25519', type: 'ssh-ed25519' }, { id: 'work-rsa', type: 'ssh-rsa' }];

// Right text of the Key part, `cap` cells at most. Id, type and place in
// the list as far as they fit; pf_key_text in app_profile.c.
function moKeyText(key, keys, cap) {
  const at = keys.findIndex((k) => k.id === key);
  if (at < 0) return key ? `${key.slice(0, cap - 10)} (missing)` : 'none stored';
  const { id, type } = keys[at], pos = keys.length > 1 ? ` ${at + 1}/${keys.length}` : '';
  if (type && id.length + type.length + pos.length + 2 <= cap) return `${id}  ${type}${pos}`;
  return `${id.slice(0, cap - pos.length)}${pos}`;
}

// The host form. Save sits in the header. The header bar is Cancel.
// st.edit: the entry in typing, else -1. st.caret: its caret. st.keys: the stored keys.
function moEdit(ui, app) {
  const st = app.state, pg = moPage(ui), isNew = st.host === -1;
  if (!st.form || st.formFor !== st.host) {
    const p = moHostOf(st), w = moParse(p.where);
    st.form = isNew ? { name: '', user: '', host: '', port: '22', pass: '', auth: 0, key: '', show: false }
      : { name: p.name, user: w.user, host: w.host, port: w.port, pass: 'hunter22',
          auth: w.key ? 1 : 0, key: w.key ? MO_KEYS[0].id : '', show: false };
    st.formFor = st.host;
    st.formWas = JSON.stringify(st.form);
    st.orig = isNew ? '' : p.name;
    st.edit = -1;
  }
  const f = st.form, body = pg.body, gx = pg.m.gutter, keys = st.keys || MO_KEYS;
  moFirst(ui, MO_ID.FIELD);
  const changed = JSON.stringify(f) !== st.formWas;
  const port = /^\d+$/.test(f.port) && +f.port >= 1 && +f.port <= 65535;
  // An untouched new form shows no red yet. Save stays off until it is valid.
  const bad = { port: !port, name: !f.name, host: !f.host, user: !f.user, key: f.auth === 1 && !f.key };
  const wrong = Object.values(bad).some(Boolean);
  const alert = bad.port ? 'Port: a number from 1 to 65535.'
    : bad.name && changed ? 'A host needs a name.'
    : bad.host && changed ? 'A host needs an address.'
    : bad.user && changed ? 'A host needs a user.'
    : bad.key ? 'No stored key. Setup, Hosts imports one.' : '';
  const leave = () => { st.form = null; app.back(); };
  // A changed form asks first. An unchanged form leaves at once.
  const cancel = () => {
    st.edit = -1;
    if (!changed) { leave(); return; }
    st.ask = { title: 'Discard the changes?', verb: 'Discard', accent: C.AMBER,
      lines: [isNew ? 'The new host is not stored.' : 'This host keeps its old values.', ''],
      yes: () => { app.back(); leave(); } };
    app.go('mo-confirm');
  };
  const head = moPills(ui, pg.header, [{ id: MO_ID.PILL, label: 'Save',
    o: { tone: 'solid', accent: C.GREEN, off: wrong }, act: { tap: leave } }]);
  moHeader(ui, head, { path: 'Hosts', title: isNew ? 'New host' : `Edit ${st.orig}`,
    back: { tap: cancel } });
  ui.key('Escape', cancel);
  const half = (body.w - gx) >> 1, portW = 17, wide = body.w - gx - portW;
  const field = (i, r, label, lw) => {
    const d = MO_FIELDS[i], edit = st.edit === i;
    moField(ui, r, MO_ID.FIELD + i, label || d.label, f[d.key],
      { edit, caret: st.caret, mask: d.mask && !f.show, lw: lw || d.lw,
        bad: d.key !== 'pass' && bad[d.key] && (changed || d.key === 'port') },
      { tap: () => { st.edit = edit ? -1 : i; st.caret = Array.from(f[d.key]).length; } });
  };
  let y = body.y;
  field(0, rect(body.x, y, half, 3));
  field(1, rect(body.x + half + gx, y, body.w - half - gx, 3));
  y += 3;
  field(2, rect(body.x, y, wide, 3));
  field(3, rect(body.x + wide + gx, y, portW, 3));
  y += 3;
  // The chips start in the fields' value column.
  moChips(ui, rect(body.x, y, body.w, 3), MO_ID.OPT, 'Sign-in', ['password', 'key'], f.auth,
    (i) => {
      f.auth = i;
      if (i && !f.key && keys.length) f.key = keys[0].id;
      st.edit = -1;
    }, { capW: 12 });
  y += 3;
  const key = f.auth === 1;
  if (key) {
    const right = moKeyText(f.key, keys, body.w - 12);
    moPart(ui, rect(body.x, y, moActW('Key', right), 3), MO_ID.OPT + 5,
      { title: 'Key', right, rightBold: true, pad: 3, off: !keys.length }, { tap: () => {
        const at = keys.findIndex((k) => k.id === f.key);
        f.key = keys[(at + 1) % keys.length].id;
      } });
    y += 3;
  }
  const toggle = key ? 'Show passphrase' : 'Show password';
  field(4, rect(body.x, y, half, 3), key ? 'Passphrase' : 'Password', key ? 12 : 0);
  moToggle(ui, rect(body.x + half + gx, y, moToggleW(toggle), 3), MO_ID.OPT + 4,
    toggle, f.show, { pad: 3 }, { tap: () => { f.show = !f.show; } });
  y += 3;
  if (alert && y < body.y + body.h) {
    ui.pen(C.RED);
    ui.text(rect(body.x + 2, y, body.w - 4, 1), alert, S.TEXT | BOLD, LEFT);
    ui.pen(C.DEFAULT);
  }
  if (st.edit >= 0) {
    const d = MO_FIELDS[st.edit], cs = Array.from(f[d.key]);
    const caret = Math.min(st.caret === undefined ? cs.length : st.caret, cs.length);
    ui.key('text', (ch) => {
      if (d.key === 'port' && !/^\d$/.test(ch)) return;
      if (d.key === 'name' && /[[\]]/.test(ch)) return;
      f[d.key] = [...cs.slice(0, caret), ch, ...cs.slice(caret)].join('');
      st.caret = caret + 1;
    });
    ui.key('Backspace', () => {
      if (caret === 0) return;
      f[d.key] = [...cs.slice(0, caret - 1), ...cs.slice(caret)].join('');
      st.caret = caret - 1;
    });
    const done = () => { st.edit = -1; };
    ui.key('Enter', done);
    ui.key('Escape', done);
    // Up and down leave the field. Left and right move the caret.
    ui.key('ArrowUp', done);
    ui.key('ArrowDown', done);
    ui.key('ArrowLeft', () => { st.caret = Math.max(0, caret - 1); });
    ui.key('ArrowRight', () => { st.caret = Math.min(cs.length, caret + 1); });
  }
  moBarRow(ui, pg.status,st.edit >= 0 ? 'Type on the keyboard.' : 'Tap a field to change it.',
    st.edit >= 0 ? 'type  ↵ done  ↑↓ leave' : '↵ change  Esc cancel');
}

// The Wi-Fi manager and the saved networks a frame shows (st.wifi).
const moNets = (st) => st.wifi || WIFI_UP;
const moJoined = (w, n) => w.state === 'up' && n.ssid === w.ssid;

// Signal word: the same cuts as the status chip (SB_RSSI_FAIR, SB_RSSI_WEAK).
const moSignal = (rssi) => (rssi > -67 ? ['good', C.GREEN] : rssi > -78 ? ['fair', C.AMBER] : ['weak', C.RED]);

// The info panel of the Wi-Fi section: lines and the stripe accent.
function moWifiPanel(st) {
  const w = moNets(st), up = w.state === 'up', [word, pen] = moSignal(w.rssi);
  return { stripe: up ? C.GREEN : 0, rows: [
    ['Network', up ? w.ssid : w.state],
    ['Address', up ? w.ip : '-'],
    ['Gateway', up ? w.gw : '-'],
    up && w.rssi ? ['Signal', `${w.rssi} dBm`, 0, 0, word, pen] : ['Signal', '-'],
  ] };
}

// One tile per saved network, in preference order.
const moWifiRows = (st) => moNets(st).nets.map((n, i) => {
  const joined = moJoined(moNets(st), n);
  return { title: n.ssid, right: String(i + 1), stripe: joined ? C.GREEN : 0,
           detail: joined ? '✓ joined' : n.pw === 'locked' ? 'locked' : 'saved',
           detailPen: joined ? C.GREEN : 0 };
});

const moHostRows = (st) => (st.hosts || PROFILES).map((p) => {
  const w = moParse(p.where);
  return { title: p.name, detail: `${w.user}@${w.host}${w.port === '22' ? '' : `:${w.port}`}`,
           stripe: profAccent(p.name) };
});

const MO_SETUP = [
  // `panel`: a text panel above the items. `list`: rows as tiles under the
  // items, on every page of tiles; `rows` makes the tile parts, `open` runs
  // on a tap. The hosts list is shut while a session is live (st.live).
  // The store holds st.max hosts (8 on the deck); a full store shuts "+ New".
  { name: 'Hosts', note: '8 stored', list: { minW: 30, empty: 'No hosts yet.', rows: moHostRows,
      off: (st) => !!st.live, open: (st, n, app) => { st.host = n; app.go('mo-host'); } }, items: [
    { t: 'act', label: '+ New', off: (st, n) => st.live || n >= (st.max || 8), go: 'mo-edit', host: -1 },
    { t: 'act', label: 'From phone', nav: true },
    { t: 'act', label: 'From PC', nav: true }] },
  { name: 'Wi-Fi', note: 'HomeAP', panel: moWifiPanel,
    list: { minW: 24, empty: 'No saved networks.', rows: moWifiRows,
      open: (st, n, app) => { st.net = n; app.go('mo-net'); } }, items: [
    { t: 'act', label: 'Reconnect' }, { t: 'act', label: 'Add from phone', nav: true }] },
  { name: 'Keyboard', note: 'K380 connected', items: [
    { t: 'act', label: 'Pair keyboard', nav: true },
    { t: 'act', label: 'Forget bonds', accent: C.RED, ask: 'Forget every paired keyboard?' }] },
  { name: 'CRT FX', note: 'scanlines, phosphor', items: [
    { t: 'toggle', key: 'scan', label: 'Scanlines', init: true },
    { t: 'toggle', key: 'pop', label: 'Bold pop', init: true },
    { t: 'pick', key: 'phos', label: 'Phosphor', set: ['color', 'green', 'amber'], init: 0 }] },
  { name: 'Motion FX', note: 'wobble, wipe, static', items: [
    { t: 'pick', key: 'wob', label: 'Wobble', set: ['off', 'subtle', 'medium', 'deep'], init: 2 },
    { t: 'pick', key: 'wipe', label: 'Wipe in', set: ['off', '0.3 s', '0.6 s'], init: 1 },
    { t: 'pick', key: 'coll', label: 'Collapse', set: ['off', '0.2 s', '0.5 s'], init: 1 },
    { t: 'pick', key: 'stat', label: 'Static', set: ['off', 'light', 'heavy'], init: 1 }] },
  { name: 'System', note: 'saver, reset', items: [
    { t: 'pick', key: 'font', label: 'Font size', set: ['8x16', '10x20', '12x24'], init: 2,
      ask: (v) => ({ title: `Restart with the ${v} font?`, verb: 'Restart', accent: C.AMBER,
        lines: ['The deck restarts to change the font.', 'An open session ends.'] }) },
    { t: 'pick', key: 'saver', label: 'Saver + lock, min', set: ['1', '3', '5', '10', '30'], init: 2 },
    { t: 'toggle', key: 'edge', label: 'Edge scroll', init: false },
    { t: 'act', label: 'Clear host keys', accent: C.RED, danger: true, ask: 'Clear every pinned host key?' },
    { t: 'act', label: 'Factory reset', accent: C.RED, danger: true, ask: 'Erase every host and setting?' }] },
  { name: 'Keystore', note: 'unlocked', items: [
    { t: 'act', label: 'Lock deck', go: 'mo-unlock' }, { t: 'act', label: 'Change code', nav: true },
    { t: 'act', label: 'Remove code', accent: C.RED, danger: true, nav: true, go: 'mo-unlock' }] },
];

// Setup hub: a section per tile, with its present state as the detail.
function moSetup(ui, app) {
  const st = app.state, pg = moPage(ui);
  moHeader(ui, pg.header, { title: 'Setup', back: { tap: () => app.back() } });
  const g = grid(pg.body, moCols(pg.body.w, 24, pg.m.gutter), 4, pg.m.gutter, 0);
  MO_SETUP.forEach((c, i) => moPart(ui, gridCell(g, i), MO_ID.HUB + i,
    { title: c.name, detail: c.note, right: '›' },
    { tap: () => { st.cat = i; st.spage = 0; app.go('mo-section'); } }));
  moBarRow(ui, pg.status,'The bar at the top goes back.', '↵ open  Esc back');
}

// The tiles of a section list in `area`, as HOME draws a host; or the empty
// text. list.off shuts the rows. st.tileFocus lights the first tile of the
// page. Arrows at a page edge turn it (ui.paged).
function moSectionList(ui, app, list, rows, area, gx, L) {
  const st = app.state;
  if (!rows.length) {
    ui.pen(C.DEFAULT);
    ui.text(rect(area.x + 1, area.y, area.w - 2, 1), list.empty, S.MUTED, LEFT);
    return;
  }
  const g = grid(area, L.tcols, 4, gx, 0), first = st.spage * L.tper;
  ui.paged = { base: MO_ID.HOST, count: rows.length, cols: L.tcols, per: L.tper,
               go: (to) => { st.spage = Math.trunc(to / L.tper); } };
  for (let i = 0; i < L.tper && first + i < rows.length; i++) {
    const n = first + i;
    moPart(ui, gridCell(g, i), MO_ID.HOST + n, {
      ...rows[n], off: !!(list.off && list.off(st)), pose: st.tileFocus && i === 0 ? 'focus' : '' },
    { tap: () => list.open(st, n, app) });
  }
}

// One section. A choice takes a row and shows every option, so no value
// hides behind a cycle. Switches and actions keep their own width and
// pack into rows from the left.
function moSection(ui, app) {
  const st = app.state, pg = moPage(ui), c = MO_SETUP[st.cat | 0], body = pg.body;
  const gx = pg.m.gutter;
  st.opt = st.opt || {};
  // The panel takes the top of the body; the rows and tiles lay out below.
  const panel = c.panel ? c.panel(st) : null;
  const pan = panel ? cutTop(body, panel.rows.length + 2) : null;
  const own = (it) => (it.t === 'toggle' ? moToggleW(it.label) : moActW(it.label, it.nav ? '›' : ''));
  const pack = (room, danger) => {
    const out = [];
    let open = null, used = 0;
    c.items.forEach((it, n) => {
      if (!!it.danger !== danger) return;
      const cell = { it, id: MO_ID.OPT + n * 8, w: it.t === 'pick' ? room : Math.min(own(it), room) };
      if (it.t === 'pick') { out.push([cell]); open = null; }
      else if (open && used + gx + cell.w <= room) { open.push(cell); used += gx + cell.w; }
      else { open = [cell]; out.push(open); used = cell.w; }
    });
    return out;
  };
  // Plain rows fill pages top down. Danger rows take the last page, with
  // one empty row above. A full last page gives them a page of their own.
  const per = Math.max(1, Math.trunc(body.h / 3));
  const lay = (room) => {
    const plain = pack(room, false), danger = pack(room, true);
    const pp = Math.ceil(plain.length / per);
    const tail = plain.length - (pp ? (pp - 1) * per : 0);
    const own = pp && danger.length && tail + 1 + danger.length > per ? 1 : 0;
    const n = Math.max(1, pp + own);
    return { plain, danger, pages: n };
  };
  // A section with a list. Plain rows sit above the tiles on every page.
  // Danger rows hang below them on the last. Tiles take the rest of the
  // body, 4 rows each. The pages are pages of tiles.
  const hosts = c.list ? c.list.rows(st) : [];
  const listOff = !!(c.list && c.list.off && c.list.off(st));
  const layList = (room) => {
    const plain = pack(room, false), danger = pack(room, true);
    const listY = plain.length ? plain.length * 3 + 1 : 0;
    const listH = Math.max(0, body.h - listY - (danger.length ? danger.length * 3 + 1 : 0));
    const g = grid(rect(0, 0, room, listH), moCols(room, c.list.minW, gx), 4, gx, 0);
    const tper = gridCapacity(g);
    return { plain, danger, listY, listH, tcols: g.ncols, tper, pages: pageCount(hosts.length, tper) };
  };
  // A paged section gives two columns to the rail, so it packs again.
  const layout = c.list ? layList : lay;
  let L = layout(body.w);
  if (L.pages > 1) L = layout(body.w - 2);
  const pages = L.pages;
  // pg -1: a row of every page.
  const rows = L.plain.map((r, i) => (c.list ? { r, pg: -1, y: i * 3 }
      : { r, pg: Math.trunc(i / per), y: (i % per) * 3 }))
    .concat(L.danger.map((r, i) => ({ r, pg: pages - 1,
      y: Math.max(0, body.h - (L.danger.length - i) * 3) })));
  const itOff = (it) => !!(it.off && it.off(st, hosts.length));
  const onPage = (row) => row.pg < 0 || row.pg === st.spage;
  const capW = c.items.reduce((n, it) => (it.t === 'pick' ? Math.max(n, moCapW(it.label)) : n), 0);
  st.spage = Math.min(Math.max(st.spage | 0, 0), pages - 1);
  // The first key focus: the first live item of the page, else its first tile.
  const firstCell = rows.filter(onPage).flatMap((row) => row.r).find((cell) => !itOff(cell.it));
  moFirst(ui, firstCell ? firstCell.id
    : c.list && hosts.length && !listOff ? MO_ID.HOST + st.spage * L.tper : MO_ID.OPT);
  const flip = { prev: { tap: () => st.spage-- }, next: { tap: () => st.spage++ } };
  const head = moPager(ui, pg.header, st.spage, pages, flip);
  moHeader(ui, head, { path: 'Setup', title: c.name, back: { tap: () => app.back() } });
  if (pages > 1) {
    moRail(ui, cutRight(body, 1), st.spage, pages);
    cutRight(body, 1);
    ui.key('PageUp', () => { if (st.spage > 0) st.spage--; });
    ui.key('PageDown', () => { if (st.spage < pages - 1) st.spage++; });
  }
  if (pan) moInfo(ui, rect(pan.x, pan.y, pan.w - (pages > 1 ? 2 : 0), pan.h), panel.rows, panel.stripe);
  if (c.list) moSectionList(ui, app, c.list, hosts, rect(body.x, body.y + L.listY, body.w, L.listH), gx, L);
  rows.filter(onPage).forEach(({ r: row, y: dy }) => {
    const y = body.y + dy;
    let x = body.x;
    row.forEach(({ it, id, w }) => {
      const r = rect(x, y, w, 3);
      x += w + gx;
      const v = it.key in st.opt ? st.opt[it.key] : it.init;
      if (it.t === 'pick') moChips(ui, r, id, it.label, it.set, v, (i) => {
        if (!it.ask || i === v) { st.opt[it.key] = i; return; }
        st.ask = { ...it.ask(it.set[i]), yes: () => { st.opt[it.key] = i; app.back(); } };
        app.go('mo-confirm');
      }, { capW });
      else if (it.t === 'toggle')
        moToggle(ui, r, id, it.label, v, { pad: 3 }, { tap: () => { st.opt[it.key] = !v; } });
      else moPart(ui, r, id, {
        title: it.label, right: it.nav ? '›' : '', tone: it.accent ? 'tint' : 'card', accent: it.accent,
        pad: 3, off: itOff(it),
      }, { tap: () => {
        if (it.host !== undefined) st.host = it.host;
        if (it.go) app.go(it.go);
        else if (it.ask) {
          st.ask = { title: it.ask, verb: it.label, lines: ['This cannot be undone.', ''],
            yes: () => { app.back(); app.note = `${it.label}: done`; } };
          app.go('mo-confirm');
        } else app.note = it.label;
      } });
    });
  });
  moBarRow(ui, pg.status,app.note || 'A change applies at once.', '↵ change  Esc back');
}

// A saved network's page. st.net is its place in the list. It shows facts,
// Join now, the two moves, and Forget apart at the bottom. Join now is off
// for the joined network and for a network with a locked password.
function moNet(ui, app) {
  const st = app.state, pg = moPage(ui), w = moNets(st), body = pg.body;
  const at = Math.min(st.net | 0, w.nets.length - 1), n = w.nets[at];
  const joined = moJoined(w, n), locked = n.pw === 'locked';
  moHeader(ui, pg.header, { path: 'Wi-Fi', title: n.ssid, back: { tap: () => app.back() } });
  moInfo(ui, cutTop(body, 5), [
    ['Network', n.ssid, 0, 0, joined ? '✓ joined' : '', C.GREEN],
    ['Position', `${at + 1} of ${w.nets.length}`],
    ['Password', n.pw],
  ], joined ? C.GREEN : 0);
  const offJoin = joined || locked, offUp = at <= 0, offDown = at >= w.nets.length - 1;
  moFirst(ui, MO_ID.BTN + (!offJoin ? 0 : !offUp ? 1 : !offDown ? 2 : 3));
  const f = moFlow(body, pg.m.gutter, 3);
  const btn = (i, label, o, tap) => moBtn(ui, moFlowNext(f, moBtnW(label)), MO_ID.BTN + i, label, o, { tap });
  btn(0, 'Join now', { tone: 'solid', accent: C.GREEN, off: offJoin }, () => { app.note = 'joining'; });
  moFlowBreak(f);
  btn(1, '↑ Move earlier', { off: offUp }, () => { app.note = 'moved'; });
  btn(2, '↓ Move later', { off: offDown }, () => { app.note = 'moved'; });
  moBtn(ui, rect(body.x, body.y + body.h - 3, moBtnW('Forget'), 3), MO_ID.BTN + 3, 'Forget',
    { tone: 'tint', accent: C.RED }, { tap: () => {
      st.ask = { title: `Forget ${n.ssid}?`, verb: 'Forget', accent: C.RED,
        lines: [joined ? 'The deck leaves this network.' : 'The network and its password go.', ''],
        yes: () => { app.back(); app.back(); } };
      app.go('mo-confirm');
    } });
  moBarRow(ui, pg.status, app.note || 'The bar at the top goes back.', '↵ choose  Esc back');
}

const MO_NETS = [
  ['HomeAP', 3, 'saved'], ['HomeAP-5G', 3, 'saved'], ['garage', 2, ''], ['Workshop', 2, 'saved'],
  ['printer-setup', 2, 'open'], ['Neighbour 2.4', 1, ''], ['cafe-guest', 1, 'open'], ['IoT', 3, ''],
  ['vodafone-4F2A', 1, ''], ['Lab', 2, ''], ['phone-hotspot', 3, 'saved'], ['TP-Link_9C10', 1, ''],
  ['backup-link', 2, ''],
];

// A long list in pages. The header carries the page and its controls.
// The rail at the side shows where the page sits in the list.
function moList(ui, app) {
  const st = app.state, pg = moPage(ui), body = pg.body;
  moFirst(ui, MO_ID.ROW);
  const rail = cutRight(body, 1);
  cutRight(body, 1);
  const g = grid(body, pg.m.cls === WIDE ? 3 : 2, 3, pg.m.gutter, 0);
  const per = gridCapacity(g), pages = pageCount(MO_NETS.length, per);
  st.lpage = pageClamp(st.lpage | 0, MO_NETS.length, per);
  const head = moPager(ui, pg.header, st.lpage, pages,
    { prev: { tap: () => st.lpage-- }, next: { tap: () => st.lpage++ } });
  moHeader(ui, head, { path: 'Wi-Fi', title: 'Networks', note: `${MO_NETS.length} found`,
    back: { tap: () => app.back() } });
  moRail(ui, rail, st.lpage, pages);
  const joined = st.net | 0;
  for (let i = 0; i < per && st.lpage * per + i < MO_NETS.length; i++) {
    const n = st.lpage * per + i, [ssid, bars, tag] = MO_NETS[n], on = n === joined;
    const ladder = [0x2582, 0x2584, 0x2586].map((cp, b) => String.fromCodePoint(b < bars ? cp : 0x2581)).join('');
    moPart(ui, gridCell(g, i), MO_ID.ROW + n, {
      title: ssid, right: `${on ? '✓ joined' : tag} ${ladder}`.trim(), rightBold: on,
      tone: on ? 'tint' : 'card', accent: on ? C.GREEN : 0,
    }, { tap: () => { st.net = n; } });
  }
  ui.key('PageUp', () => { if (st.lpage > 0) st.lpage--; });
  ui.key('PageDown', () => { if (st.lpage < pages - 1) st.lpage++; });
  moBarRow(ui, pg.status,'Tap a network to join it.', '↵ join  PgUp PgDn page  Esc back');
}

// Title and note of the pad for its phase; phase_head in app_unlock.c.
// st.phase: unlock, old, new, confirm or remove. A flow (st.flow) has a
// back target; the device gate has none.
function moUnlockHead(st) {
  const phase = st.phase || 'unlock';
  if (phase === 'old') return ['Change code', 'Enter the current code'];
  if (phase === 'new' || phase === 'confirm')
    return [st.creating ? 'Create keystore' : 'Change code',
            phase === 'new' ? 'Choose a new code, 4 or more' : 'Enter the new code again'];
  if (phase === 'remove') return ['Remove code', 'Enter the code to remove it'];
  return [st.flow ? 'Keystore locked' : 'Deck locked',
          st.resume ? 'Enter the access code to connect' : 'Enter the access code'];
}

// Marks of the typed code: bullets for the taken characters, muted dots
// for those still to come. st.reveal echoes the newest one.
function moCodeRow(ui, line, st, len) {
  const c = codePlan(len, st.expected | 0, line.w), step = c.spaced ? 2 : 1;
  let x = line.x + ((line.w - c.width) >> 1);
  ui.pen(C.DEFAULT);
  for (let i = 0; i < c.bullets + c.dots; i++, x += step) {
    if (i >= c.bullets) ui.putch(x, line.y, 0xB7, S.PANEL_MUTED);
    else if (i === c.bullets - 1 && st.reveal) ui.putch(x, line.y, st.reveal, S.PANEL | BOLD);
    else ui.putch(x, line.y, 0x2022, S.PANEL | BOLD);
  }
}

// The entry panel. st.note is a red line, st.wait the seconds left of a
// backoff, st.deriving the check that runs after a submit.
function moEntryPanel(ui, entry, st, len) {
  const line = rect(entry.x + 2, entry.y + 1, entry.w - 4, 1), wait = st.wait | 0;
  moSlab(ui, entry, moLook(ui, 0, {}));
  if (st.note && !st.deriving) moStripe(ui, entry, C.RED);
  ui.pen(C.DEFAULT);
  if (st.deriving) {
    const glyph = String.fromCodePoint(spinnerGlyph(ui.frameNo));
    ui.text(line, `${glyph} Checking`, S.PANEL | BOLD, CENTER);
  } else if (st.note) {
    ui.pen(C.RED);
    ui.text(line, wait > 0 && !len ? `${st.note}, wait ${wait} s` : st.note, S.PANEL | BOLD, CENTER);
  } else if (wait > 0 && !len) {
    ui.pen(C.AMBER);
    ui.text(line, `Wait ${wait} s`, S.PANEL | BOLD, CENTER);
  } else if (!len) {
    const defining = st.phase === 'new' || st.phase === 'confirm';
    ui.text(line, defining ? 'new code' : 'access code', S.PANEL_MUTED, CENTER);
  } else moCodeRow(ui, line, st, len);
  ui.pen(C.DEFAULT);
}

// Access code pad. Keys are five rows high where they fit, else three:
// an odd height keeps the digit centred.
function moUnlock(ui, app) {
  const st = app.state, pg = moPage(ui), code = st.code || '', len = moLen(code);
  const [title, note] = moUnlockHead(st);
  moHeader(ui, pg.header, { title, note, back: st.flow ? { tap: () => app.back() } : undefined });
  if (st.flow) ui.key('Escape', () => app.back());
  const kh = pg.body.h >= 3 + 4 * 5 ? 5 : 3;
  const kw = 9, w = 3 * kw + 2, pad = center(pg.body, w, 3 + 4 * kh);
  if (pg.body.h < 15) { ui.issues.push({ kind: 'drop', text: 'code pad' }); return; }
  moEntryPanel(ui, cutTop(pad, 3), st, len);
  const submit = (c) => {
    if (!c) return;
    st.code = '';
    // The mock treats 0000 as the wrong code.
    st.note = c === '0000' ? '✘ wrong code' : '';
    st.wait = st.note ? 12 : 0;
    if (!st.note) app.home();
  };
  const enter = () => submit(code);
  const add = (d) => () => {
    if (len >= 13) return;
    st.code = code + d;
    st.note = '';
    if (st.expected && len + 1 === st.expected) submit(st.code);
  };
  const erase = () => { st.code = code.slice(0, -1); st.note = ''; };
  const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '←', '0', 'OK'];
  KEYS.forEach((label, i) => {
    const r = rect(pad.x + (i % 3) * (kw + 1), pad.y + Math.trunc(i / 3) * kh, kw, kh);
    if (label === '←') moBtn(ui, r, MO_ID.KEY + i, label, { off: !code }, { tap: erase });
    else if (label === 'OK')
      moBtn(ui, r, MO_ID.KEY + i, label, { tone: 'solid', accent: C.GREEN, off: !code }, { tap: enter });
    else { moBtn(ui, r, MO_ID.KEY + i, label, {}, { tap: add(label) }); ui.key(label, add(label)); }
  });
  ui.key('Backspace', erase);
  if (code) ui.key('Enter', enter);
  moBarRow(ui, pg.status,'Tap digits, or type the code.', '0-9 type  ↵ unlock');
}

// Wrap text into at most two lines of `w` cells. The first line breaks at
// the last space that fits. The rest goes on the second line whole.
function moWrap(text, w) {
  if (moLen(text) <= w) return [text];
  let cut = text.lastIndexOf(' ', w);
  if (cut <= 0) cut = w;
  return [text.slice(0, cut), text.slice(cut).replace(/^ +/, '')];
}

// Connecting: what the shell knows, and a way out. st.retry shows the
// wait before the next try. `fail` shows a refused sign-in on a red panel;
// st.long gives it a reason that wraps.
function moConnect(ui, app, fail) {
  const st = app.state, pg = moPage(ui), p = moHostOf(st), w = moParse(p.where);
  const area = rect(pg.body.x, pg.header.y, pg.body.w, pg.header.h + pg.body.h);
  const addr = `${w.user}@${w.host} port ${w.port}`;
  if (fail) {
    const reason = st.long ? 'the server offered no method the deck supports' : 'wrong password';
    // The value column is the panel less 4, less the one-cell label column and its gap.
    const lines = moWrap(`Sign in refused: ${reason}`, Math.min(area.w, 46) - 4 - 3);
    const rows = [
      ['✘', `No session on ${p.name}`, C.WHITE, C.RED],
      ['', ''],
      ...lines.map((l) => ['', l]),
      ['', addr],
    ];
    const box = center(area, Math.min(area.w, 46), rows.length + 5), card = cutTop(box, rows.length + 2);
    moInfo(ui, card, rows, C.RED);
    const f = moFlow(box, pg.m.gutter, 3);
    moFirst(ui, MO_ID.BTN + 1);
    moBtn(ui, moFlowNext(f, moBtnW('Back')), MO_ID.BTN, 'Back', {}, { tap: () => app.back() });
    moBtn(ui, moFlowNext(f, moBtnW('Try again')), MO_ID.BTN + 1, 'Try again', {},
      { tap: () => app.go('mo-connect') });
    moBtn(ui, moFlowNext(f, moBtnW('Edit host')), MO_ID.BTN + 2, 'Edit host',
      { tone: 'solid', accent: C.BLUE }, { tap: () => app.go('mo-edit') });
    moBarRow(ui, pg.status,'', '←→ move  ↵ choose');
    return;
  }
  const box = center(area, Math.min(area.w, 46), 10), card = cutTop(box, 7);
  moInfo(ui, card, [
    [String.fromCodePoint(spinnerGlyph(ui.frameNo + 2)),
      `${st.retry ? 'Retrying' : 'Connecting to'} ${p.name}`, C.WHITE, C.AMBER],
    ['', ''],
    ['✓', 'Wi-Fi  HomeAP', 0, C.GREEN],
    ['', addr],
    ['', st.retry ? 'Next try in 3 s' : 'Try 2, 14 s'],
  ], profAccent(p.name));
  moBtn(ui, rect(box.x, box.y, moBtnW('Cancel'), 3), MO_ID.BTN, 'Cancel', {}, { tap: () => app.back() });
  ui.hot(card, MO_ID.ROW, { tap: () => app.go('mo-menu') });
  moBarRow(ui, pg.status,'Mock: tap the panel to finish.', '↵ cancel');
}

const MO_FP = '3f9a1c7e5b02d84a6c19e7f30b5d28a41e6c7f902ab3d5c8e1f4067a9b2d3c58';

// The host-key pages. Both stack a text panel, the fingerprint panel and
// one row of buttons under a back header. `changed` is the pinned key
// that now differs: a red stripe, and Cancel holds the first focus.
function moHostKey(ui, app, changed) {
  const st = app.state, pg = moPage(ui), p = moHostOf(st), w = moParse(p.where), body = pg.body;
  moHeader(ui, pg.header, { title: changed ? 'Host key changed' : 'New host key',
    back: { tap: () => app.back() } });
  const lines = changed
    ? [`The key of ${w.host} differs from the pinned one.`,
       'Replace it only if you know the server got a new key.']
    : [`First connection to ${p.name},`, `${w.host} port ${w.port}.`,
       'Compare the fingerprint before you trust it.'];
  const card = cutTop(body, lines.length + 2);
  moSlab(ui, card, moLook(ui, 0, {}));
  if (changed) moStripe(ui, card, C.RED);
  lines.forEach((l, i) => ui.text(rect(card.x + 2, card.y + 1 + i, card.w - 4, 1), l,
    i === lines.length - 1 ? S.PANEL_MUTED : S.PANEL, LEFT));
  const fp = cutTop(body, 5);
  moSlab(ui, fp, moLook(ui, 0, {}));
  ui.pen(MO_LABEL);
  ui.text(rect(fp.x + 2, fp.y + 1, fp.w - 4, 1), 'SHA256', S.PANEL_MUTED, LEFT);
  ui.pen(C.WHITE);
  for (let i = 0; i < 2; i++) {
    const groups = [];
    for (let g = 0; g < 8; g++) groups.push(MO_FP.slice(i * 32 + g * 4, i * 32 + g * 4 + 4));
    ui.text(rect(fp.x + 2, fp.y + 2 + i, fp.w - 4, 1), groups.join(' '), S.PANEL | BOLD, LEFT);
  }
  ui.pen(C.DEFAULT);
  const f = moFlow(body, pg.m.gutter, 3);
  const cancel = () => moBtn(ui, moFlowNext(f, moBtnW('Cancel')), MO_ID.BTN, 'Cancel', {},
    { tap: () => app.back() });
  if (changed) {
    cancel();
    moBtn(ui, moFlowNext(f, moBtnW('Replace key')), MO_ID.BTN + 1, 'Replace key',
      { tone: 'tint', accent: C.RED }, { tap: () => {
        st.ask = { title: 'Replace the pinned key?', verb: 'Replace', accent: C.RED,
          lines: ['A changed key can mean an attack.', 'The old pin is lost.'],
          yes: () => app.go('mo-connect') };
        app.go('mo-confirm');
      } });
  } else {
    moBtn(ui, moFlowNext(f, moBtnW('Trust and connect')), MO_ID.BTN, 'Trust and connect',
      { tone: 'solid', accent: C.GREEN }, { tap: () => app.go('mo-connect') });
    moBtn(ui, moFlowNext(f, moBtnW('Cancel')), MO_ID.BTN + 1, 'Cancel', {}, { tap: () => app.back() });
  }
  moBarRow(ui, pg.status,'The bar at the top goes back.', '←→ move  ↵ choose  Esc cancel');
}

// A QR code on a white panel, as half blocks. `r` is qrW by qrH cells.
function moQr(ui, r, size, mod, caption) {
  const span = qrW(size), crows = qrH(size) - 1;
  ui.pen(C.WHITE);
  ui.fill(r, ' ', S.BAR);
  for (let cr = 0; cr < crows; cr++)
    for (let cc = 0; cc < span; cc++) {
      const my = 2 * cr - QR_ZONE;
      const g = qrGlyph(mod(cc - QR_ZONE, my), mod(cc - QR_ZONE, my + 1));
      if (g !== 0x20) ui.putch(r.x + cc, r.y + cr, g, S.BAR);
    }
  if (r.h > crows) ui.text(rect(r.x, r.y + crows, r.w, 1), caption, S.BAR, CENTER);
  ui.pen(C.DEFAULT);
}

// One state line on the slate: a mark and a text in one pen, wrapped to
// two lines. Returns the rows it took.
function moState(ui, r, mark, pen, text) {
  const lines = wrapLines(text, r.w - 4, 2).slice(0, Math.max(0, r.h));
  ui.pen(pen);
  if (lines.length) ui.text(rect(r.x + 2, r.y, 1, 1), mark, S.TEXT | BOLD, LEFT);
  lines.forEach((t, i) => ui.text(rect(r.x + 4, r.y + i, r.w - 4, 1), t, S.TEXT, LEFT));
  ui.pen(C.DEFAULT);
  return lines.length;
}

// The onboarding page: steps on a text panel, then a state. A QR code
// sits at the right if it fits. A state2 makes the state a striped panel.
// With no steps it is a result card. o: { steps, qr, mark, pen, state, state2 }.
function moOnboard(ui, body, o) {
  let left = rect(body.x, body.y, body.w, body.h);
  const qr = o.qr, qw = qr ? qrW(qr.size) : 0;
  const qh = qr ? qrRows(qr.size, body.w, body.h) : 0;
  if (qh) {
    moQr(ui, rect(body.x + body.w - qw, body.y, qw, qh), qr.size, qr.mod, qr.caption);
    left = rect(body.x, body.y, body.w - qw - 2, body.h);
  }
  if (!o.steps.length) {
    const w = Math.min(left.w, 46);
    const rows = [...wrapLines(o.state, w - 4 - 3, 2).map((t, i) => [i ? '' : o.mark, t, C.WHITE, i ? 0 : o.pen]),
      ['', ''], ['', o.state2]];
    moInfo(ui, center(left, w, rows.length + 2), rows, o.pen);
    return;
  }
  const lines = stepLines(o.steps, left.w - 4 - 3);
  moInfo(ui, cutTop(left, lines.length + 2),
    lines.map((l) => [l.label, l.text, l.pen, l.labelPen]), 0);
  cutTop(left, 1);
  if (!o.state2) {
    moState(ui, left, o.mark, o.pen, o.state);
    return;
  }
  const vw = left.w - 4 - 3;
  const rows = [...wrapLines(o.state, vw, 2).map((t, i) => [i ? '' : o.mark, t, C.WHITE, i ? 0 : o.pen]),
    ...wrapLines(o.state2, vw, 2).map((t) => ['', t])];
  moInfo(ui, cutTop(left, rows.length + 2), rows, o.pen);
}

const MO_KBDS = [['Keychron K2', 0], ['Logitech K380', 1], ['MX Keys Mini', 1],
  ['Apple Magic Keyboard', 0], ['Kinesis Adv360', 1], ['ZSA Moonlander', 0],
  ['HHKB Hybrid', 1], ['Ducky One 3', 0]];

// Pair a keyboard. st.n keyboards found, st.left seconds before the scan
// closes (shown in the last ten). Forget bonds sits at the bottom, so
// it stays put while the list changes.
function moPair(ui, app) {
  const st = app.state, pg = moPage(ui), body = pg.body, n = Math.min(st.n | 0, MO_KBDS.length);
  const g = grid(rect(body.x, body.y + 1, body.w, body.h - 5), moCols(body.w, 30, pg.m.gutter), 4, pg.m.gutter, 0);
  const per = gridCapacity(g), pages = pageCount(n, per);
  st.page = pageClamp(st.page | 0, n, per);
  const head = moPager(ui, pg.header, st.page, pages,
    { prev: { tap: () => st.page-- }, next: { tap: () => st.page++ } });
  moHeader(ui, head, { path: 'Keyboard', title: 'Pair keyboard', back: { tap: () => app.back() } });
  const first = st.page * per, shown = Math.max(0, Math.min(per, n - first));
  moFirst(ui, shown ? MO_ID.HOST + first : MO_ID.BTN);
  moState(ui, rect(body.x, body.y, body.w, 1),
    n ? String.fromCodePoint(G.CHECK) : String.fromCodePoint(spinnerGlyph(ui.frameNo + 2)),
    n ? C.GREEN : C.AMBER, n ? `${n} found. Tap yours.` : 'Scanning for keyboards');
  if (st.left) {
    ui.pen(C.AMBER);
    ui.text(rect(body.x, body.y, body.w - 2, 1), `closes in ${st.left} s`, S.TEXT, RIGHT);
    ui.pen(C.DEFAULT);
  }
  for (let i = 0; i < shown; i++) {
    const k = first + i;
    moPart(ui, gridCell(g, i), MO_ID.HOST + k, {
      title: MO_KBDS[k][0], detail: MO_KBDS[k][1] ? 'random address' : 'public address',
      stripe: C.GREEN }, { tap: () => app.back() });
  }
  ui.key('PageUp', () => { if (st.page > 0) st.page--; });
  ui.key('PageDown', () => { if (st.page < pages - 1) st.page++; });
  ui.text(rect(body.x + 2, body.y + body.h - 4, body.w - 2, 1), 'Put the keyboard in pairing mode.',
    S.MUTED, LEFT);
  moBtn(ui, rect(body.x, body.y + body.h - 3, moBtnW('Forget bonds'), 3), MO_ID.BTN, 'Forget bonds',
    { tone: 'tint', accent: C.RED }, { tap: () => {
      st.ask = { title: 'Forget every paired keyboard?', verb: 'Forget', accent: C.RED,
        lines: ['Every bond is deleted from the deck.', 'Pair again from this page.'] };
      app.go('mo-confirm');
    } });
  moBarRow(ui, pg.status,'', '');
}

const MO_QR_WIFI = fakeQr(33, 0x5EED0001), MO_QR_HOST = fakeQr(25, 0x5EED0002);

// Add a network from the phone. st.phase: 'wait', 'test', 'ok' or 'fail'.
function moWifi(ui, app) {
  const st = app.state, pg = moPage(ui), phase = st.phase || 'wait';
  moHeader(ui, pg.header, { path: 'Wi-Fi', title: 'Add network (phone)', back: { tap: () => app.back() } });
  const spin = String.fromCodePoint(spinnerGlyph(ui.frameNo + 2));
  const o = { steps: [
    { mark: '1', text: 'Get the "ESP SoftAP Provisioning" app' },
    { mark: '2', text: 'Join this Wi-Fi:', value: 'CYBERDECK-SIM', pen: C.GREEN },
    { mark: '3', text: 'Proof code:', value: '1A2B3C4D', pen: C.AMBER },
    { mark: '4', text: 'Pick your Wi-Fi and password in the app.' },
  ], qr: { size: 33, mod: MO_QR_WIFI, caption: 'scan with the app' } };
  if (phase === 'wait') Object.assign(o, { mark: spin, pen: C.GREEN, state: 'Waiting for the phone' });
  if (phase === 'test') Object.assign(o, { mark: spin, pen: C.AMBER, state: 'Testing the network' });
  if (phase === 'fail')
    Object.assign(o, { mark: String.fromCodePoint(G.CROSS), pen: C.RED, state: 'Wrong password, or the network was not found.',
      state2: 'Try again from the app. Back cancels.' });
  if (phase === 'ok')
    Object.assign(o, { steps: [], qr: null, mark: String.fromCodePoint(G.CHECK), pen: C.GREEN,
      state: 'Joined HomeAP. Saved.', state2: 'Back to HOME in 2 s' });
  moOnboard(ui, pg.body, o);
  moBarRow(ui, pg.status,'', '');
}

// Import hosts. st.mode 'phone' (the deck's own Wi-Fi) or 'pc' (the
// network). st.pop false: the proof code is off. st.saved, st.removed:
// the counts. st.err: the last rejection.
function moImport(ui, app) {
  const st = app.state, pg = moPage(ui), pc = st.mode === 'pc';
  moHeader(ui, pg.header, { path: 'Hosts', title: pc ? 'Import from PC' : 'Import from phone',
    back: { tap: () => app.back() } });
  const steps = pc ? [
    { mark: '1', text: 'Open on your PC:', value: 'http://192.168.1.158', pen: C.WHITE },
    st.pop === false
      ? { mark: '2', text: 'Proof code is off in this build. The page is open.', pen: C.AMBER }
      : { mark: '2', text: 'Proof code:', value: '1A2B3C4D', pen: C.AMBER },
    { mark: '3', text: 'Fill the form and save.' },
    { mark: '!', text: 'Plain HTTP. Use it on a network you trust.', pen: C.AMBER },
  ] : [
    { mark: '1', text: 'Join this Wi-Fi:', value: 'DECK-SETUP-SIM', pen: C.GREEN },
    { mark: '2', text: 'Wi-Fi password:', value: '1A2B3C4D', pen: C.AMBER },
    { mark: '3', text: 'Open in a browser:', value: 'http://192.168.4.1', pen: C.WHITE },
    { mark: '4', text: 'Fill the form and save. Repeat for more.' },
  ];
  const o = { steps, qr: { size: 25, mod: MO_QR_HOST, caption: pc ? 'scan to open' : 'scan to join' } };
  const saved = st.saved | 0, removed = st.removed | 0, name = st.name || 'nas';
  const tick = String.fromCodePoint(G.CHECK);
  if (st.err) Object.assign(o, { mark: String.fromCodePoint(G.CROSS), pen: C.RED, state: `Rejected: ${st.err}` });
  else if (saved && removed)
    Object.assign(o, { mark: tick, pen: C.GREEN, state: `Last: ${name} (${saved} saved, ${removed} removed)` });
  else if (removed) Object.assign(o, { mark: tick, pen: C.GREEN, state: `Removed ${name} (${removed} removed)` });
  else if (saved) Object.assign(o, { mark: tick, pen: C.GREEN, state: `Imported ${name} (${saved} saved)` });
  else Object.assign(o, { mark: String.fromCodePoint(spinnerGlyph(ui.frameNo + 2)), pen: C.GREEN,
    state: 'Waiting for a browser' });
  moOnboard(ui, pg.body, o);
  moBarRow(ui, pg.status,'', '');
}

// Session menu: a slate sheet centred over the dimmed terminal, above the
// status row. Its parts are the same parts, because the sheet is screen
// background. Short grids drop the info lines, then the gaps.
function moMenu(ui, app) {
  const st = app.state, p = moHostOf(st);
  ui.term(TERM_LINES, MO_TERM_FG, 0);
  ui.dim();
  const area = rect(0, 0, ui.cols, ui.rows > 1 ? ui.rows - 1 : ui.rows);
  const info = area.h >= 18, gaps = area.h >= 15, g = gaps ? 1 : 0;
  const sheet = center(area, ui.cols >= 32 ? 30 : ui.cols, info ? 18 : gaps ? 15 : 11);
  ui.fill(sheet, ' ', S.TEXT);
  const inn = inset(sheet, 1, 1);
  moHeader(ui, cutTop(inn, 3), { path: 'Session', title: p.name, back: { tap: () => app.back() } });
  moPart(ui, cutTop(inn, 3), MO_ID.BTN, { title: 'Reset terminal' }, { tap: () => app.back() });
  moPart(ui, cutTop(inn, 3), MO_ID.BTN + 1, { title: 'Setup', right: '›' }, { tap: () => app.go('mo-setup') });
  cutTop(inn, g);
  moBtn(ui, cutTop(inn, 3), MO_ID.BTN + 2, 'Disconnect', { tone: 'tint', accent: C.RED },
    { tap: () => app.home() });
  if (!info) return;
  cutTop(inn, g);
  const txt = inset(inn, 1, 0), lw = 9;
  [['Uptime', '2 h 14 min'], ['Session', '14 min']].forEach(([label, value], i) => {
    const y = inn.y + i;
    ui.pen(MO_LABEL);
    ui.text(rect(txt.x, y, lw, 1), label, S.TEXT, LEFT);
    ui.pen(C.DEFAULT);
    moUnits(ui, rect(txt.x + lw, y, txt.w - lw, 1), value, S.TEXT, S.MUTED);
  });
}

// In-session toast: a panel-tone box at the top right of the terminal.
function moSession(ui, app) {
  const t = app.state.toast || { text: 'session dropped - reconnecting', tone: NOTE.PLAIN };
  ui.term(TERM_LINES, MO_TERM_FG, 0);
  toast(ui, rect(0, 0, ui.cols - 1, 1), t.text, t.tone);
}

// Block, box and mark glyphs the look draws with, at this font. Each
// shows on the slate and on a panel. It checks the font, not a screen.
function moGlyphs(ui) {
  const RANGES = [[0x2580, 0x259F], [0x2500, 0x251F], [0x25A0, 0x25CF],
    [0x2190, 0x2195], [0x2713, 0x2718], [0x2039, 0x203A], [0x2026, 0x2026]];
  let x = 1, y = 0;
  ui.pen(C.DEFAULT);
  ui.fill(rect(0, 0, ui.cols, ui.rows), ' ', S.TEXT);
  ui.pen(C.CYAN);
  for (const [from, to] of RANGES) {
    for (let cp = from; cp <= to && y < ui.rows; cp++) {
      ui.putch(x, y, cp, S.TEXT);
      ui.putch(x + 1, y, cp, S.PANEL);
      x += 3;
      if (x + 3 > ui.cols) { x = 1; y++; }
    }
    if (x !== 1) { x = 1; y++; }
  }
  ui.pen(C.DEFAULT);
}

// A host form in a known state, for a gate frame. `more` adds state: the
// caret, or the stored keys.
const moFormState = (over, edit = -1, more = {}) => () => {
  const base = { name: 'homelab', user: 'root', host: '192.168.1.10', port: '22',
                 pass: 'hunter22', auth: 0, key: '', show: false };
  return { host: 0, formFor: 0, form: { ...base, ...over }, formWas: JSON.stringify(base),
           orig: 'homelab', edit, ...more };
};

const MO_LONG_HOST = 'edge-gateway-03.eu-west-2.compute.internal.example-corp.co.uk';

// The frames the C kit must match cell for cell. kitcheck.mjs draws each
// frame and compares it with the lab screen of the same name.
// `state` makes the sketch state. `pose` is the key focus or the held finger.
export const MO_GATE = [
  { name: 'mo-parts', sketch: 'mo-parts' },
  { name: 'mo-home', sketch: 'mo-home' },
  { name: 'mo-home-keys', sketch: 'mo-home', pose: { focus: MO_ID.HOST + 1 } },
  { name: 'mo-home-down', sketch: 'mo-home', pose: { down: MO_ID.HOST + 2 } },
  { name: 'mo-home-32', sketch: 'mo-home', state: () => ({ hosts: PROFILES32, page: 1 }) },
  { name: 'mo-home-32-last', sketch: 'mo-home',
    state: () => ({ hosts: PROFILES32, page: 1, lastRow: true }) },
  { name: 'mo-home-32-pair', sketch: 'mo-home',
    state: () => ({ hosts: PROFILES32, page: 0, pair: true }) },
  { name: 'mo-host', sketch: 'mo-host' },
  { name: 'mo-host-key', sketch: 'mo-host', state: () => ({ host: 3 }), pose: { focus: MO_ID.BTN + 2 } },
  { name: 'mo-confirm', sketch: 'mo-confirm' },
  { name: 'mo-confirm-ask', sketch: 'mo-confirm', pose: { focus: MO_ID.BTN + 1 },
    state: () => ({ ask: { title: 'Discard the changes?', verb: 'Discard', accent: C.AMBER,
                           lines: ['This host keeps its old values.', ''] } }) },
  { name: 'mo-edit', sketch: 'mo-edit' },
  { name: 'mo-edit-new', sketch: 'mo-edit', state: () => ({ host: -1 }) },
  { name: 'mo-edit-typing', sketch: 'mo-edit', state: moFormState({ host: '192.168.1.1', show: true }, 2) },
  { name: 'mo-edit-bad', sketch: 'mo-edit', state: moFormState({ name: '', port: '70000' }) },
  { name: 'mo-edit-key', sketch: 'mo-edit', state: moFormState({ auth: 1, key: MO_KEYS[0].id }),
    pose: { focus: MO_ID.OPT + 5 } },
  { name: 'mo-edit-nokey', sketch: 'mo-edit', state: moFormState({ auth: 1 }, -1, { keys: [] }) },
  { name: 'mo-edit-caret', sketch: 'mo-edit', state: moFormState({ host: MO_LONG_HOST }, 2, { caret: 24 }) },
  { name: 'mo-edit-secret', sketch: 'mo-edit', state: moFormState({}, 4, { caret: 3 }) },
  { name: 'mo-status-busy', sketch: 'mo-home', state: () => ({ bar: 'busy' }) },
  { name: 'mo-status-busy-blink', sketch: 'mo-home', state: () => ({ bar: 'busy-blink' }) },
  { name: 'mo-status-alert', sketch: 'mo-home', state: () => ({ bar: 'alert' }) },
  { name: 'mo-status-off', sketch: 'mo-home', state: () => ({ bar: 'off' }) },
  { name: 'mo-status-locks-none', sketch: 'mo-home', state: () => ({ bar: 'locks-none' }) },
  { name: 'mo-status-locks-both', sketch: 'mo-home', state: () => ({ bar: 'locks-both' }) },
  { name: 'mo-status-note', sketch: 'mo-home', state: () => ({ bar: 'note' }) },
  { name: 'mo-status-note-good', sketch: 'mo-home', state: () => ({ bar: 'note-good' }) },
  { name: 'mo-status-note-alert', sketch: 'mo-home', state: () => ({ bar: 'note-alert' }) },
  { name: 'mo-status-hint', sketch: 'mo-home', state: () => ({ bar: 'hint' }) },
  { name: 'mo-status-net-up', sketch: 'mo-home', state: () => ({ bar: 'net-up' }) },
  { name: 'mo-status-kbd-up', sketch: 'mo-home', state: () => ({ bar: 'kbd-up' }) },
  { name: 'mo-status-net-lost', sketch: 'mo-home', state: () => ({ bar: 'net-lost' }) },
  { name: 'mo-status-collapse', sketch: 'mo-home', state: () => ({ bar: 'collapse' }) },
  { name: 'mo-status-up-note', sketch: 'mo-home', state: () => ({ bar: 'up-note' }) },
  { name: 'mo-session-toast', sketch: 'mo-session' },
  { name: 'mo-session-toast-ok', sketch: 'mo-session',
    state: () => ({ toast: { text: 'homelab - F12 or long-press for menu', tone: NOTE.GOOD } }) },
  { name: 'mo-glyphs', sketch: 'mo-glyphs' },
  { name: 'mo-setup', sketch: 'mo-setup', pose: { focus: MO_ID.HUB + 3 } },
  { name: 'mo-section-wifi', sketch: 'mo-section', state: () => ({ cat: 1 }) },
  { name: 'mo-section-wifi-down', sketch: 'mo-section', state: () => ({ cat: 1, wifi: WIFI_DOWN }) },
  { name: 'mo-section-wifi-full', sketch: 'mo-section',
    state: () => ({ cat: 1, wifi: WIFI_FULL, spage: 1, tileFocus: true }) },
  { name: 'mo-net', sketch: 'mo-net', state: () => ({ net: 0 }) },
  { name: 'mo-net-locked', sketch: 'mo-net', state: () => ({ net: 1 }), pose: { focus: MO_ID.BTN + 2 } },
  { name: 'mo-net-last', sketch: 'mo-net', state: () => ({ net: 2 }), pose: { focus: MO_ID.BTN } },
  { name: 'mo-confirm-forget-net', sketch: 'mo-confirm', pose: { focus: MO_ID.BTN },
    state: () => ({ ask: { title: 'Forget HomeAP?', verb: 'Forget', accent: C.RED,
                           lines: ['The deck leaves this network.', ''] } }) },
  { name: 'mo-section-fx', sketch: 'mo-section', state: () => ({ cat: 3 }) },
  { name: 'mo-section-motion', sketch: 'mo-section', state: () => ({ cat: 4 }), pose: { down: MO_ID.OPT + 9 } },
  { name: 'mo-section-system', sketch: 'mo-section', state: () => ({ cat: 5 }), pose: { focus: MO_ID.OPT + 16 } },
  { name: 'mo-section-danger', sketch: 'mo-section', state: () => ({ cat: 5 }), pose: { focus: MO_ID.OPT + 32 } },
  { name: 'mo-section-keystore', sketch: 'mo-section', state: () => ({ cat: 6 }), pose: { focus: MO_ID.OPT + 16 } },
  { name: 'mo-section-hosts', sketch: 'mo-section', state: () => ({ cat: 0 }) },
  { name: 'mo-section-hosts-few', sketch: 'mo-section',
    state: () => ({ cat: 0, hosts: PROFILES.slice(0, 5) }) },
  { name: 'mo-section-hosts-32', sketch: 'mo-section',
    state: () => ({ cat: 0, hosts: PROFILES32, max: 40, spage: 1, tileFocus: true }) },
  { name: 'mo-section-hosts-live', sketch: 'mo-section', state: () => ({ cat: 0, live: true }) },
  { name: 'mo-section-hosts-empty', sketch: 'mo-section', state: () => ({ cat: 0, hosts: [] }) },
  { name: 'mo-list', sketch: 'mo-list' },
  { name: 'mo-list-2', sketch: 'mo-list', state: () => ({ lpage: 1, net: 12 }), pose: { focus: MO_ID.ROW + 12 } },
  { name: 'mo-unlock', sketch: 'mo-unlock' },
  { name: 'mo-unlock-code', sketch: 'mo-unlock', state: () => ({ code: '12' }), pose: { down: MO_ID.KEY + 4 } },
  { name: 'mo-unlock-known', sketch: 'mo-unlock', state: () => ({ code: '12', expected: 6 }) },
  { name: 'mo-unlock-wrong', sketch: 'mo-unlock', state: () => ({ note: '✘ wrong code', wait: 12 }) },
  { name: 'mo-unlock-wait', sketch: 'mo-unlock', state: () => ({ wait: 12 }) },
  { name: 'mo-unlock-check', sketch: 'mo-unlock', state: () => ({ deriving: true }) },
  { name: 'mo-unlock-new', sketch: 'mo-unlock',
    state: () => ({ flow: true, phase: 'new', creating: true, code: 'ab7', reveal: '7' }) },
  { name: 'mo-unlock-mismatch', sketch: 'mo-unlock',
    state: () => ({ flow: true, phase: 'new', note: 'The codes do not match' }) },
  { name: 'mo-unlock-resume', sketch: 'mo-unlock',
    state: () => ({ flow: true, resume: true, code: '1', expected: 4 }) },
  { name: 'mo-connect', sketch: 'mo-connect' },
  { name: 'mo-connect-retry', sketch: 'mo-connect', state: () => ({ retry: true }) },
  { name: 'mo-fail', sketch: 'mo-fail', pose: { focus: MO_ID.BTN + 1 } },
  { name: 'mo-fail-long', sketch: 'mo-fail', state: () => ({ long: true }), pose: { focus: MO_ID.BTN + 1 } },
  { name: 'mo-hostkey', sketch: 'mo-hostkey', pose: { focus: MO_ID.BTN } },
  { name: 'mo-hostkey-changed', sketch: 'mo-hostkey-changed', pose: { focus: MO_ID.BTN } },
  { name: 'mo-confirm-replace', sketch: 'mo-confirm', pose: { focus: MO_ID.BTN },
    state: () => ({ ask: { title: 'Replace the pinned key?', verb: 'Replace', accent: C.RED,
                           lines: ['A changed key can mean an attack.', 'The old pin is lost.'] } }) },
  { name: 'mo-menu', sketch: 'mo-menu', pose: { focus: MO_ID.BTN } },
  { name: 'mo-pair-scan', sketch: 'mo-pair' },
  { name: 'mo-pair', sketch: 'mo-pair', state: () => ({ n: 2 }), pose: { focus: MO_ID.HOST } },
  { name: 'mo-pair-closing', sketch: 'mo-pair', state: () => ({ n: 1, left: 9 }), pose: { focus: MO_ID.BTN } },
  { name: 'mo-pair-many', sketch: 'mo-pair', state: () => ({ n: 8 }), pose: { down: MO_ID.HOST + 1 } },
  { name: 'mo-pair-many-2', sketch: 'mo-pair', state: () => ({ n: 8, page: 1 }), pose: { focus: MO_ID.HOST + 6 } },
  { name: 'mo-confirm-forget', sketch: 'mo-confirm', pose: { focus: MO_ID.BTN },
    state: () => ({ ask: { title: 'Forget every paired keyboard?', verb: 'Forget', accent: C.RED,
                           lines: ['Every bond is deleted from the deck.', 'Pair again from this page.'] } }) },
  { name: 'mo-wifiprov', sketch: 'mo-wifi' },
  { name: 'mo-wifiprov-test', sketch: 'mo-wifi', state: () => ({ phase: 'test' }) },
  { name: 'mo-wifiprov-fail', sketch: 'mo-wifi', state: () => ({ phase: 'fail' }) },
  { name: 'mo-wifiprov-ok', sketch: 'mo-wifi', state: () => ({ phase: 'ok' }) },
  { name: 'mo-import-phone', sketch: 'mo-import' },
  { name: 'mo-import-pc', sketch: 'mo-import', state: () => ({ mode: 'pc' }) },
  { name: 'mo-import-pc-off', sketch: 'mo-import', state: () => ({ mode: 'pc', pop: false }) },
  { name: 'mo-import-saved', sketch: 'mo-import', state: () => ({ mode: 'pc', saved: 2, name: 'nas' }) },
  { name: 'mo-import-both', sketch: 'mo-import', state: () => ({ saved: 2, removed: 1, name: 'homelab' }) },
  { name: 'mo-import-removed', sketch: 'mo-import', state: () => ({ removed: 1, name: 'old-box' }) },
  { name: 'mo-import-rejected', sketch: 'mo-import',
    state: () => ({ mode: 'pc', err: 'a host needs an address' }) },
];

export const MO_SKETCHES = [
  { name: 'mo-home', title: 'Hosts: tap connects, hold for options', theme: 'slate', draw: moHome },
  { name: 'mo-host', title: 'Host page (hold on a tile)', theme: 'slate', draw: moHost },
  { name: 'mo-confirm', title: 'Question page', theme: 'slate', draw: moConfirm },
  { name: 'mo-edit', title: 'Host form', theme: 'slate', draw: moEdit },
  { name: 'mo-setup', title: 'Setup hub', theme: 'slate', draw: moSetup },
  { name: 'mo-section', title: 'Setup section: switches and choices', theme: 'slate', draw: moSection },
  { name: 'mo-net', title: 'Saved network page', theme: 'slate', draw: moNet },
  { name: 'mo-list', title: 'Paged list with pager and rail', theme: 'slate', draw: moList },
  { name: 'mo-unlock', title: 'Access code pad', theme: 'slate', draw: moUnlock },
  { name: 'mo-connect', title: 'Connecting', theme: 'slate', draw: (u, a) => moConnect(u, a, false) },
  { name: 'mo-fail', title: 'Connect failed', theme: 'slate', draw: (u, a) => moConnect(u, a, true) },
  { name: 'mo-hostkey', title: 'Host key, first contact', theme: 'slate', draw: (u, a) => moHostKey(u, a, false) },
  { name: 'mo-hostkey-changed', title: 'Host key changed', theme: 'slate', draw: (u, a) => moHostKey(u, a, true) },
  { name: 'mo-pair', title: 'Pair a keyboard', theme: 'slate', draw: moPair },
  { name: 'mo-wifi', title: 'Add a network from the phone', theme: 'slate', draw: moWifi },
  { name: 'mo-import', title: 'Import hosts', theme: 'slate', draw: moImport },
  { name: 'mo-menu', title: 'Session menu sheet', theme: 'slate', draw: moMenu },
  { name: 'mo-session', title: 'Session toast', theme: 'slate', draw: moSession },
  { name: 'mo-glyphs', title: 'Block, box and mark glyphs', theme: 'slate', draw: moGlyphs },
  { name: 'mo-parts', title: 'Parts in every state', theme: 'slate', draw: moParts },
];
