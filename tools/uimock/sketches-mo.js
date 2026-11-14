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
} from './kit.js';
import { PROFILES, TERM_LINES } from './baseline.js';

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
             detail: S.FOCUS, hot: true };
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
// Returns { k, y, inn } for a caller that draws a control, or null.
function moPart(ui, r, id, o, act) {
  if (r.w < 4 || r.h < 3) { ui.issues.push({ kind: 'drop', text: o.title || `#${id}` }); return null; }
  if (!o.off) ui.hot(r, id, act);
  const k = moLook(ui, id, o);
  moSlab(ui, r, k);
  if (!k.off && o.stripe) moStripe(ui, r, o.stripe);
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
  if (o.detail) ui.text(rect(inn.x, y + 1, inn.w - (o.reserve | 0), 1), o.detail, k.detail, o.align || LEFT);
  ui.pen(C.DEFAULT);
  return { k, y, inn };
}

// A button: one centred label. `tone` and `accent` pick its weight.
function moBtn(ui, r, id, label, o, act) {
  return moPart(ui, r, id, { ...o, title: label, align: CENTER }, act);
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

// One choice of several: a caption, then a chip per option. The chosen
// chip has a filled face and the check mark. Focus is the light look.
function moChips(ui, r, idBase, caption, labels, value, pick, o = {}) {
  // Seven cells is the narrowest chip a finger hits at the small font.
  const cw = (l) => Math.max(7, moLen(l) + 4);
  const need = labels.reduce((n, l) => n + cw(l) + 1, -1);
  const y = moCentre(r.y, r.h, 1, caption);
  ui.pen(C.DEFAULT);
  ui.text(rect(r.x + 1, y, r.w - need - 2, 1), caption, S.TEXT, LEFT);
  if (need > r.w) { ui.issues.push({ kind: 'drop', text: `${caption} chips` }); return; }
  let x = r.x + r.w - need;
  labels.forEach((l, i) => {
    const w = cw(l), chosen = i === value;
    moPart(ui, rect(x, r.y, w, r.h), idBase + i, {
      title: chosen ? `✓ ${l}` : l, align: CENTER, plain: !chosen, pad: 1,
      tone: chosen ? 'solid' : 'card', accent: chosen ? (o.accent || C.BLUE) : 0,
      pose: o.poses ? o.poses[i] : '', off: o.off,
    }, { tap: () => pick(i) });
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


// Status row: the adopted chips on a panel-tone bar, then a hint.
// The hint names keys after the first key press.
// `alert` replaces the hint with a red line: what is wrong now.
function moStatus(ui, r, touchHint, keyHint, alert) {
  const y = r.y;
  ui.pen(C.DEFAULT);
  ui.fill(r, ' ', S.PANEL);
  let x = r.x + 1;
  ui.pen(C.GREEN);
  ui.puts(x, y, ' NET ', S.FOCUS | BOLD);
  x += 5;
  for (const g of [0x2582, 0x2584, 0x2586]) ui.putch(x++, y, g, S.FOCUS | BOLD);
  ui.putch(x++, y, ' ', S.FOCUS);
  x += 1;
  ui.pen(C.CYAN);
  ui.puts(x, y, ' KBD ', S.FOCUS | BOLD);
  x += 5;
  ui.pen(C.AMBER);
  ui.putch(x++, y, 'C', S.FOCUS | BOLD);
  ui.pen(C.CYAN);
  ui.puts(x, y, 'N ', S.FOCUS);
  x += 2;
  const clock = r.x + r.w - 8, room = rect(x + 2, y, clock - x - 3, 1);
  const hint = ui.inter.ring && keyHint ? keyHint : touchHint;
  if (alert) {
    ui.pen(C.RED);
    ui.text(room, alert, S.PANEL | BOLD, LEFT);
  } else if (hint) {
    ui.pen(C.DEFAULT);
    ui.text(room, hint, S.PANEL_MUTED, LEFT);
  }
  ui.pen(C.BLUE);
  ui.puts(clock, y, ' 14:07 ', S.FOCUS | BOLD);
  ui.pen(C.DEFAULT);
}

// Page header. With `back` the whole slab is the back target.
// o: { path, title, note, back }. Cut pills and the pager off first.
function moHeader(ui, r, o) {
  const y = moCentre(r.y, r.h, 1, 'header');
  if (!o.back) {
    const tl = moLen(o.title);
    ui.pen(C.DEFAULT);
    ui.text(rect(r.x + 1, y, tl, 1), o.title, S.TEXT | BOLD, LEFT);
    if (o.note) ui.text(rect(r.x + tl + 3, y, r.w - tl - 3, 1), o.note, S.MUTED, LEFT);
    return;
  }
  const slab = r;
  ui.hot(slab, ID_BACK, o.back);
  const k = moLook(ui, ID_BACK, { tone: 'tint', accent: C.BLUE });
  moSlab(ui, slab, k);
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
  if (ui.rows - y >= 1) moStatus(ui, rect(0, ui.rows - 1, ui.cols, 1), 'Hold a host for options', '');
}

// Slate screen, then header, body and status rects. Header and body
// share the side margin.
function moPage(ui) {
  moScreen(ui);
  const m = ui.metrics(), r = rect(0, 0, ui.cols, ui.rows);
  const status = cutBottom(r, 1), body = inset(r, m.margin, 0), header = cutTop(body, 3);
  return { header, body, status, m };
}

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

// A form field: label, then the value. Editing lights it and adds a caret.
// o.bad marks an invalid value: red stripe, red text, a mark at the end.
function moField(ui, r, id, label, value, o, act) {
  const p = moPart(ui, r, id, { title: '', accent: C.CYAN, lit: o.edit, stripe: o.bad ? C.RED : 0 }, act);
  if (!p) return;
  const k = p.k, lw = o.lw || 10, vx = p.inn.x + lw, vw = p.inn.w - lw;
  const cs = Array.from(o.mask ? '•'.repeat(moLen(value)) : value);
  const shown = o.edit && cs.length > vw - 1 ? cs.slice(cs.length - (vw - 1)).join('') : cs.join('');
  ui.pen(k.card ? MO_LABEL : k.pen);
  ui.text(rect(p.inn.x, p.y, lw - 1, 1), label, k.detail, LEFT);
  ui.pen(o.bad && k.card ? C.RED : k.pen);
  ui.text(rect(vx, p.y, vw - (o.bad ? 2 : 0), 1), shown, k.title, LEFT);
  if (o.bad) ui.putch(p.inn.x + p.inn.w - 1, p.y, '!', k.title);
  ui.pen(k.pen);
  if (o.edit) {
    ui.pen(C.WHITE);
    ui.putch(Math.min(vx + moLen(shown), vx + vw - 1), p.y, ' ', S.BAR);
  }
  ui.pen(C.DEFAULT);
}

// "user@host:port  key" in parts.
function moParse(where) {
  const m = /^([^@]+)@([^:]+):(\d+)(\s+key)?/.exec(where);
  return m ? { user: m[1], host: m[2], port: m[3], key: !!m[4] }
           : { user: '', host: where, port: '22', key: false };
}
const moHostOf = (st) => PROFILES[st.host | 0] || PROFILES[0];

// HOME: a host per tile. A tap connects. A hold opens the host page.
// New, Setup and Lock sit in the header, so no host count hides them.
function moHome(ui, app) {
  const st = app.state, pg = moPage(ui);
  moFirst(ui, MO_ID.HOST);
  const head = moPills(ui, pg.header, [
    { id: MO_ID.PILL, label: '+ New', act: { tap: () => { st.host = -1; app.go('mo-edit'); } } },
    { id: MO_ID.PILL + 1, label: 'Setup', act: { tap: () => app.go('mo-setup') } },
    { id: MO_ID.PILL + 2, label: 'Lock', act: { tap: () => app.go('mo-unlock') } },
  ]);
  const g = grid(pg.body, 2, 4, pg.m.gutter, 0);
  const per = gridCapacity(g), pages = pageCount(PROFILES.length, per);
  st.page = pageClamp(st.page | 0, PROFILES.length, per);
  const rest = moPager(ui, head, st.page, pages,
    { prev: { tap: () => st.page-- }, next: { tap: () => st.page++ } });
  moHeader(ui, rest, { title: 'Hosts', note: `${PROFILES.length} of 8` });
  for (let i = 0; i < per && st.page * per + i < PROFILES.length; i++) {
    const n = st.page * per + i, p = PROFILES[n], w = moParse(p.where);
    const act = { tap: () => { st.host = n; app.go('mo-connect'); },
                  hold: () => { st.host = n; app.go('mo-host'); } };
    moPart(ui, gridCell(g, i), MO_ID.HOST + n, {
      title: p.name, detail: `${w.user}@${w.host}${w.port === '22' ? '' : `:${w.port}`}`,
      stripe: profAccent(p.name), right: String(n + 1) }, act);
    ui.key(String(n + 1), act.tap);
  }
  moStatus(ui, pg.status, 'Tap to connect. Hold for options.', '↵ connect  Space options  1-8');
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
  moBtn(ui, cutTop(body, 3), MO_ID.BTN, 'Connect', { tone: 'solid', accent: C.GREEN },
    { tap: () => app.go('mo-connect') });
  const g = grid(body, 2, 3, pg.m.gutter, 0);
  moBtn(ui, gridCell(g, 0), MO_ID.BTN + 1, 'Edit', {}, { tap: () => app.go('mo-edit') });
  moBtn(ui, gridCell(g, 1), MO_ID.BTN + 2, 'Delete', { tone: 'tint', accent: C.RED },
    { tap: () => { st.ask = null; app.go('mo-confirm'); } });
  moBtn(ui, gridCell(g, 2), MO_ID.BTN + 3, '↑ Move earlier', {}, { tap: () => { app.note = 'moved'; } });
  moBtn(ui, gridCell(g, 3), MO_ID.BTN + 4, '↓ Move later', {}, { tap: () => { app.note = 'moved'; } });
  moStatus(ui, pg.status, app.note || 'The bar at the top goes back.', '↵ choose  Esc back');
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
  const g = grid(box, 2, 3, pg.m.gutter, 0);
  moBtn(ui, gridCell(g, 0), MO_ID.BTN, 'Cancel', {}, { tap: () => app.back() });
  moBtn(ui, gridCell(g, 1), MO_ID.BTN + 1, ask.verb, { tone: 'solid', accent }, { tap: yes });
  moStatus(ui, pg.status, '', '←→ move  ↵ choose  Esc cancel');
}

const MO_FIELDS = [
  { key: 'name', label: 'Name' }, { key: 'user', label: 'User' },
  { key: 'host', label: 'Host' }, { key: 'port', label: 'Port', lw: 6 },
  { key: 'pass', label: 'Password', mask: true },
];

// The host form. Save sits in the header. The header bar is Cancel.
function moEdit(ui, app) {
  const st = app.state, pg = moPage(ui), isNew = st.host === -1;
  if (!st.form || st.formFor !== st.host) {
    const p = moHostOf(st), w = moParse(p.where);
    st.form = isNew ? { name: '', user: '', host: '', port: '22', pass: '', auth: 0, show: false }
      : { name: p.name, user: w.user, host: w.host, port: w.port, pass: 'hunter22',
          auth: w.key ? 1 : 0, show: false };
    st.formFor = st.host;
    st.formWas = JSON.stringify(st.form);
    st.edit = -1;
  }
  const f = st.form, body = pg.body, gx = pg.m.gutter;
  moFirst(ui, MO_ID.FIELD);
  const changed = JSON.stringify(f) !== st.formWas;
  const port = /^\d+$/.test(f.port) && +f.port >= 1 && +f.port <= 65535;
  // An untouched new form shows no red yet. Save stays off until it is valid.
  const bad = {
    name: f.name ? '' : 'A host needs a name.',
    host: f.host ? '' : 'A host needs an address.',
    port: port ? '' : 'Port: a number from 1 to 65535.',
  };
  const wrong = bad.port || bad.name || bad.host;
  const shown = (key) => (bad[key] && (changed || key === 'port') ? bad[key] : '');
  const leave = () => { st.form = null; app.back(); };
  // A changed form asks first. An unchanged form leaves at once.
  const cancel = () => {
    st.edit = -1;
    if (JSON.stringify(f) === st.formWas) { leave(); return; }
    st.ask = { title: 'Discard the changes?', verb: 'Discard', accent: C.AMBER,
      lines: ['This host keeps its old values.', ''], yes: () => { app.back(); leave(); } };
    app.go('mo-confirm');
  };
  const head = moPills(ui, pg.header, [{ id: MO_ID.PILL, label: 'Save',
    o: { tone: 'solid', accent: C.GREEN, off: !!wrong }, act: { tap: leave } }]);
  moHeader(ui, head, { path: 'Hosts', title: isNew ? 'New host' : `Edit ${f.name}`,
    back: { tap: cancel } });
  ui.key('Escape', cancel);
  const half = (body.w - gx) >> 1, portW = 17, wide = body.w - gx - portW;
  const field = (i, r) => {
    const d = MO_FIELDS[i], edit = st.edit === i;
    moField(ui, r, MO_ID.FIELD + i, d.label, f[d.key],
      { edit, mask: d.mask && !f.show, lw: d.lw, bad: shown(d.key) },
      { tap: () => { st.edit = edit ? -1 : i; } });
  };
  let y = body.y;
  field(0, rect(body.x, y, half, 3));
  field(1, rect(body.x + half + gx, y, body.w - half - gx, 3));
  y += 3;
  field(2, rect(body.x, y, wide, 3));
  field(3, rect(body.x + wide + gx, y, portW, 3));
  y += 3;
  moChips(ui, rect(body.x, y, body.w, 3), MO_ID.OPT, 'Sign-in', ['password', 'key'], f.auth,
    (i) => { f.auth = i; st.edit = -1; });
  y += 3;
  if (f.auth === 0) {
    field(4, rect(body.x, y, half, 3));
    moToggle(ui, rect(body.x + half + gx, y, body.w - half - gx, 3), MO_ID.OPT + 4,
      'Show password', f.show, {}, { tap: () => { f.show = !f.show; } });
  } else {
    moPart(ui, rect(body.x, y, half, 3), MO_ID.OPT + 5,
      { title: 'Key', right: 'deck-ed25519 ›', rightBold: true }, { tap: () => { app.note = 'key picker'; } });
  }
  if (st.edit >= 0) {
    const d = MO_FIELDS[st.edit];
    ui.key('text', (ch) => { f[d.key] += ch; });
    ui.key('Backspace', () => { f[d.key] = Array.from(f[d.key]).slice(0, -1).join(''); });
    const done = () => { st.edit = -1; };
    ui.key('Enter', done);
    ui.key('Escape', done);
    // Up and down leave the field. Left and right belong to the caret.
    ui.key('ArrowUp', done);
    ui.key('ArrowDown', done);
    ui.key('ArrowLeft', () => {});
    ui.key('ArrowRight', () => {});
  }
  moStatus(ui, pg.status, st.edit >= 0 ? 'Type on the keyboard.' : 'Tap a field to change it.',
    st.edit >= 0 ? 'type  ↵ done  ↑↓ leave' : '↵ change  Esc cancel',
    shown('port') || shown('name') || shown('host'));
}

const MO_SETUP = [
  { name: 'Hosts', note: '8 stored', items: [
    { t: 'act', label: 'Import from phone', nav: true },
    { t: 'act', label: 'Import from PC', nav: true }] },
  { name: 'Wi-Fi', note: 'HomeAP  -55 dBm', items: [
    { t: 'act', label: 'Reconnect' }, { t: 'act', label: 'Add network (phone)', nav: true }] },
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
  { name: 'Font', note: '12x24  66x20', items: [
    { t: 'pick', key: 'font', label: 'Size (restarts the deck)', set: ['8x16', '10x20', '12x24'], init: 2,
      ask: (v) => ({ title: `Restart with the ${v} font?`, verb: 'Restart', accent: C.AMBER,
        lines: ['The deck restarts to change the font.', 'An open session ends.'] }) }] },
  { name: 'System', note: 'saver, reset', items: [
    { t: 'pick', key: 'saver', label: 'Saver + lock, min', set: ['1', '3', '5', '10', '30'], init: 2 },
    { t: 'toggle', key: 'edge', label: 'Edge scroll', init: false },
    { t: 'act', label: 'Clear host keys', accent: C.RED, ask: 'Clear every pinned host key?' },
    { t: 'act', label: 'Factory reset', accent: C.RED, ask: 'Erase every host and setting?' }] },
  { name: 'Keystore', note: 'unlocked', items: [
    { t: 'act', label: 'Lock deck', go: 'mo-unlock' }, { t: 'act', label: 'Change code', nav: true },
    { t: 'act', label: 'Remove code', accent: C.RED, nav: true, go: 'mo-unlock' }] },
];

// Setup hub: a section per tile, with its present state as the detail.
function moSetup(ui, app) {
  const st = app.state, pg = moPage(ui);
  moHeader(ui, pg.header, { title: 'Setup', back: { tap: () => app.back() } });
  const g = grid(pg.body, 2, 4, pg.m.gutter, 0);
  MO_SETUP.forEach((c, i) => moPart(ui, gridCell(g, i), MO_ID.HUB + i,
    { title: c.name, detail: c.note, right: '›' },
    { tap: () => { st.cat = i; st.spage = 0; app.go('mo-section'); } }));
  moStatus(ui, pg.status, 'The bar at the top goes back.', '↵ open  Esc back');
}

// One section. Switches and actions share a row of two. A choice takes
// a full row and shows every option, so no value hides behind a cycle.
function moSection(ui, app) {
  const st = app.state, pg = moPage(ui), c = MO_SETUP[st.cat | 0], body = pg.body;
  st.opt = st.opt || {};
  moFirst(ui, MO_ID.OPT);
  const rows = [];
  let open = null;
  c.items.forEach((it, n) => {
    const cell = { it, id: MO_ID.OPT + n * 8 };
    if (it.t === 'pick') { rows.push([cell]); open = null; }
    else if (open) { open.push(cell); open = null; }
    else { open = [cell]; rows.push(open); }
  });
  const per = Math.max(1, Math.trunc(body.h / 3)), pages = pageCount(rows.length, per);
  st.spage = pageClamp(st.spage | 0, rows.length, per);
  const flip = { prev: { tap: () => st.spage-- }, next: { tap: () => st.spage++ } };
  const head = moPager(ui, pg.header, st.spage, pages, flip);
  moHeader(ui, head, { path: 'Setup', title: c.name, back: { tap: () => app.back() } });
  if (pages > 1) {
    moRail(ui, cutRight(body, 1), st.spage, pages);
    cutRight(body, 1);
    ui.key('PageUp', () => { if (st.spage > 0) st.spage--; });
    ui.key('PageDown', () => { if (st.spage < pages - 1) st.spage++; });
  }
  const gx = pg.m.gutter, half = (body.w - gx) >> 1;
  rows.slice(st.spage * per, st.spage * per + per).forEach((row, ri) => {
    const y = body.y + ri * 3;
    row.forEach(({ it, id }, ci) => {
      const r = it.t === 'pick' ? rect(body.x, y, body.w, 3)
        : rect(body.x + ci * (half + gx), y, ci ? body.w - half - gx : half, 3);
      const v = it.key in st.opt ? st.opt[it.key] : it.init;
      if (it.t === 'pick') moChips(ui, r, id, it.label, it.set, v, (i) => {
        if (!it.ask || i === v) { st.opt[it.key] = i; return; }
        st.ask = { ...it.ask(it.set[i]), yes: () => { st.opt[it.key] = i; app.back(); } };
        app.go('mo-confirm');
      });
      else if (it.t === 'toggle')
        moToggle(ui, r, id, it.label, v, {}, { tap: () => { st.opt[it.key] = !v; } });
      else moPart(ui, r, id, {
        title: it.label, right: it.nav ? '›' : '', tone: it.accent ? 'tint' : 'card', accent: it.accent,
      }, { tap: () => {
        if (it.go) app.go(it.go);
        else if (it.ask) {
          st.ask = { title: it.ask, verb: it.label, lines: ['This cannot be undone.', ''],
            yes: () => { app.back(); app.note = `${it.label}: done`; } };
          app.go('mo-confirm');
        } else app.note = it.label;
      } });
    });
  });
  moStatus(ui, pg.status, app.note || 'A change applies at once.', '↵ change  Esc back');
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
  moStatus(ui, pg.status, 'Tap a network to join it.', '↵ join  PgUp PgDn page  Esc back');
}

// Access code pad. Keys are three rows high, so a digit sits centred.
function moUnlock(ui, app) {
  const st = app.state, pg = moPage(ui), code = st.code || '', wrong = !!st.wrong && !code;
  moHeader(ui, pg.header, { title: 'Deck locked', note: 'Enter the access code' });
  const kw = 9, w = 3 * kw + 2, pad = center(pg.body, w, 15);
  if (pg.body.h < 15) { ui.issues.push({ kind: 'drop', text: 'code pad' }); return; }
  const entry = cutTop(pad, 3);
  moSlab(ui, entry, moLook(ui, 0, {}));
  if (wrong) moStripe(ui, entry, C.RED);
  ui.pen(wrong ? C.RED : C.DEFAULT);
  ui.text(rect(entry.x + 2, entry.y + 1, entry.w - 4, 1),
    code ? Array.from(code, () => '•').join(' ') : wrong ? '✘ wrong code' : 'access code',
    code || wrong ? S.PANEL | BOLD : S.PANEL_MUTED, CENTER);
  ui.pen(C.DEFAULT);
  const add = (d) => () => { if (moLen(code) < 13) st.code = code + d; };
  const erase = () => { st.code = code.slice(0, -1); };
  // The mock treats 0000 as the wrong code.
  const enter = () => {
    if (!code) return;
    st.code = '';
    st.wrong = code === '0000';
    if (!st.wrong) app.home();
  };
  const KEYS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '←', '0', 'OK'];
  KEYS.forEach((label, i) => {
    const r = rect(pad.x + (i % 3) * (kw + 1), pad.y + Math.trunc(i / 3) * 3, kw, 3);
    if (label === '←') moBtn(ui, r, MO_ID.KEY + i, label, { off: !code }, { tap: erase });
    else if (label === 'OK')
      moBtn(ui, r, MO_ID.KEY + i, label, { tone: 'solid', accent: C.GREEN, off: !code }, { tap: enter });
    else { moBtn(ui, r, MO_ID.KEY + i, label, {}, { tap: add(label) }); ui.key(label, add(label)); }
  });
  ui.key('Backspace', erase);
  if (code) ui.key('Enter', enter);
  moStatus(ui, pg.status, 'Tap digits, or type the code.', '0-9 type  ↵ unlock',
    wrong ? 'Wrong code. Try again.' : '');
}

// Connecting: the steps so far, and a way out. `fail` shows the same
// panel after a refused sign-in: the red step says what went wrong.
function moConnect(ui, app, fail) {
  const st = app.state, pg = moPage(ui), p = moHostOf(st), w = moParse(p.where);
  const area = rect(pg.body.x, pg.header.y, pg.body.w, pg.header.h + pg.body.h);
  const box = center(area, Math.min(area.w, 46), 11), card = cutTop(box, 8);
  const ok = (text) => ['✓', text, 0, C.GREEN];
  if (fail) {
    moInfo(ui, card, [
      ['✘', `No session on ${p.name}`, C.WHITE, C.RED],
      ['', ''],
      ok('Wi-Fi  HomeAP'), ok(`Reach  ${w.host} port ${w.port}`), ok('Key exchange'),
      ['✘', 'Sign in: password refused', C.RED, C.RED],
    ], C.RED);
    const g = grid(box, 2, 3, pg.m.gutter, 0);
    moBtn(ui, gridCell(g, 0), MO_ID.BTN, 'Back', {}, { tap: () => app.back() });
    moBtn(ui, gridCell(g, 1), MO_ID.BTN + 1, 'Edit host', { tone: 'solid', accent: C.BLUE },
      { tap: () => app.go('mo-edit') });
    moStatus(ui, pg.status, '', '←→ move  ↵ choose', 'Sign in failed.');
    return;
  }
  moInfo(ui, card, [
    [String.fromCodePoint(spinnerGlyph(ui.frameNo + 2)), `Connecting to ${p.name}`, C.WHITE, C.AMBER],
    ['', ''],
    ok('Wi-Fi  HomeAP'), ok(`Reach  ${w.host} port ${w.port}`),
    ['›', 'Key exchange', C.WHITE, C.AMBER],
    ['·', `Sign in as ${w.user}`],
  ], profAccent(p.name));
  moBtn(ui, box, MO_ID.BTN, 'Cancel', {}, { tap: () => app.back() });
  ui.hot(card, MO_ID.ROW, { tap: () => app.go('mo-menu') });
  moStatus(ui, pg.status, 'Mock: tap the panel to finish.', '↵ cancel');
}

// Session menu: a slate sheet at the right over the dimmed terminal.
// Its parts are the same parts, because the sheet is screen background.
function moMenu(ui, app) {
  const st = app.state, p = moHostOf(st), m = ui.metrics();
  ui.term(TERM_LINES, MO_TERM_FG, 0);
  ui.dim();
  const w = Math.min(ui.cols, m.cls === COMPACT ? 30 : 36);
  const sheet = rect(ui.cols - w, 0, w, ui.rows);
  ui.fill(sheet, ' ', S.TEXT);
  const inn = inset(sheet, 1, 0);
  moHeader(ui, cutTop(inn, 3), { path: 'Session', title: p.name, back: { tap: () => app.back() } });
  moInfo(ui, cutTop(inn, 4), [['Up', '2 h 14 min'], ['Link', '41 min, -55 dBm']]);
  moPart(ui, cutTop(inn, 3), MO_ID.BTN, { title: 'Reset terminal' }, { tap: () => app.back() });
  moPart(ui, cutTop(inn, 3), MO_ID.BTN + 1, { title: 'Setup', right: '›' }, { tap: () => app.go('mo-setup') });
  moBtn(ui, cutTop(inn, 3), MO_ID.BTN + 2, 'Disconnect', { tone: 'tint', accent: C.RED },
    { tap: () => app.home() });
}

// A host form in a known state, for a gate frame.
const moFormState = (over, edit = -1) => () => {
  const base = { name: 'homelab', user: 'root', host: '192.168.1.10', port: '22',
                 pass: 'hunter22', auth: 0, show: false };
  return { host: 0, formFor: 0, form: { ...base, ...over }, formWas: JSON.stringify(base), edit };
};

// The frames the C kit must match cell for cell. kitcheck.mjs draws each
// frame and compares it with the lab screen of the same name.
// `state` makes the sketch state. `pose` is the key focus or the held finger.
export const MO_GATE = [
  { name: 'mo-parts', sketch: 'mo-parts' },
  { name: 'mo-home', sketch: 'mo-home' },
  { name: 'mo-home-keys', sketch: 'mo-home', pose: { focus: MO_ID.HOST + 1 } },
  { name: 'mo-home-down', sketch: 'mo-home', pose: { down: MO_ID.HOST + 2 } },
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
  { name: 'mo-edit-key', sketch: 'mo-edit', state: moFormState({ auth: 1 }), pose: { focus: MO_ID.OPT + 5 } },
  { name: 'mo-setup', sketch: 'mo-setup', pose: { focus: MO_ID.HUB + 3 } },
  { name: 'mo-section-wifi', sketch: 'mo-section', state: () => ({ cat: 1 }) },
  { name: 'mo-section-fx', sketch: 'mo-section', state: () => ({ cat: 3 }) },
  { name: 'mo-section-motion', sketch: 'mo-section', state: () => ({ cat: 4 }), pose: { down: MO_ID.OPT + 9 } },
  { name: 'mo-section-system', sketch: 'mo-section', state: () => ({ cat: 6 }), pose: { focus: MO_ID.OPT + 16 } },
  { name: 'mo-list', sketch: 'mo-list' },
  { name: 'mo-list-2', sketch: 'mo-list', state: () => ({ lpage: 1, net: 12 }), pose: { focus: MO_ID.ROW + 12 } },
  { name: 'mo-unlock', sketch: 'mo-unlock' },
  { name: 'mo-unlock-code', sketch: 'mo-unlock', state: () => ({ code: '12' }), pose: { down: MO_ID.KEY + 4 } },
  { name: 'mo-unlock-wrong', sketch: 'mo-unlock', state: () => ({ wrong: true }) },
  { name: 'mo-connect', sketch: 'mo-connect' },
  { name: 'mo-fail', sketch: 'mo-fail', pose: { focus: MO_ID.BTN + 1 } },
  { name: 'mo-menu', sketch: 'mo-menu', pose: { focus: MO_ID.BTN } },
];

export const MO_SKETCHES = [
  { name: 'mo-home', title: 'Hosts: tap connects, hold for options', theme: 'slate', draw: moHome },
  { name: 'mo-host', title: 'Host page (hold on a tile)', theme: 'slate', draw: moHost },
  { name: 'mo-confirm', title: 'Question page', theme: 'slate', draw: moConfirm },
  { name: 'mo-edit', title: 'Host form', theme: 'slate', draw: moEdit },
  { name: 'mo-setup', title: 'Setup hub', theme: 'slate', draw: moSetup },
  { name: 'mo-section', title: 'Setup section: switches and choices', theme: 'slate', draw: moSection },
  { name: 'mo-list', title: 'Paged list with pager and rail', theme: 'slate', draw: moList },
  { name: 'mo-unlock', title: 'Access code pad', theme: 'slate', draw: moUnlock },
  { name: 'mo-connect', title: 'Connecting', theme: 'slate', draw: (u, a) => moConnect(u, a, false) },
  { name: 'mo-fail', title: 'Connect failed', theme: 'slate', draw: (u, a) => moConnect(u, a, true) },
  { name: 'mo-menu', title: 'Session menu sheet', theme: 'slate', draw: moMenu },
  { name: 'mo-parts', title: 'Parts in every state', theme: 'slate', draw: moParts },
];
