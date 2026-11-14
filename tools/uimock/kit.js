// kit.js — the UI kit's drawing surface in JS, for design work without a
// C build. It ports layout and the Slab widgets from components/ui line
// for line. kitcheck.mjs proves the port against the lab's dump.
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
export const CARD = Object.freeze({
  CENTER: 0x01, DISABLED: 0x02, CHECKED: 0x04, NAV: 0x08, ACTION: 0x10, LIT: 0x20,
});
export const INPUT = Object.freeze({ MASK: 0x01, EDITING: 0x02, CYCLE: 0x04 });
export const ID_BACK = 0xFFFF;
export const COMPACT = 0, REGULAR = 1, WIDE = 2;
export const PAGER_W = 15;

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
  hline(col, row, width, l, f, r) {
    this.putch(col, row, l, S.TEXT);
    for (let i = 1; i < width - 1; i++) this.putch(col + i, row, f, S.TEXT);
    this.putch(col + width - 1, row, r, S.TEXT);
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

  // -- page scaffolding (ui_widgets.c)

  metrics() { return metricsFor(this.cols, this.rows); }
  page() {
    const m = this.metrics(), r = rect(0, 0, this.cols, this.rows);
    const header = cutTop(r, m.header), status = cutBottom(r, 1);
    return { header, status, body: inset(r, m.margin, 0) };
  }

  // -- widgets, Slab rendition

  _look(accent, flags, focus, down) {
    const off = flags & CARD.DISABLED;
    if (flags & CARD.LIT) down = true;
    const k = { pen: accent, value: S.WELL, ring: false };
    if (off && !focus) {
      k.fill = k.title = k.detail = S.WELL;
      k.edge = S.WELL_EDGE;
    } else if (focus || down) {
      k.fill = k.detail = S.FOCUS;
      k.title = S.FOCUS | BOLD;
      k.edge = S.FOCUS_EDGE;
      k.ring = focus;
    } else {
      k.fill = k.detail = S.BAR;
      k.title = S.BAR | BOLD;
      k.edge = S.BAR_EDGE;
    }
    return k;
  }

  // Paint the surface; returns the rect its content may use.
  _paint(r, k) {
    const edged = r.h >= 3;
    this.pen(k.pen);
    for (let y = 0; y < r.h; y++) {
      let cp = 0x20, st = k.fill;
      if (edged && y === 0) { cp = G.HALF_DN; st = k.edge; }
      if (edged && y === r.h - 1) { cp = G.HALF_UP; st = k.edge; }
      for (let x = 0; x < r.w; x++) this.putch(r.x + x, r.y + y, cp, st);
    }
    const inn = edged ? inset(r, 0, 1) : { ...r };
    if (k.ring) {
      this.pen(C.WHITE);
      for (let y = 0; y < r.h; y++) {
        const cp = !edged ? G.LHALF : y === 0 ? G.QUAD_LL
                 : y === r.h - 1 ? G.QUAD_UL : G.LHALF;
        this.putch(r.x, r.y + y, cp, S.BAR_EDGE);
      }
      this.pen(k.pen);
    }
    return inset(inn, r.w >= 8 ? 2 : 1, 0);
  }

  // The one tappable surface. c: { title, detail, value, accent, flags }.
  card(r, id, c, act = null) {
    if (empty(r)) return;
    const flags = c.flags | 0, accent = c.accent | 0;
    const off = flags & CARD.DISABLED;
    this.hot(r, id, act);
    const k = this._look(accent, flags, this.focused(id), !off && this.pressed(id));
    const inn = this._paint(r, k);
    if (empty(inn)) return;
    const title = c.title || '';

    if (c.value) {
      const vl = chars(c.value).length;
      const room = inn.w + 1 - chars(title).length;
      let ww = vl + 4 < 8 ? 8 : vl + 4;
      if (ww > room) ww = room < 8 ? 8 : room;
      const w = cutRight(inn, ww - 2);
      for (let y = r.y; y < r.y + r.h; y++) {
        const top = r.h >= 3 && y === r.y, bot = r.h >= 3 && y === r.y + r.h - 1;
        for (let x = w.x; x < w.x + w.w + 2; x++)
          this.putch(x, y, top ? G.HALF_DN : bot ? G.HALF_UP : 0x20,
                     top || bot ? S.WELL_EDGE : S.WELL);
      }
      this.text(rect(w.x + 1, inn.y + div(inn.h - 1, 2), w.w - 1, 1), c.value, k.value, RIGHT);
      cutRight(inn, 1);
    } else if (flags & (CARD.NAV | CARD.CHECKED)) {
      const w = cutRight(inn, 1), chk = flags & CARD.CHECKED;
      this.putch(w.x, inn.y + div(inn.h - 1, 2), chk ? G.CHECK : G.CHEVRON,
                 chk ? k.title : k.detail);
      cutRight(inn, 1);
    }

    const two = !!c.detail && inn.h >= 2;
    const ty = inn.y + div(inn.h - (two ? 2 : 1), 2);
    const al = flags & CARD.CENTER ? CENTER : LEFT;
    this.text(rect(inn.x, ty, inn.w, 1), title, k.title, al);
    if (two) this.text(rect(inn.x, ty + 1, inn.w, 1), c.detail, k.detail, al);
    this.pen(C.DEFAULT);
  }

  btn(r, id, label, accent, act = null) {
    this.card(r, id, { title: label, accent, flags: CARD.CENTER | CARD.ACTION }, act);
  }

  // Page title bar. With `back`, the whole bar is the back target.
  header(r, path, title, note, back, act = null) {
    if (empty(r)) return;
    const m = this.metrics().margin, row = r.y + div(r.h, 2);
    const lit = back && (this.focused(ID_BACK) || this.pressed(ID_BACK));
    if (back) this.hot(r, ID_BACK, act);
    const k = this._look(C.BLUE, 0, false, lit);
    let inn = this._paint({ ...r }, k);
    inn = inset(inn, m - 1, 0);
    if (back) {
      this.putch(inn.x, row, G.POINT_L, k.title);
      cutLeft(inn, 3);
    }
    if (note) {
      const nl = chars(note).length;
      const n = cutRight(inn, nl + 1 < inn.w ? nl + 1 : inn.w);
      this.text(rect(n.x, row, n.w, 1), note, k.detail, RIGHT);
    }
    if (path) {
      const pl = chars(path).length;
      if (pl + 3 < div(inn.w, 2) + 3) {
        this.text(rect(inn.x, row, pl, 1), path, k.detail, LEFT);
        this.putch(inn.x + pl + 1, row, '/', k.detail);
        cutLeft(inn, pl + 3);
      }
    }
    this.text(rect(inn.x, row, inn.w, 1), title, k.title, LEFT);
    this.pen(C.DEFAULT);
  }

  // Draws `< 1/2 >` at the right end of a header. Returns the rest.
  pager(header, prev, next, page, pages, acts = {}) {
    if (pages <= 1 || header.w < PAGER_W + 8) return header;
    const r = cutRight(header, PAGER_W), n = cutRight(r, 5), p = cutLeft(r, 5);
    this.card(p, prev, { title: '<', accent: C.BLUE,
      flags: CARD.CENTER | (page > 0 ? 0 : CARD.DISABLED) }, acts.prev);
    this.card(n, next, { title: '>', accent: C.BLUE,
      flags: CARD.CENTER | (page < pages - 1 ? 0 : CARD.DISABLED) }, acts.next);
    this.pen(C.BLUE);
    this.fill(r, 0x20, S.TEXT);
    this.text(rect(r.x, r.y + div(r.h, 2), r.w, 1), `${page + 1}/${pages}`,
              S.TEXT | BOLD, CENTER);
    this.pen(C.DEFAULT);
    return header;
  }

  // A labeled value: plain, masked, editing (caret) or cycle.
  input(r, id, label, text, cursor, flags, act = null) {
    if (empty(r)) return;
    this.hot(r, id, act);
    const edit = flags & INPUT.EDITING;
    const k = this._look(C.CYAN, 0, edit || this.focused(id), this.pressed(id));
    const inn = this._paint(r, k);
    if (empty(inn)) return;
    const row = inn.y + div(inn.h - 1, 2);
    if (flags & INPUT.CYCLE) {
      this.putch(inn.x + inn.w - 1, row, G.CHEVRON, k.detail);
      cutRight(inn, 2);
    }
    const ll = chars(label).length, lw = ll + 2 < 7 ? 7 : ll + 2;
    this.text(rect(inn.x, row, lw - 1, 1), label, k.detail, LEFT);
    cutLeft(inn, lw);
    if (inn.w < 2) return;
    const cs = chars(text), len = cs.length;
    cursor = clamp(cursor, 0, len);
    const start = edit && cursor > inn.w - 1 ? cursor - (inn.w - 1) : 0;
    for (let i = 0; i < inn.w; i++) {
      const idx = start + i;
      const cp = idx < len ? (flags & INPUT.MASK ? '*' : cs[idx]) : ' ';
      let st = k.title;
      if (edit && idx === cursor) { this.pen(C.WHITE); st = S.BAR; }
      this.putch(inn.x + i, row, cp, st);
      this.pen(k.pen);
    }
    this.pen(C.DEFAULT);
  }

  // One-row gauge in eighths of a cell.
  meter(r, value, max, accent) {
    if (empty(r) || max <= 0) return;
    value = clamp(value, 0, max);
    const eighths = div(value * r.w * 8, max);
    this.pen(accent);
    for (let i = 0; i < r.w; i++) {
      const f = eighths - i * 8;
      this.putch(r.x + i, r.y, f >= 8 ? G.BLOCK : f > 0 ? 0x2590 - f : 0x20, S.WELL);
    }
    this.pen(C.DEFAULT);
  }

  // A window over a dimmed backdrop; returns its content rect.
  dialog(w, h, title, accent) {
    this.dim();
    const r = center(rect(0, 0, this.cols, this.rows), w, h);
    this.pen(accent);
    const x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (let y = r.y; y <= y1; y++)
      for (let x = r.x; x <= x1; x++) {
        let cp = 0x20, st = S.TEXT;
        if (y === r.y) { cp = G.HALF_DN; st = S.PANEL_EDGE; }
        else if (y <= r.y + 2) st = S.PANEL;
        else if (y === y1) { cp = x === r.x ? 0x2599 : x === x1 ? 0x259F : G.HALF_DN; st = S.PANEL_EDGE; }
        else if (x === r.x) { cp = G.LHALF; st = S.PANEL_EDGE; }
        else if (x === x1) { cp = G.RHALF; st = S.PANEL_EDGE; }
        this.putch(x, y, cp, st);
      }
    this.putch(r.x + 2, r.y + 1, G.LHALF, S.PANEL);
    this.text(rect(r.x + 4, r.y + 1, r.w - 6, 1), title, S.PANEL | BOLD, LEFT);
    this.pen(C.DEFAULT);
    const inn = inset(r, 2, 1);
    cutTop(inn, 3);
    return inn;
  }
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
