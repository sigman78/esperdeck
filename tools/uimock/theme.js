// theme.js — the style table builder in JS: a port of ui_theme_build()
// in components/ui/ui_theme.c. kitcheck.mjs proves it against the lab's
// dumped tables. A sketch uses it to try a colour pair the lab lacks.

import { rgb565 } from './model.js';
import { S, STYLE_COUNT, ACCENTS } from './kit.js';

const u16 = (v) => v & 0xFFFF;
const tdiv = (a, b) => Math.trunc(a / b);

const ACCENT_TEXT = [
  0, rgb565(85, 255, 85), rgb565(85, 255, 255), rgb565(255, 85, 255),
  rgb565(255, 255, 85), rgb565(255, 85, 85), rgb565(85, 85, 255), rgb565(255, 255, 255),
];
const ACCENT_BAR = [
  rgb565(148, 148, 148), rgb565(96, 168, 96), rgb565(80, 160, 168), rgb565(168, 96, 160),
  rgb565(200, 152, 72), rgb565(184, 88, 80), rgb565(104, 112, 192), rgb565(255, 255, 255),
];
const ACCENT_WELL = [
  rgb565(89, 89, 89), rgb565(58, 101, 58), rgb565(48, 96, 101), rgb565(101, 58, 96),
  rgb565(120, 91, 43), rgb565(110, 53, 48), rgb565(62, 67, 115), rgb565(153, 153, 153),
];

const barText = (a, bar, bg) =>
  (a >= 1 && a <= 6 ? u16(((bar >> 2) & 0x39E7) + 0x7BEF + 0x39E7) : bg);

function panelOf(bg) {
  const r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
  return u16(((r + tdiv(24 - r, 5)) << 11) | ((g + tdiv(52 - g, 5)) << 5) | (b + tdiv(30 - b, 5)));
}

function mix8(a, b, n) {
  const r = tdiv(((a >> 11) & 0x1F) * n + ((b >> 11) & 0x1F) * (8 - n), 8);
  const g = tdiv(((a >> 5) & 0x3F) * n + ((b >> 5) & 0x3F) * (8 - n), 8);
  const bl = tdiv((a & 0x1F) * n + (b & 0x1F) * (8 - n), 8);
  return u16((r << 11) | (g << 5) | bl);
}

// The luma recipe of the display's mono effect: 0..62.
function luma(p) {
  const r6 = ((p >> 11) & 0x1F) << 1, g6 = (p >> 5) & 0x3F, b6 = (p & 0x1F) << 1;
  return (r6 * 5 + g6 * 9 + b6 * 2) >> 4;
}

// Muted text on a panel: five eighths of the text colour. A hue near
// the panel's own brightness takes more, so it stays readable.
function panelMuted(text, panel) {
  for (let n = 5; n < 8; n++) {
    const c = mix8(text, panel, n);
    if (Math.abs(luma(c) - luma(panel)) >= 6) return c;
  }
  return text;
}

const wash = (c) => u16(((c >> 1) & 0x7BEF) + 0x7BEF);

// Returns { fg, bg, table }, the shape of a theme in a lab dump.
export function themeBuild(fg, bg) {
  const table = new Array(STYLE_COUNT * ACCENTS * 2).fill(0);
  const set = (style, a, f, b) => {
    table[(style * ACCENTS + a) * 2] = f;
    table[(style * ACCENTS + a) * 2 + 1] = b;
  };
  const panel = panelOf(bg);
  for (let a = 0; a < ACCENTS; a++) {
    const text = a ? ACCENT_TEXT[a] : fg;
    set(S.TEXT, a, text, bg);
    set(S.MUTED, a, u16(((text >> 1) & 0x7BEF) + ((bg >> 1) & 0x7BEF)), bg);
    set(S.BAR, a, barText(a, ACCENT_BAR[a], bg), ACCENT_BAR[a]);
    set(S.WELL, a, barText(a, ACCENT_WELL[a], bg), ACCENT_WELL[a]);
    set(S.FOCUS, a, bg, wash(ACCENT_BAR[a]));
    set(S.TRACK, a, 0x7BEF, 0x39E7);
    set(S.BAR_EDGE, a, ACCENT_BAR[a], bg);
    set(S.WELL_EDGE, a, ACCENT_WELL[a], bg);
    set(S.FOCUS_EDGE, a, wash(ACCENT_BAR[a]), bg);
    set(S.PANEL, a, text, panel);
    set(S.PANEL_MUTED, a, panelMuted(text, panel), panel);
    set(S.PANEL_EDGE, a, panel, bg);
  }
  return { fg, bg, table };
}

// Colour pairs the lab does not dump. A sketch names one as its theme.
export const THEMES = {
  slate: themeBuild(rgb565(208, 216, 216), rgb565(0, 0, 0)),
};
