// sketches.js — redesign proposals. Same shape as baseline.js entries:
// { name, title, draw(ui, app), pose? }. Build on kit.js. A helper that
// a sketch adds beyond the kit is a candidate for components/ui.
//
// Round 1: a DOS-utility look. Framed windows, list and description
// panes, raised buttons with a shadow, gray dialogs, a hint line.
// Every colour is an entry of the deck's own style table.

import {
  S, C, G, BOLD, LEFT, CENTER, RIGHT, WIDE, COMPACT,
  rect, cutTop, cutBottom, cutLeft, cutRight, inset, center,
} from './kit.js';
import { PROFILES, TERM_LINES } from './baseline.js';
import { MO_SKETCHES } from './sketches-mo.js';

// A surface: the style and pen a window paints with. `edge` is the same
// colour as ink on the screen background. `inv` says a black half-cell
// on this surface needs that edge style. `key` is the accent of a
// neutral key: gray on a colour, white on gray.
const NU = {
  blue:  { st: S.WELL,  pen: C.BLUE,    edge: S.WELL_EDGE,  sub: S.WELL,        inv: true,  key: C.DEFAULT },
  slate: { st: S.PANEL, pen: C.DEFAULT, edge: S.PANEL_EDGE, sub: S.PANEL_MUTED, inv: true,  key: C.DEFAULT,
           frame: S.PANEL_MUTED },
  gray:  { st: S.BAR,   pen: C.DEFAULT, edge: S.BAR_EDGE,   sub: S.BAR,         inv: false, key: C.WHITE },
  red:   { st: S.BAR,   pen: C.RED,     edge: S.BAR_EDGE,   sub: S.BAR,         inv: true,  key: C.DEFAULT },
};

const NU_ID = { ROW: 1000, CAT: 1100, OPT: 1200, BTN: 1400, KEY: 1500, FIELD: 1600 };

// Rows of one touch target: two cells at 12x24 and 10x20, three at 8x16.
const nuUnit = (ui) => (ui.metrics().cls === WIDE ? 3 : 2);

// Drop shadow on one cell. Text under it stays readable but dark.
// A block glyph under it would show as a bright shape, so it goes black.
function nuShade(ui, x, y) {
  if (x < 0 || y < 0 || x >= ui.cols || y >= ui.rows) return;
  const i = y * ui.cols + x, f = ui.frame, cp = f.cp[i];
  if (cp && !(cp >= 0x2580 && cp <= 0x259F)) { f.pal[i] = S.MUTED * 8 + C.DEFAULT; f.bold[i] = 0; }
  else { ui.pen(C.DEFAULT); ui.putch(x, y, ' ', S.TEXT); }
}

// A caption centred on a span of a window's frame row.
function nuCaption(ui, x, y, w, text, sf) {
  ui.pen(sf.pen);
  ui.text(rect(x, y, w, 1), ` ${text} `, sf.st | BOLD, CENTER);
  ui.pen(C.DEFAULT);
}

// A black half cell on a surface: the lower half, or the upper half.
function nuHalf(ui, sf, x, y, lower) {
  ui.pen(sf.pen);
  if (sf.inv) ui.putch(x, y, lower ? G.HALF_UP : G.HALF_DN, sf.edge);
  else ui.putch(x, y, lower ? G.HALF_DN : G.HALF_UP, sf.st);
}

// A double-framed window with a centred title. Returns its interior.
function nuWin(ui, r, title, sf, shadow = true) {
  if (shadow) {
    for (let y = r.y + 1; y <= r.y + r.h; y++) { nuShade(ui, r.x + r.w, y); nuShade(ui, r.x + r.w + 1, y); }
    for (let x = r.x + 2; x < r.x + r.w; x++) nuShade(ui, x, r.y + r.h);
  }
  const x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  ui.pen(sf.pen);
  ui.fill(r, ' ', sf.st);
  const fr = sf.frame || sf.st;
  for (let x = r.x + 1; x < x1; x++) { ui.putch(x, r.y, 0x2550, fr); ui.putch(x, y1, 0x2550, fr); }
  for (let y = r.y + 1; y < y1; y++) { ui.putch(r.x, y, 0x2551, fr); ui.putch(x1, y, 0x2551, fr); }
  ui.putch(r.x, r.y, 0x2554, fr); ui.putch(x1, r.y, 0x2557, fr);
  ui.putch(r.x, y1, 0x255A, fr); ui.putch(x1, y1, 0x255D, fr);
  if (title) ui.text(rect(r.x + 2, r.y, r.w - 4, 1), ` ${title} `, sf.st | BOLD, CENTER);
  ui.pen(C.DEFAULT);
  return inset(r, 1, 1);
}

// A vertical rule from the top frame to the bottom frame of a window.
function nuSplit(ui, win, x, sf) {
  ui.pen(sf.pen);
  const fr = sf.frame || sf.st;
  ui.putch(x, win.y, 0x2564, fr);
  for (let y = win.y + 1; y < win.y + win.h - 1; y++) ui.putch(x, y, 0x2502, fr);
  ui.putch(x, win.y + win.h - 1, 0x2567, fr);
  ui.pen(C.DEFAULT);
}

// A one-row rule with a caption: `── Deck ─────`.
function nuRule(ui, r, caption, sf) {
  ui.pen(sf.pen);
  for (let x = r.x; x < r.x + r.w; x++) ui.putch(x, r.y, 0x2500, sf.sub);
  if (caption) ui.text(rect(r.x + 2, r.y, r.w - 3, 1), ` ${caption} `, sf.st | BOLD, LEFT);
  ui.pen(C.DEFAULT);
}

// A raised button: a face with a black shadow to the right and below.
// Pressed, the face moves into its shadow. o: { off, lit }.
function nuBtn(ui, r, id, label, accent, sf, act, o = {}) {
  ui.hot(r, o.off ? 0 : id, act);
  const down = !o.off && ui.pressed(id), foc = ui.focused(id);
  const fw = r.w - 1, fh = Math.max(1, r.h - 1), x0 = r.x + (down ? 1 : 0);
  const st = o.off ? S.WELL : down || foc || o.lit ? S.FOCUS : S.BAR;
  const ty = r.y + ((fh - 1) >> 1);
  ui.pen(o.off ? C.DEFAULT : accent);
  ui.fill(rect(x0, r.y, fw, fh), ' ', st);
  ui.text(rect(x0 + 1, ty, fw - 2, 1), label, st | BOLD, CENTER);
  if (foc) { ui.putch(x0, ty, G.ARROW, st); ui.putch(x0 + fw - 1, ty, G.POINT_L, st); }
  if (!down) {
    nuHalf(ui, sf, r.x + fw, r.y, true);
    ui.pen(C.DEFAULT);
    for (let y = 1; y < fh; y++) ui.putch(r.x + fw, r.y + y, ' ', S.TEXT);
    if (r.h >= 2) for (let x = r.x + 1; x <= r.x + fw; x++) nuHalf(ui, sf, x, r.y + fh, false);
  }
  ui.pen(C.DEFAULT);
}

// Buttons side by side in one band, equal widths.
function nuBtnRow(ui, r, sf, btns) {
  const w = Math.trunc(r.w / btns.length);
  btns.forEach((b, i) => {
    if (b) nuBtn(ui, rect(r.x + i * w, r.y, w, r.h), b.id, b.label, b.accent | 0, sf, b.act, b);
  });
}

// One list row: a bold title and a detail line. The chosen row is a bar.
function nuRow(ui, r, id, title, detail, sel, sf, act) {
  ui.hot(r, id, act);
  const down = ui.pressed(id);
  let st = sf.st, sub = sf.sub, pen = sf.pen;
  if (down) { st = sub = S.BAR; pen = C.WHITE; }
  else if (sel) { st = sub = S.FOCUS; pen = C.CYAN; }
  ui.pen(pen);
  if (sel || down) ui.fill(r, ' ', st);
  if (ui.focused(id)) ui.putch(r.x, r.y, G.ARROW, st | BOLD);
  ui.text(rect(r.x + 2, r.y, r.w - 3, 1), title, st | BOLD, LEFT);
  if (detail && r.h >= 2) ui.text(rect(r.x + 2, r.y + 1, r.w - 3, 1), detail, sub, LEFT);
  ui.pen(C.DEFAULT);
}

// A check row: `[√] Scanlines`. The whole band toggles it.
function nuCheck(ui, r, id, label, on, sf, act) {
  ui.hot(r, id, act);
  const st = ui.pressed(id) ? S.FOCUS : sf.st;
  ui.pen(ui.pressed(id) ? C.CYAN : sf.pen);
  if (ui.pressed(id)) ui.fill(r, ' ', st);
  if (ui.focused(id)) ui.putch(r.x, r.y, G.ARROW, st | BOLD);
  ui.puts(r.x + 1, r.y, on ? '[√]' : '[ ]', st | BOLD);
  ui.text(rect(r.x + 5, r.y, r.w - 10, 1), label, st, LEFT);
  ui.text(rect(r.x + r.w - 4, r.y, 3, 1), on ? 'on' : 'off', st | BOLD, RIGHT);
  ui.pen(C.DEFAULT);
}

// One choice out of a few: a caption row, then a band of keys. The
// chosen key stays pushed in, as on a car radio.
function nuRadio(ui, r, idBase, caption, labels, value, sf, pick) {
  ui.pen(sf.pen);
  ui.text(rect(r.x + 1, r.y, r.w - 2, 1), caption, sf.st, LEFT);
  const band = rect(r.x, r.y + 1, r.w, r.h - 1), w = Math.trunc(band.w / labels.length);
  labels.forEach((l, i) => {
    const k = rect(band.x + i * w, band.y, w, band.h), id = idBase + i;
    if (i !== value) { nuBtn(ui, k, id, l, sf.key, sf, { tap: () => pick(i) }); return; }
    ui.hot(k, id, null);
    ui.pen(C.GREEN);
    ui.fill(rect(k.x + 1, k.y, k.w - 1, Math.max(1, k.h - 1)), ' ', S.FOCUS);
    const ky = k.y + ((Math.max(1, k.h - 1) - 1) >> 1);
    ui.text(rect(k.x + 2, ky, k.w - 3, 1), l, S.FOCUS | BOLD, CENTER);
    if (ui.focused(id)) { ui.putch(k.x + 1, ky, G.ARROW, S.FOCUS); ui.putch(k.x + k.w - 1, ky, G.POINT_L, S.FOCUS); }
  });
  ui.pen(C.DEFAULT);
}

// A form field: a label, then a black well. Editing lights the well.
function nuField(ui, r, id, label, value, o, sf, act) {
  ui.hot(r, id, act);
  const lw = 7, well = rect(r.x + lw, r.y, r.w - lw - 1, 1);
  ui.pen(sf.pen);
  ui.text(rect(r.x + 1, r.y, lw - 1, 1), label, sf.st, LEFT);
  const shown = o.mask ? '*'.repeat(value.length) : value;
  const st = o.edit || ui.pressed(id) ? S.FOCUS : S.TEXT;
  ui.pen(o.edit || ui.pressed(id) ? C.CYAN : C.WHITE);
  ui.fill(well, ' ', st);
  ui.text(rect(well.x + 1, well.y, well.w - 2, 1), shown, st, LEFT);
  if (o.edit) {
    ui.pen(C.WHITE);
    ui.putch(Math.min(well.x + 1 + shown.length, well.x + well.w - 1), well.y, ' ', S.BAR);
  }
  ui.pen(C.DEFAULT);
}

// The hint line: chips, a hint for the screen, the clock.
function nuStatus(ui, r, hint) {
  ui.pen(C.DEFAULT);
  ui.fill(r, ' ', S.BAR);
  let x = r.x + 1;
  ui.puts(x, r.y, 'NET', S.BAR | BOLD);
  x += 4;
  for (const cp of [0x2582, 0x2584, 0x2586, 0x20]) ui.putch(x++, r.y, cp, S.BAR);
  x += 1;
  ui.puts(x, r.y, 'KBD', S.BAR | BOLD);
  x += 4;
  ui.pen(C.AMBER);
  ui.putch(x++, r.y, 'C', S.BAR | BOLD);
  ui.pen(C.DEFAULT);
  ui.putch(x++, r.y, 'N', S.BAR);
  ui.putch(++x, r.y, 0x2502, S.BAR);
  ui.text(rect(x + 2, r.y, r.x + r.w - 9 - x, 1), hint || '', S.BAR, LEFT);
  ui.puts(r.x + r.w - 7, r.y, '14:07', S.BAR | BOLD);
}

function nuDesktop(ui) {
  ui.pen(C.BLUE);
  ui.fill(rect(0, 0, ui.cols, ui.rows), 0x2591, S.MUTED);
  ui.pen(C.DEFAULT);
}

// "root@192.168.1.10:22  key" -> its parts.
function nuParse(where) {
  const key = /\bkey$/.test(where), w = where.replace(/\s+key$/, '');
  const at = w.indexOf('@'), colon = w.lastIndexOf(':');
  return { user: w.slice(0, at), host: w.slice(at + 1, colon), port: w.slice(colon + 1), key };
}

// HOME as a host manager. The list sits on the left. The chosen host
// and its actions sit on the right. All eight hosts fit every grid.
function nuHome(ui, app, sf) {
  const st = app.state, m = ui.metrics(), u = nuUnit(ui);
  if (st.sel == null) st.sel = 0;
  const full = rect(0, 0, ui.cols, ui.rows), status = cutBottom(full, 1);
  const win = { ...full }, inn = nuWin(ui, full, null, sf, false);
  const lw = m.cls === COMPACT ? 25 : m.cls === WIDE ? 36 : 30;
  const left = cutLeft(inn, lw);
  nuSplit(ui, win, cutLeft(inn, 1).x, sf);
  nuCaption(ui, left.x, win.y, left.w, 'Hosts', sf);
  nuCaption(ui, inn.x, win.y, inn.w, 'Description', sf);
  const right = inset(inn, 1, 0);

  PROFILES.forEach((p, i) => {
    const q = nuParse(p.where);
    nuRow(ui, cutTop(left, u), NU_ID.ROW + i, p.name, `${q.user}@${q.host}`, i === st.sel, sf,
          { tap: () => (st.sel === i ? app.go('nu-connect') : (st.sel = i)),
            hold: () => { st.sel = i; app.go('nu-edit'); } });
  });

  const p = PROFILES[st.sel], q = nuParse(p.where);
  const deck = cutBottom(right, u);
  nuRule(ui, cutBottom(right, 1), 'Deck', sf);
  if (m.cls !== COMPACT) cutBottom(right, 1);
  const acts2 = cutBottom(right, u), acts1 = cutBottom(right, u);

  cutTop(right, 1);
  ui.pen(sf.pen);
  ui.text(cutTop(right, 1), p.name, sf.st | BOLD, LEFT);
  const facts = [['Login', `${q.user}@${q.host}`], ['Port', q.port],
                 ['Auth', q.key ? 'key  ed25519' : 'password'], ['Seen', '2 hours ago']];
  if (m.cls !== COMPACT) cutTop(right, 1);
  for (const [k, v] of facts) {
    const line = cutTop(right, 1);
    ui.pen(sf.pen);
    ui.text(rect(line.x, line.y, 6, 1), k, sf.sub, LEFT);
    ui.text(rect(line.x + 7, line.y, line.w - 7, 1), v, sf.st, LEFT);
  }
  nuBtnRow(ui, acts1, sf, [
    { id: NU_ID.BTN, label: 'Connect', accent: C.GREEN, act: { tap: () => app.go('nu-connect') } }]);
  nuBtnRow(ui, acts2, sf, [
    { id: NU_ID.BTN + 1, label: 'Edit', accent: C.CYAN, act: { tap: () => app.go('nu-edit') } },
    { id: NU_ID.BTN + 2, label: 'Delete', accent: C.RED, act: { tap: () => app.go('nu-confirm') } }]);
  nuBtnRow(ui, deck, sf, [
    { id: NU_ID.BTN + 3, label: 'New host', accent: sf.key, act: { tap: () => app.go('nu-edit') } },
    { id: NU_ID.BTN + 4, label: 'Setup', accent: sf.key, act: { tap: () => app.go('nu-setup') } },
    { id: NU_ID.BTN + 5, label: 'Lock', accent: C.AMBER, act: { tap: () => app.go('nu-unlock') } }]);
  nuStatus(ui, status, app.note || 'Tap a host. Tap it again to connect.');
}

const NU_SETUP = [
  { name: 'Hosts', note: '8 stored', items: [
    { t: 'act', label: 'Import (phone)' }, { t: 'act', label: 'Import (PC)' }] },
  { name: 'Wi-Fi', note: 'HomeAP  -55 dBm', items: [
    { t: 'act', label: 'Reconnect' }, { t: 'act', label: 'Add network' }] },
  { name: 'Keyboard', note: 'K380 connected', items: [
    { t: 'act', label: 'Pair keyboard' }, { t: 'act', label: 'Forget bonds', accent: C.RED }] },
  { name: 'CRT FX', note: 'scanlines, phosphor', items: [
    { t: 'check', key: 'scan', label: 'Scanlines', init: true },
    { t: 'check', key: 'pop', label: 'Bold pop', init: true },
    { t: 'radio', key: 'phos', label: 'Phosphor', set: ['color', 'green', 'amber'], init: 0 }] },
  { name: 'Motion FX', note: 'wobble, wipe, static', items: [
    { t: 'radio', key: 'wob', label: 'Wobble', set: ['off', 'subtle', 'medium', 'deep'], init: 2 },
    { t: 'radio', key: 'wipe', label: 'Wipe in', set: ['off', '0.3 s', '0.6 s'], init: 1 },
    { t: 'radio', key: 'coll', label: 'Collapse', set: ['off', '0.2 s', '0.5 s'], init: 1 },
    { t: 'radio', key: 'stat', label: 'Static', set: ['off', 'light', 'heavy'], init: 1 }] },
  { name: 'Font', note: '12x24  66x20', items: [
    { t: 'radio', key: 'font', label: 'Size (a change restarts the deck)',
      set: ['8x16', '10x20', '12x24'], init: 2 }] },
  { name: 'System', note: 'saver, reset', items: [
    { t: 'radio', key: 'saver', label: 'Saver and lock after, minutes',
      set: ['1', '3', '5', '10', '30'], init: 2 },
    { t: 'check', key: 'edge', label: 'Edge scroll', init: false },
    { t: 'act', label: 'Clear host keys', accent: C.RED },
    { t: 'act', label: 'Factory reset', accent: C.RED }] },
  { name: 'Keystore', note: 'unlocked', items: [
    { t: 'act', label: 'Lock deck', accent: C.AMBER }, { t: 'act', label: 'Change code' },
    { t: 'act', label: 'Remove code', accent: C.RED }] },
];

// Setup in one window: sections on the left, the chosen section's
// options on the right. One level deep, so one step back.
function nuSetup(ui, app, sf) {
  const st = app.state, m = ui.metrics(), u = nuUnit(ui);
  if (st.cat == null) st.cat = 3;
  st.opt = st.opt || {};
  const full = rect(0, 0, ui.cols, ui.rows), status = cutBottom(full, 1);
  const win = { ...full }, inn = nuWin(ui, full, null, sf, false);
  const lw = m.cls === COMPACT ? 25 : m.cls === WIDE ? 36 : 30;
  const left = cutLeft(inn, lw);
  nuSplit(ui, win, cutLeft(inn, 1).x, sf);
  nuCaption(ui, left.x, win.y, left.w, 'Setup', sf);
  nuCaption(ui, inn.x, win.y, inn.w, NU_SETUP[st.cat].name, sf);
  const right = inset(inn, 1, 0);
  cutTop(right, 1);

  NU_SETUP.forEach((c, i) => nuRow(ui, cutTop(left, u), NU_ID.CAT + i, c.name, c.note,
                                   i === st.cat, sf, { tap: () => (st.cat = i) }));

  const back = cutBottom(right, u);
  nuBtnRow(ui, back, sf, [null, null,
    { id: NU_ID.BTN, label: '◄ Hosts', accent: C.CYAN, act: { tap: () => app.back() } }]);
  let id = NU_ID.OPT, pair = null;
  for (const it of NU_SETUP[st.cat].items) {
    const cur = it.key in st.opt ? st.opt[it.key] : it.init;
    if (it.t === 'check') {
      nuCheck(ui, cutTop(right, u), id++, it.label, cur, sf, { tap: () => (st.opt[it.key] = !cur) });
    } else if (it.t === 'radio') {
      nuRadio(ui, cutTop(right, u + 1), id, it.label, it.set, cur, sf, (v) => (st.opt[it.key] = v));
      id += it.set.length;
    } else {
      // Actions sit two to a band.
      if (!pair) pair = { band: cutTop(right, u), n: 0 };
      const w = Math.trunc(pair.band.w / 2);
      nuBtn(ui, rect(pair.band.x + pair.n * w, pair.band.y, w, u), id++, it.label,
            it.accent == null ? sf.key : it.accent, sf, { tap: () => (app.note = ` ${it.label}`) });
      if (++pair.n === 2) pair = null;
    }
  }
  nuStatus(ui, status, app.note || 'A change applies at once.');
}

// The host editor as a form. Text still needs the keyboard.
function nuEdit(ui, app, sf) {
  const st = app.state, u = nuUnit(ui);
  if (st.field == null) st.field = 1;
  if (st.auth == null) st.auth = 0;
  const full = rect(0, 0, ui.cols, ui.rows), status = cutBottom(full, 1);
  const inn = inset(nuWin(ui, full, 'Edit host', sf, false), 1, 0);
  const acts = cutBottom(inn, u);
  const F = [['Name', 'homelab'], ['Host', '192.168.1.'], ['Port', '22'], ['User', 'root']];
  F.forEach(([l, v], i) => nuField(ui, cutTop(inn, u), NU_ID.FIELD + i, l, v,
    { edit: st.field === i }, sf, { tap: () => (st.field = i) }));
  nuRadio(ui, cutTop(inn, u + 1), NU_ID.OPT, 'Sign in with', ['password', 'key'], st.auth, sf,
          (v) => (st.auth = v));
  nuField(ui, cutTop(inn, u), NU_ID.FIELD + 4, st.auth ? 'Key' : 'Pass',
          st.auth ? 'id_ed25519' : 'hunter2', { mask: !st.auth, edit: st.field === 4 }, sf,
          { tap: () => (st.field = 4) });
  nuBtnRow(ui, acts, sf, [
    { id: NU_ID.BTN, label: 'Save', accent: C.GREEN, act: { tap: () => app.back() } },
    null,
    { id: NU_ID.BTN + 1, label: 'Delete', accent: C.RED, act: { tap: () => app.go('nu-confirm') } },
    { id: NU_ID.BTN + 2, label: 'Cancel', accent: C.CYAN, act: { tap: () => app.back() } }]);
  nuStatus(ui, status, app.note || 'Tap a field, then type on the keyboard.');
}

// A gray dialog on top of whatever is on screen. Returns its interior.
function nuDialog(ui, w, h, title, sf = NU.gray) {
  ui.hots.length = 0;
  const r = center(rect(0, 0, ui.cols, ui.rows - 1), w, h);
  return inset(nuWin(ui, r, title, sf, true), 1, 0);
}

function nuConfirm(ui, app) {
  nuHome(ui, app, NU.blue);
  const u = nuUnit(ui), inn = nuDialog(ui, 40, 6 + u, 'Delete host', NU.gray);
  ui.pen(C.DEFAULT);
  cutTop(inn, 1);
  ui.text(cutTop(inn, 1), 'Delete "homelab" and its key?', S.BAR | BOLD, LEFT);
  ui.text(cutTop(inn, 1), 'The host key pin goes with it.', S.BAR, LEFT);
  nuBtnRow(ui, cutBottom(inn, u), NU.gray, [
    { id: NU_ID.BTN + 8, label: 'Delete', accent: C.RED, act: { tap: () => app.back() } },
    { id: NU_ID.BTN + 9, label: 'Keep', accent: C.CYAN, act: { tap: () => app.back() } }]);
}

function nuConnect(ui, app) {
  nuHome(ui, app, NU.blue);
  const u = nuUnit(ui), inn = nuDialog(ui, 44, 9 + u, 'Connecting to homelab', NU.gray);
  const steps = [['Wi-Fi link', 1], ['TCP 192.168.1.10:22', 1],
                 ['Host key SHA256:9fK2..Qm1c', 1], ['Signing in', 0]];
  ui.pen(C.DEFAULT);
  cutTop(inn, 1);
  for (const [t, done] of steps) {
    const line = cutTop(inn, 1);
    ui.putch(line.x, line.y, done ? 0x221A : G.ARROW, S.BAR | BOLD);
    ui.text(rect(line.x + 2, line.y, line.w - 2, 1), t, done ? S.BAR : S.BAR | BOLD, LEFT);
  }
  cutTop(inn, 1);
  const bar = cutTop(inn, 1), fill = Math.trunc(bar.w * 7 / 10);
  for (let x = 0; x < bar.w; x++) {
    ui.pen(x < fill ? C.GREEN : C.DEFAULT);
    ui.putch(bar.x + x, bar.y, ' ', x < fill ? S.BAR : S.WELL);
  }
  nuBtnRow(ui, cutBottom(inn, u), NU.gray, [null,
    { id: NU_ID.BTN + 8, label: 'Cancel', accent: C.CYAN, act: { tap: () => app.back() } }, null]);
}

// The menu over a live session: a small window, the terminal dimmed.
function nuMenu(ui, app) {
  ui.term(TERM_LINES, app.fg, app.bg);
  ui.dim();
  const u = nuUnit(ui);
  const items = [['Resume session', 'back to the terminal'], ['Reset terminal', 'clear a stuck screen'],
                 ['Disconnect', 'close the remote shell'], ['Setup', 'effects, font, keys']];
  const inn = nuDialog(ui, 34, items.length * u + 4, 'Session', NU.gray);
  items.forEach(([t, d], i) => nuRow(ui, cutTop(inn, u), NU_ID.ROW + i, t, d, false, NU.gray,
    { tap: () => (i === 3 ? app.go('nu-setup') : i === 2 ? app.home() : app.back()) }));
  nuRule(ui, cutTop(inn, 1), null, NU.gray);
  ui.pen(C.DEFAULT);
  ui.text(cutTop(inn, 1), 'up 01:12:44   link 12:03', S.BAR, CENTER);
}

// The access-code pad: a gray dialog on the desktop pattern.
function nuUnlock(ui, app) {
  const st = app.state;
  if (st.code == null) st.code = 3;
  nuDesktop(ui);
  const full = rect(0, 0, ui.cols, ui.rows), status = cutBottom(full, 1);
  const kw = 8, kh = 3, PAD = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'Del', '0', 'OK'];
  const inn = nuDialog(ui, kw * 3 + 5, kh * 4 + 5, 'Access code', NU.gray);
  cutTop(inn, 1);
  const slots = cutTop(inn, 1);
  ui.pen(C.DEFAULT);
  for (let i = 0; i < 6; i++)
    ui.putch(slots.x + Math.trunc((slots.w - 11) / 2) + i * 2, slots.y,
             i < st.code ? G.LED_ON : G.LED_OFF, S.BAR | BOLD);
  cutTop(inn, 1);
  const pad = center(inn, kw * 3, kh * 4);
  PAD.forEach((k, i) => nuBtn(ui, rect(pad.x + (i % 3) * kw, pad.y + Math.trunc(i / 3) * kh, kw, kh),
    NU_ID.KEY + i, k, i === 9 ? C.AMBER : i === 11 ? C.GREEN : NU.gray.key, NU.gray, { tap: () => {
      if (i === 9) st.code = Math.max(0, st.code - 1);
      else if (i === 11) app.back();
      else st.code = Math.min(6, st.code + 1);
    } }));
  nuStatus(ui, status, 'The deck is locked.');
}

// Every state of the round-1 parts on one sheet, on blue and on gray.
function nuStates(ui, app) {
  const u = nuUnit(ui), full = rect(0, 0, ui.cols, ui.rows), status = cutBottom(full, 1);
  const half = Math.trunc(full.w / 2);
  [[NU.blue, 'On a window', 0], [NU.gray, 'On a dialog', 100]].forEach(([sf, cap, base], n) => {
    const inn = inset(nuWin(ui, rect(n * half, 0, half, full.h), cap, sf, false), 1, 0);
    cutTop(inn, 1);
    nuBtnRow(ui, cutTop(inn, u), sf, [
      { id: NU_ID.BTN + base, label: 'Normal', accent: C.GREEN },
      { id: NU_ID.BTN + base + 1, label: 'Pressed', accent: C.GREEN }]);
    nuBtnRow(ui, cutTop(inn, u), sf, [
      { id: NU_ID.BTN + base + 2, label: 'Focus', accent: C.GREEN },
      { id: NU_ID.BTN + base + 3, label: 'Off', accent: C.GREEN, off: true }]);
    nuRow(ui, cutTop(inn, u), NU_ID.ROW + base, 'Row', 'plain', false, sf);
    nuRow(ui, cutTop(inn, u), NU_ID.ROW + base + 1, 'Row', 'chosen', true, sf);
    nuRow(ui, cutTop(inn, u), NU_ID.ROW + base + 2, 'Row', 'pressed', false, sf);
    nuCheck(ui, cutTop(inn, u), NU_ID.OPT + base, 'Check', true, sf);
    nuRadio(ui, cutTop(inn, u + 1), NU_ID.OPT + base + 10, 'Radio', ['one', 'two', 'three'], 1, sf, () => {});
  });
  nuStatus(ui, status, 'Pose: one button pressed, one in focus.');
}

export const SKETCHES = [
  ...MO_SKETCHES,
  { name: 'nu-home', title: 'Hosts: list and description', draw: (u, a) => nuHome(u, a, NU.blue) },
  { name: 'nu-setup', title: 'Setup: sections and options', draw: (u, a) => nuSetup(u, a, NU.blue) },
  { name: 'nu-edit', title: 'Edit host: form', draw: (u, a) => nuEdit(u, a, NU.blue) },
  { name: 'nu-confirm', title: 'Confirm: gray dialog with shadow', draw: nuConfirm },
  { name: 'nu-connect', title: 'Connecting: progress dialog', draw: nuConnect },
  { name: 'nu-menu', title: 'Session menu over the terminal', draw: nuMenu },
  { name: 'nu-unlock', title: 'Access code pad', draw: nuUnlock },
  { name: 'nu-states', title: 'Part states: normal, pressed, focus, off', draw: nuStates,
    pose: { focus: NU_ID.BTN + 2, down: NU_ID.BTN + 1 } },
  { name: 'nu-home-slate', title: 'Hosts, slate surface, muted frames', theme: 'slate',
    draw: (u, a) => nuHome(u, a, NU.slate) },
  { name: 'nu-setup-slate', title: 'Setup, slate surface, muted frames', theme: 'slate',
    draw: (u, a) => nuSetup(u, a, NU.slate) },
];
