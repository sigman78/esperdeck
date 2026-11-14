// model.js — the deck's display path as a pure function: a grid of cells
// in, RGB565 pixels out. Glyphs and style tables come from
// `cyberdeck_uilab --dump`, never from a hand copy. check.mjs proves the
// output equal to the lab's own screenshots, pixel for pixel.
// Runs unchanged in Node and in a browser.

export const ATTR_BOLD = 1, ATTR_UNDERLINE = 2, ATTR_REVERSE = 4;

// Panels the model can show. `mm` is the active area; it gives the
// physical size of a touch target. 800x480 is the board in the tree.
export const PANELS = {
  '800x480':  { width: 800,  height: 480, mm: [154.08, 85.92] },
  '1024x600': { width: 1024, height: 600, mm: [154.21, 85.92] },
};

export class Font {
  constructor(json) {
    this.w = json.w;
    this.h = json.h;
    this.name = `${json.w}x${json.h}`;
    this.spriteBase = json.sprite_base;
    this.spriteCount = json.sprite_count;
    this.sprites = new Map();          // cp -> Uint16Array(h), set by a design
    this._src = [json.regular, json.bold];
    this._digits = json.w <= 8 ? 2 : 4;
    this._cache = new Map();
    this._blank = new Uint16Array(json.h);
  }

  isSprite(cp) { return cp >= this.spriteBase && cp < this.spriteBase + this.spriteCount; }
  has(cp) { return this.isSprite(cp) || cp in this._src[0]; }
  hasBold(cp) { return cp in this._src[1]; }
  codepoints() { return Object.keys(this._src[0]).map(Number); }

  // Rows of a glyph, bit (w-1) = leftmost pixel. null when the font has
  // no such glyph (the device then draws '?').
  rows(cp, bold) {
    if (this.isSprite(cp)) return this.sprites.get(cp) || this._blank;
    const key = bold ? cp + 0x10000 : cp;
    let g = this._cache.get(key);
    if (g !== undefined) return g;
    const s = (bold && this._src[1][cp]) || this._src[0][cp];
    if (!s) g = null;
    else {
      g = new Uint16Array(this.h);
      for (let r = 0; r < this.h; r++)
        g[r] = parseInt(s.substr(r * this._digits, this._digits), 16);
    }
    this._cache.set(key, g);
    return g;
  }
}

// One frame: the overlay layer (cp 0 = transparent) over a terminal layer.
export class Frame {
  constructor(cols, rows) {
    const n = cols * rows;
    this.cols = cols;
    this.rows = rows;
    this.cp = new Uint16Array(n);
    this.pal = new Uint8Array(n);
    this.bold = new Uint8Array(n);
    this.scrim = false;
    this.tcp = new Uint16Array(n).fill(0x20);
    this.tfg = new Uint16Array(n);
    this.tbg = new Uint16Array(n);
    this.tattr = new Uint8Array(n);
  }

  // Build from one "screens" entry of a lab dump.
  static fromDump(lab, scr) {
    const f = new Frame(lab.cols, lab.rows);
    for (let i = 0, n = lab.cols * lab.rows; i < n; i++) {
      f.cp[i] = scr.ov[i * 3];
      f.bold[i] = scr.ov[i * 3 + 1] & 1;
      f.pal[i] = scr.ov[i * 3 + 2];
      f.tcp[i] = scr.term[i * 4];
      f.tfg[i] = scr.term[i * 4 + 1];
      f.tbg[i] = scr.term[i * 4 + 2];
      f.tattr[i] = scr.term[i * 4 + 3];
    }
    f.scrim = scr.scrim;
    return f;
  }
}

const dim565 = (p) => (p - ((p >> 4) & 0x0861)) & 0xFFFF;

function mono565(p, mode) {
  const r6 = ((p >> 11) & 0x1F) << 1, g6 = (p >> 5) & 0x3F, b6 = (p & 0x1F) << 1;
  const y = (r6 * 5 + g6 * 9 + b6 * 2) >> 4;
  return mode === 1 ? (y << 5) : (((y >> 1) << 11) | (((y * 11) >> 4) << 5));
}

const ERR_A = 0xF81F, ERR_B = 0xFFE0;   // missing glyph in strict mode

// Render a frame. `table` is a theme's flat [fg, bg, fg, bg, ...] style
// table. Returns { px: Uint16Array of RGB565, missing: [{col,row,cp}] }.
// opts:
//   - width, height: panel size in pixels.
//   - scanlines: default on.
//   - boldPop: default on. Brightens bold terminal text.
//   - mono: 0 color, 1 green, 2 amber.
//   - strict: a missing glyph paints an error cell, not '?'.
export function render(frame, font, table, opts = {}) {
  const W = opts.width, H = opts.height;
  const scan = opts.scanlines !== false, pop = opts.boldPop !== false;
  const mono = opts.mono | 0;
  const cw = font.w, ch = font.h;
  const cols = Math.min(frame.cols, Math.floor(W / cw));
  const rows = Math.min(frame.rows, Math.floor(H / ch));
  const count = table.length >> 1;
  const px = new Uint16Array(W * H);
  const missing = [];
  const full = (1 << cw) - 1;

  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const i = r * frame.cols + c;
      let fg, bg, g, ul = false, err = false;
      const ocp = frame.cp[i];
      if (ocp) {
        const p = frame.pal[i] < count ? frame.pal[i] : 0;
        fg = table[p * 2];
        bg = table[p * 2 + 1];
        g = font.rows(ocp, frame.bold[i]);
        if (!g) { missing.push({ col: c, row: r, cp: ocp }); err = !!opts.strict; g = font.rows(0x3F, false); }
        if (mono) { fg = mono565(fg, mono); bg = mono565(bg, mono); }
      } else {
        const a = frame.tattr[i], tcp = frame.tcp[i];
        fg = frame.tfg[i];
        bg = frame.tbg[i];
        if (a & ATTR_REVERSE) { const t = fg; fg = bg; bg = t; }
        ul = !!(a & ATTR_UNDERLINE);
        if (frame.scrim) { fg = (fg >> 1) & 0x7BEF; bg = (bg >> 1) & 0x7BEF; }
        if (tcp === 0x20 || tcp === 0) g = null;
        else g = font.rows(tcp, a & ATTR_BOLD) || font.rows(0x3F, false);
        if (mono) { fg = mono565(fg, mono); bg = mono565(bg, mono); }
        if ((a & ATTR_BOLD) && pop) fg |= (fg >> 1) & 0x7BEF;
      }
      const fgd = scan ? dim565(fg) : fg, bgd = scan ? dim565(bg) : bg;
      for (let y = 0; y < ch; y++) {
        const Y = r * ch + y, odd = Y & 1;
        let bits = g ? g[y] : 0;
        if (ul && y >= ch - 2) bits = full;
        let o = Y * W + c * cw;
        if (err) {
          for (let x = 0; x < cw; x++) px[o++] = ((x >> 1) + (y >> 1)) & 1 ? ERR_A : ERR_B;
          continue;
        }
        const f = odd ? fgd : fg, b = odd ? bgd : bg;
        for (let x = cw - 1; x >= 0; x--) px[o++] = (bits >> x) & 1 ? f : b;
      }
    }
  }
  return { px, missing };
}

// RGB565 -> RGBA bytes. `exact` uses the lab screenshot's plain shifts
// (white = 248). The default fills the low bits (white = 255).
export function toRGBA(px, exact = false) {
  const out = new Uint8ClampedArray(px.length * 4);
  for (let i = 0, o = 0; i < px.length; i++, o += 4) {
    const p = px[i], r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    if (exact) { out[o] = r << 3; out[o + 1] = g << 2; out[o + 2] = b << 3; }
    else { out[o] = (r << 3) | (r >> 2); out[o + 1] = (g << 2) | (g >> 4); out[o + 2] = (b << 3) | (b >> 2); }
    out[o + 3] = 255;
  }
  return out;
}

export const rgb565 = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
export const css565 = (p) => {
  const r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
  return `rgb(${(r << 3) | (r >> 2)},${(g << 2) | (g >> 4)},${(b << 3) | (b >> 2)})`;
};

// Physical size of a cell rect on a panel, in mm.
export function rectMM(panel, font, w, h) {
  return [w * font.w * panel.mm[0] / panel.width, h * font.h * panel.mm[1] / panel.height];
}
