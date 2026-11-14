// kit.js — the UI kit's drawing surface in JS, for design work without a
// C build. It ports layout, the status row and the text helpers from
// components/ui. kitcheck.mjs proves the port against the lab's dump.
// The surface enforces the device rules. A cell has one style and one
// accent. The surface rejects a style outside the vocabulary.

import { Frame } from './model.js';

export const S = Object.freeze({
  TEXT: 0, MUTED: 1, BAR: 2, WELL: 3, FOCUS: 4, TRACK: 5, BAR_EDGE: 6,
  WELL_EDGE: 7, FOCUS_EDGE: 8, PANEL: 9, PANEL_MUTED: 10, PANEL_EDGE: 11,
});
export const STYLE_COUNT = 12, ACCENTS = 8, BOLD = 0x80;
export const C = Object.freeze({
  DEFAULT: 0, GREEN: 1, CYAN: 2, MAGENTA: 3, AMBER: 4, RED: 5, BLUE: 6, WHITE: 7,
});
export const G = Object.freeze({
  BOX_H: 0x2500, BOX_V: 0x2502, BOX_TL: 0x256D, BOX_TR: 0x256E, BOX_BL: 0x2570,
  BOX_BR: 0x256F, BLOCK: 0x2588, SHADE1: 0x2591, SHADE2: 0x2592, SHADE3: 0x2593,
  LHALF: 0x258C, RHALF: 0x2590, QUAD_LL: 0x2596, QUAD_UL: 0x2598,
  HALF_UP: 0x2580, HALF_DN: 0x2584, CHECK: 0x2713, CROSS: 0x2718,
  CHEVRON: 0x203A, ELLIPSIS: 0x2026, LED_ON: 0x25CF, LED_OFF: 0x25CB,
  DIAMOND: 0x25C6, PLAY: 0x25B6, ARROW: 0x25BA, POINT_L: 0x25C4, VBAR: 0x25AE,
  BULLET: 0x2022,
});
export const LEFT = 0, CENTER = 1, RIGHT = 2;
export const ID_BACK = 0xFFFF;
export const COMPACT = 0, REGULAR = 1, WIDE = 2;

const div = (a, b) => Math.trunc(a / b);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const chars = (s) => Array.from(String(s));

export const rect = (x, y, w, h) => ({ x, y, w, h });
export const empty = (r) => r.w <= 0 || r.h <= 0;

export function cutTop(r, n) {
  n = clamp(n, 0, r.h > 0 ? r.h : 0);
  const s = rect(r.x, r.y, r.w, n);
  r.y += n; r.h -= n;
  return s;
}
export function cutBottom(r, n) {
  n = clamp(n, 0, r.h > 0 ? r.h : 0);
  r.h -= n;
  return rect(r.x, r.y + r.h, r.w, n);
}
export function cutLeft(r, n) {
  n = clamp(n, 0, r.w > 0 ? r.w : 0);
  const s = rect(r.x, r.y, n, r.h);
  r.x += n; r.w -= n;
  return s;
}
export function cutRight(r, n) {
  n = clamp(n, 0, r.w > 0 ? r.w : 0);
  r.w -= n;
  return rect(r.x + r.w, r.y, n, r.h);
}
export function inset(r, dx, dy) {
  return rect(r.x + dx, r.y + dy, Math.max(0, r.w - 2 * dx), Math.max(0, r.h - 2 * dy));
}
export function center(outer, w, h) {
  w = clamp(w, 0, Math.max(0, outer.w));
  h = clamp(h, 0, Math.max(0, outer.h));
  return rect(outer.x + div(outer.w - w, 2), outer.y + div(outer.h - h, 2), w, h);
}
export function grid(area, ncols, ch, gx, gy) {
  const g = { area: { ...area }, ncols, ch, gx, gy, cw: 0, nrows: 0 };
  if (ncols <= 0 || ch <= 0 || empty(area)) { g.ncols = 0; return g; }
  g.cw = div(area.w - (ncols - 1) * gx, ncols);
  g.nrows = div(area.h + gy, ch + gy);
  if (g.cw <= 0) g.ncols = g.nrows = 0;
  return g;
}
export function gridCell(g, i) {
  if (i < 0 || i >= g.ncols * g.nrows) return rect(0, 0, 0, 0);
  const c = i % g.ncols, r = div(i, g.ncols);
  return rect(g.area.x + c * (g.cw + g.gx), g.area.y + r * (g.ch + g.gy), g.cw, g.ch);
}
export const gridCapacity = (g) => g.ncols * g.nrows;

export function metricsFor(cols) {
  const m = { gutter: 2, unit: 3, header: 3, cls: COMPACT, margin: 1 };
  if (cols >= 96) { m.cls = WIDE; m.margin = 2; }
  else if (cols >= 76) { m.cls = REGULAR; }
  return m;
}
export const pageCount = (count, per) =>
  per <= 0 || count <= per ? 1 : div(count + per - 1, per);
export function pageClamp(page, count, per) {
  const n = pageCount(count, per);
  return page < 0 ? 0 : page >= n ? n - 1 : page;
}

const SPIN = [0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827];
export const spinnerGlyph = (frame) => SPIN[frame % 8];

// First shown character of an entry value; ui_entry_first in C. The window
// of `room` cells holds room - 1 characters and keeps the caret in view.
export function entryFirst(len, caret, room) {
  const shown = room - 1;
  if (len <= shown) return 0;
  let first = caret - (room >> 1);
  if (first > len - shown) first = len - shown;
  return first < 0 ? 0 : first;
}

// How a typed code shows in `room` cells; ui_code_plan in C. Marks for
// the taken characters, then dots for those still to come.
export function codePlan(len, expected, room) {
  const total = Math.max(len, expected);
  const spaced = total > 0 && total * 2 - 1 <= room;
  const cap = Math.min(total, room), bullets = Math.min(len, cap), dots = cap - bullets;
  const marks = bullets + dots;
  return { bullets, dots, spaced, width: marks === 0 ? 0 : spaced ? marks * 2 - 1 : marks };
}

// Status row; ui_note_span, ui_toast_w, ui_status and ui_toast in C.
// A chip is { accent, cells: [{ state, cp, accent, keep }] }. Its states:
export const SB = Object.freeze({ ABSENT: 0, OFF: 1, ON: 2, BUSY: 3, ALERT: 4 });
export const NOTE = Object.freeze({ NONE: 0, PLAIN: 1, GOOD: 2, ALERT: 3 });
const NOTE_MIN_GAP = 16, SB_BLINK_FRAMES = 8;

export function noteSpan(left, chipsEnd, clockX, takeRow) {
  let gap = clockX - chipsEnd, x = chipsEnd + 1, whole = false;
  if (takeRow && gap < NOTE_MIN_GAP) { whole = true; x = left + 1; gap = clockX - left; }
  return { x, w: gap > 2 ? gap - 2 : 0, whole };
}

export function toastW(len, mark, maxW) {
  const w = len + 2 + (mark ? 2 : 0);
  return maxW < 4 ? 0 : Math.min(w, maxW);
}

function sbLook(c, accent, frame) {
  let cp = c.cp;
  switch (c.state) {
    case SB.ON: return { cp, style: S.FOCUS, pen: c.accent || accent };
    case SB.BUSY:
      if (!c.keep) cp = spinnerGlyph(frame);
      return { cp, style: Math.trunc(frame / SB_BLINK_FRAMES) & 1 ? S.WELL : S.FOCUS, pen: C.AMBER };
    case SB.ALERT:
      if (!c.keep) cp = G.CROSS;
      return { cp, style: S.FOCUS, pen: C.RED };
    default: return { cp, style: S.WELL, pen: c.accent || C.BLUE };
  }
}

const notePen = (tone) => (tone === NOTE.GOOD ? C.GREEN : tone === NOTE.ALERT ? C.RED : C.DEFAULT);

// Cells a chip may stretch; ui_sb_room in C.
export function sbRoom(chipsEnd, clockX, note) {
  return Math.max(0, clockX - chipsEnd - (note ? NOTE_MIN_GAP : 2));
}

// `s` holds the chips, clock, note, tone, hintTouch, hintKeys and frame.
// A stretched chip adds xchip, xtext and xw: the cells shown, a blank first.
export function statusBar(ui, r, s) {
  const y = r.y, left = r.x + 1;
  ui.pen(C.DEFAULT);
  ui.fill(rect(r.x, y, r.w, 1), ' ', S.PANEL);
  let clockX = r.x + r.w;
  if (s.clock) {
    const n = chars(s.clock).length;
    clockX = r.x + r.w - (n + 2) - 1;
    ui.pen(C.BLUE);
    ui.putch(clockX, y, ' ', S.FOCUS | BOLD);
    ui.text(rect(clockX + 1, y, n, 1), s.clock, S.FOCUS | BOLD, LEFT);
    ui.putch(clockX + 1 + n, y, ' ', S.FOCUS | BOLD);
    ui.pen(C.DEFAULT);
  }
  const chips = s.chips || [], live = (c) => c.cells.length && c.cells[0].state !== SB.ABSENT;
  let x = left;
  for (const c of chips) {
    if (!live(c)) continue;
    if (x > left) x++;
    x += c.cells.length + 2;
  }
  let xw = 0;
  const xcp = s.xtext ? chars(s.xtext) : [];
  if (xcp.length && s.xchip >= 0 && s.xchip < chips.length) {
    xw = Math.min(1 + xcp.length, s.xw || 0, sbRoom(x, clockX, !!s.note));
  }
  x = left;
  chips.forEach((c, ci) => {
    if (!live(c)) return;
    if (x > left) x++;
    let last = null;
    const put = (cell, ch) => {
      const k = sbLook(cell, c.accent, s.frame || 0);
      ui.pen(k.pen);
      ui.putch(x++, y, ch === undefined ? k.cp : ch, k.style | BOLD);
      last = k;
    };
    put(c.cells[0], ' ');
    for (const cell of c.cells) put(cell);
    if (ci === s.xchip && xw > 0) {
      const k = sbLook(c.cells[0], c.accent, 0);
      ui.pen(k.pen);
      for (let i = 0; i < xw; i++) ui.putch(x++, y, i ? xcp[i - 1] : ' ', k.style | BOLD);
      last = k;
    }
    ui.putch(x++, y, ' ', last.style | BOLD);
    ui.pen(C.DEFAULT);
  });
  const sp = noteSpan(left, x, clockX, !!s.note);
  if (s.note) {
    if (sp.whole) ui.fill(rect(left, y, clockX - left, 1), ' ', S.PANEL);
    ui.pen(notePen(s.tone));
    ui.text(rect(sp.x, y, sp.w, 1), s.note, S.PANEL | BOLD, LEFT);
  } else {
    const hint = ui.inter.ring && s.hintKeys ? s.hintKeys : s.hintTouch;
    if (hint) {
      ui.pen(C.DEFAULT);
      ui.text(rect(sp.x, y, sp.w, 1), hint, S.PANEL_MUTED, LEFT);
    }
  }
  ui.pen(C.DEFAULT);
}

export function toast(ui, r, text, tone) {
  const mark = tone === NOTE.GOOD, w = toastW(chars(text).length, mark, r.w);
  if (!w) return;
  const x = r.x + r.w - w;
  ui.pen(C.DEFAULT);
  ui.fill(rect(x, r.y, w, 1), ' ', S.PANEL);
  ui.pen(notePen(tone));
  let tx = x + 1;
  if (mark) { ui.putch(tx, r.y, G.CHECK, S.PANEL | BOLD); tx += 2; }
  ui.text(rect(tx, r.y, x + w - 1 - tx, 1), text, S.PANEL | BOLD, LEFT);
  ui.pen(C.DEFAULT);
}

// The QR panel: half blocks, a quiet zone, a caption row. ui_qr in C.
export const QR_ZONE = 2;
export const qrW = (size) => size + 2 * QR_ZONE;
export const qrH = (size) => ((qrW(size) + 1) >> 1) + 1;
// Panel rows that fit a body: all, one less (no caption), or 0.
export function qrRows(size, w, h) {
  const qw = qrW(size), qh = qrH(size);
  if (!size || w - qw - 2 < 30 || qh - 1 > h) return 0;
  return qh <= h ? qh : qh - 1;
}
export function qrGlyph(top, bot) {
  return top && bot ? 0x2588 : top ? 0x2580 : bot ? 0x2584 : 0x20;
}

// A fixed fake code for the sketches and the lab: finder squares, timing
// lines and hashed modules. It is not scannable. The seed picks the data.
export function fakeQr(size, seed) {
  const finder = (x, y) => {
    for (let c = 0; c < 3; c++) {
      const dx = x - (c === 1 ? size - 7 : 0), dy = y - (c === 2 ? size - 7 : 0);
      if (dx < 0 || dx > 6 || dy < 0 || dy > 6) continue;
      return dx === 0 || dx === 6 || dy === 0 || dy === 6 || (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
    }
    return false;
  };
  return (x, y) => {
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    if ((x < 8 && y < 8) || (x >= size - 8 && y < 8) || (x < 8 && y >= size - 8)) return finder(x, y);
    if (x === 6 || y === 6) return ((x + y) & 1) === 0;
    let h = Math.imul(((x * 31 + y) ^ seed) >>> 0, 2654435761) >>> 0;
    h = (h ^ (h >>> 15)) >>> 0;
    return ((h >>> 3) & 1) === 1;
  };
}

// Text wrapped into at most `max` lines of `w` cells. A break falls at the
// last space that fits. The last line keeps the rest. ui_wrap_n in C.
export function wrapLines(text, w, max = 3) {
  const out = [];
  let rest = String(text);
  while (rest.length > w && out.length < max - 1) {
    let cut = rest.lastIndexOf(' ', w);
    if (cut <= 0) cut = w;
    out.push(rest.slice(0, cut));
    rest = rest.slice(cut).replace(/^ +/, '');
  }
  out.push(rest);
  return out;
}

// The rows of a steps panel. A step is { mark, text, value, pen }. Its
// text wraps to `w` cells under the mark. A value takes the next row, in
// its pen. A text with no value takes the pen itself. Mark '!' is a
// caution: its mark takes the pen too.
export function stepLines(steps, w) {
  const rows = [];
  for (const s of steps) {
    wrapLines(s.text, w).forEach((t, i) => rows.push({
      label: i ? '' : s.mark, text: t, pen: s.value ? 0 : s.pen | 0,
      labelPen: s.mark === '!' && !i ? s.pen | 0 : 0 }));
    if (s.value) rows.push({ label: '', text: s.value, pen: s.pen | 0, labelPen: 0 });
  }
  return rows;
}

export function profAccent(name) {
  const pal = [C.GREEN, C.CYAN, C.MAGENTA, C.AMBER, C.BLUE];
  let h = 5381;
  for (const ch of name) h = (Math.imul(h, 33) + ch.charCodeAt(0)) >>> 0;
  return pal[h % pal.length];
}

// Interaction state a screen draws against. It outlives a frame.
export const newInter = () => ({ focus: 0, ring: false, down: 0 });

export class Ui {
  // `inter`: focus / ring / finger-down state; `frameNo`: animation clock.
  constructor(cols, rows, inter = newInter(), frameNo = 0) {
    this.cols = cols;
    this.rows = rows;
    this.frame = new Frame(cols, rows);
    this.inter = inter;
    this.frameNo = frameNo;
    this.hots = [];
    this.issues = [];
    this.keys = {};
    this._pen = C.DEFAULT;
  }

  // -- primitives (ui.c)

  pen(color) {
    if (!(color >= 0 && color < ACCENTS)) throw new Error(`pen ${color}: not an accent`);
    this._pen = color;
  }
  dim() { this._pen = C.DEFAULT; this.frame.scrim = true; }

  putch(col, row, cp, style) {
    if ((style & ~(0x0F | BOLD)) !== 0 || (style & 0x0F) >= STYLE_COUNT)
      throw new Error(`style 0x${style.toString(16)}: one S.* style, optionally | BOLD`);
    if (typeof cp === 'string') cp = cp.codePointAt(0);
    if (cp > 0xFFFF) cp = 0x3F;
    if (col < 0 || col >= this.cols || row < 0 || row >= this.rows) return;
    const i = row * this.cols + col;
    this.frame.cp[i] = cp;
    this.frame.bold[i] = style & BOLD ? 1 : 0;
    this.frame.pal[i] = (style & 0x0F) * ACCENTS + this._pen;
  }
  puts(col, row, s, style) {
    for (const ch of chars(s)) {
      if (col >= this.cols) break;
      this.putch(col++, row, ch, style);
    }
  }
  fill(r, cp, style) {
    for (let y = r.y; y < r.y + r.h; y++)
      for (let x = r.x; x < r.x + r.w; x++) this.putch(x, y, cp, style);
  }
  // Text in a one-row rect; an overlong string ends in an ellipsis.
  text(r, s, style, align = LEFT) {
    if (empty(r) || s == null) return;
    const cs = chars(s), len = cs.length, n = Math.min(len, r.w);
    let x = r.x;
    if (align === CENTER) x += div(r.w - n, 2);
    else if (align === RIGHT) x += r.w - n;
    for (let i = 0; i < n; i++)
      this.putch(x + i, r.y, len > r.w && i === n - 1 ? G.ELLIPSIS : cs[i], style);
    if (len > r.w) this.issues.push({ kind: 'clip', text: String(s), w: r.w, x: r.x, y: r.y });
  }
  // Fake terminal content behind the overlay.
  term(lines, fg, bg = 0) {
    const f = this.frame;
    for (let r = 0; r < this.rows; r++) {
      const l = chars(lines.length ? lines[r % lines.length] : '');
      for (let c = 0; c < this.cols; c++) {
        const i = r * this.cols + c;
        f.tcp[i] = c < l.length ? l[c].codePointAt(0) : 0x20;
        f.tfg[i] = fg; f.tbg[i] = bg;
      }
    }
  }

  // -- interaction (ui_hot.c)

  // Register a hit region. `act`: { tap(), hold(), label } for the mock.
  hot(r, id, act = null) {
    if (!id || empty(r)) return;
    const h = { ...r, id, act };
    if (h.x < 0) { h.w += h.x; h.x = 0; }
    if (h.y < 0) { h.h += h.y; h.y = 0; }
    if (h.w <= 0 || h.h <= 0) return;
    this.hots.push(h);
    if (!this.inter.focus && id !== ID_BACK) this.inter.focus = id;
  }
  hotAt(col, row) {
    for (let i = this.hots.length - 1; i >= 0; i--) {
      const h = this.hots[i];
      if (col >= h.x && col < h.x + h.w && row >= h.y && row < h.y + h.h) return h;
    }
    return null;
  }
  // Mock only: bind a key to an action. 'text' takes each typed character.
  key(name, fn) { this.keys[name] = fn; }
  focused(id) { return this.inter.ring && this.inter.focus === id; }
  pressed(id) { return this.inter.down === id; }

  metrics() { return metricsFor(this.cols, this.rows); }
}

// Draw a screen in a fixed pose, as the lab does for a screenshot.
// `pose`: { focus, down } region ids; `inter` may carry state in.
export function drawPosed(scr, cols, rows, app) {
  // Pass 1 lays out the regions and settles the default focus.
  const inter = newInter();
  scr.draw(new Ui(cols, rows, inter), app);
  const pose = scr.pose || {};
  inter.down = pose.down || 0;
  if (pose.focus) { inter.focus = pose.focus; inter.ring = true; }
  // The lab draws its posed pass at frame 1.
  const ui = new Ui(cols, rows, inter, 1);
  scr.draw(ui, app);
  return ui;
}

// Distance from region f to c along one arrow; -1 when c is not that way.
// A cell is twice as tall as wide, so rows count double.
// A region straight ahead beats any region off to the side. Among those
// straight ahead the nearest band wins, then the nearest leading edge.
function navScore(f, c, dir) {
  const fx0 = f.x, fx1 = f.x + f.w, fy0 = 2 * f.y, fy1 = 2 * (f.y + f.h);
  const cx0 = c.x, cx1 = c.x + c.w, cy0 = 2 * c.y, cy1 = 2 * (c.y + c.h);
  let along, cross;
  switch (dir) {
    case 'right': if (cx0 < fx1) return -1; along = cx0 - fx1; break;
    case 'left':  if (cx1 > fx0) return -1; along = fx0 - cx1; break;
    case 'down':  if (cy0 < fy1) return -1; along = cy0 - fy1; break;
    case 'up':    if (cy1 > fy0) return -1; along = fy0 - cy1; break;
    default: return -1;
  }
  let edge;
  if (dir === 'left' || dir === 'right') {
    cross = cy0 >= fy1 ? cy0 - fy1 + 1 : fy0 >= cy1 ? fy0 - cy1 + 1 : 0;
    edge = Math.abs(cy0 - fy0);
  } else {
    cross = cx0 >= fx1 ? cx0 - fx1 + 1 : fx0 >= cx1 ? fx0 - cx1 + 1 : 0;
    edge = Math.abs(cx0 - fx0);
  }
  if (!cross) return along * 256 + Math.min(edge, 255);
  return 0x10000 + along + 4 * (cross + 8);
}

// Id to focus after an arrow key. `dir`: 'up' | 'down' | 'left' | 'right'.
// Unknown focus lands on the first region that is not the back bar; no
// region that way keeps the focus.
export function hotMove(hots, focusId, dir) {
  if (!hots.length) return focusId;
  const f = hots.find((h) => h.id === focusId);
  if (!f) return (hots.find((h) => h.id !== ID_BACK) || hots[0]).id;
  let best = -1, bestScore = 0;
  for (let i = 0; i < hots.length; i++) {
    if (hots[i].id === focusId) continue;
    const sc = navScore(f, hots[i], dir);
    if (sc >= 0 && (best < 0 || sc < bestScore)) { best = i; bestScore = sc; }
  }
  return best < 0 ? focusId : hots[best].id;
}
