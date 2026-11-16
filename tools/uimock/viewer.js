// viewer.js — the page logic for the deck UI model: screen viewer you can
// tap through, font comparison, style table, glyph chart. Everything drawn
// on the device canvases goes through model.js; the page only scales it.

import { Font, render, toRGBA, css565, PANELS, rectMM } from './model.js';
import {
  Ui, newInter, drawPosed, hotMove, pageStep, rect, S, C, BOLD, ID_BACK, STYLE_COUNT, ACCENTS,
} from './kit.js';
import { MO_SKETCHES } from './sketches-mo.js';
import { THEMES } from './theme.js';

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"']/g, (c) =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, '0');
const mm1 = (n) => n.toFixed(1);

const DATA = JSON.parse($('model-data').textContent);
Object.assign(DATA.themes, THEMES);
const FONTS = {};
for (const k of Object.keys(DATA.fonts)) FONTS[k] = new Font(DATA.fonts[k]);
const ACCENT_NAMES = Object.keys(C);

const TABS = ['screen', 'compare', 'styles', 'glyphs'];
const DEFAULTS = {
  font: '12x24', panel: '800x480', theme: 'auto', mono: 0, scan: true,
  touch: false, grid: false, pose: false, zoom: '1x', tab: 'screen', cmp: 'stack',
};
const STORE_KEY = 'deck-ui-model.v2';
const P = { ...DEFAULTS };
try {
  const o = JSON.parse(localStorage.getItem(STORE_KEY) || '{}');
  for (const k of Object.keys(DEFAULTS)) if (typeof o[k] === typeof DEFAULTS[k]) P[k] = o[k];
} catch (e) { /* storage unavailable: defaults */ }
if (!FONTS[P.font]) P.font = DEFAULTS.font;
if (!PANELS[P.panel]) P.panel = DEFAULTS.panel;
if (P.theme !== 'auto' && !DATA.themes[P.theme]) P.theme = DEFAULTS.theme;
if (![0, 1, 2].includes(P.mono)) P.mono = 0;
if (!['1x', '2x'].includes(P.zoom)) P.zoom = '1x';
if (!['stack', 'side'].includes(P.cmp)) P.cmp = 'stack';
if (!TABS.includes(P.tab)) P.tab = 'screen';
try { const h = location.hash.slice(1); if (TABS.includes(h)) P.tab = h; } catch (e) { /* no hash */ }
const savePrefs = () => { try { localStorage.setItem(STORE_KEY, JSON.stringify(P)); } catch (e) { /* ignore */ } };

const curFont = () => FONTS[P.font];
const curPanel = () => PANELS[P.panel];
// 'auto' takes the theme the top screen names, else the default one.
const curTheme = () => DATA.themes[P.theme !== 'auto' ? P.theme
  : (stack.length && stack[stack.length - 1].scr.theme) || 'default'];
const gridOf = (font, panel) =>
  [Math.floor(panel.width / font.w), Math.floor(panel.height / font.h)];
const renderOpts = (panel, strict) => ({
  width: panel.width, height: panel.height, scanlines: P.scan, mono: P.mono, strict,
});

function paint(canvas, res, w, h) {
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  canvas.getContext('2d').putImageData(new ImageData(toRGBA(res.px), w, h), 0, 0);
}

const ALL = ['screen', 'compare', 'styles', 'glyphs'];
const TOOLS = [
  { key: 'font', label: 'Font', tabs: ALL, type: 'radio',
    opts: Object.keys(DATA.fonts).map((k) => [k, k]), readout: true },
  { key: 'panel', label: 'Panel', tabs: ['screen', 'compare'], type: 'radio',
    opts: [['800x480', '800x480 (current board)'], ['1024x600', '1024x600 (what-if)']] },
  { key: 'theme', label: 'Theme', tabs: ALL, type: 'radio',
    opts: [['auto', "screen's own"], ...Object.keys(DATA.themes).sort().map((k) => [k, k])] },
  { key: 'mono', label: 'Phosphor', tabs: ALL, type: 'radio',
    opts: [[0, 'color'], [1, 'green'], [2, 'amber']] },
  { key: 'disp', label: 'Display', tabs: ALL, type: 'check', opts: [['scan', 'Scanlines']] },
  { key: 'ovl', label: 'Overlays', tabs: ['screen'], type: 'check',
    opts: [['touch', 'Touch regions'], ['grid', 'Cell grid']] },
  { key: 'zoom', label: 'Zoom', tabs: ['screen'], type: 'radio',
    opts: [['1x', '1x'], ['2x', '2x']] },
  { key: 'mode', label: 'Mode', tabs: ['screen'], type: 'check', opts: [['pose', 'Lab pose']] },
];

function radios(host, name, opts, cur) {
  host.innerHTML = opts.map(([v, text]) => {
    const id = `${name}-${v}`;
    return `<span class="opt"><input type="radio" id="${id}" name="${name}" value="${v}" data-pref="${name}"` +
      `${String(cur) === String(v) ? ' checked' : ''}><label for="${id}">${esc(text)}</label></span>`;
  }).join('');
}

function buildToolbar() {
  const bar = $('toolbar');
  for (const t of TOOLS) {
    const g = document.createElement('div');
    g.className = 'group';
    g.id = `grp-${t.key}`;
    g.innerHTML = `<span class="group-label">${t.label}${t.readout ? ' <span class="readout" id="grid-readout"></span>' : ''}</span>` +
      `<div class="seg" role="${t.type === 'radio' ? 'radiogroup' : 'group'}" aria-label="${t.label}" id="seg-${t.key}"></div>`;
    bar.appendChild(g);
    const seg = g.querySelector('.seg');
    if (t.type === 'radio') radios(seg, t.key, t.opts, P[t.key]);
    else seg.innerHTML = t.opts.map(([k, text]) =>
      `<span class="opt"><input type="checkbox" id="chk-${k}" data-pref="${k}"${P[k] ? ' checked' : ''}>` +
      `<label for="chk-${k}">${esc(text)}</label></span>`).join('');
  }
  radios($('cmp-layout'), 'cmp', [['stack', 'Stacked'], ['side', 'Side by side']], P.cmp);
  document.addEventListener('change', (e) => {
    const el = e.target;
    if (!el.dataset || !el.dataset.pref) return;
    const k = el.dataset.pref;
    P[k] = el.type === 'checkbox' ? el.checked
      : typeof DEFAULTS[k] === 'number' ? Number(el.value) : el.value;
    savePrefs();
    renderAll();
  });
}

function syncToolbar() {
  for (const t of TOOLS) $(`grp-${t.key}`).hidden = !t.tabs.includes(P.tab);
  const [c, r] = gridOf(curFont(), curPanel());
  $('grid-readout').textContent = `${c} x ${r}`;
  $('viewport').dataset.zoom = P.zoom;
}

const SCREEN_GROUPS = [
  { id: 'sketches', label: 'Sketches', items: MO_SKETCHES },
];
const findScreen = (name) => {
  for (const g of SCREEN_GROUPS) for (const s of g.items) if (s.name === name) return s;
  return null;
};

let stack = [];
const keyMode = () => stack.length > 0 && stack[stack.length - 1].inter.ring;
const app = {
  state: {},
  note: '',
  get fg() { return curTheme().fg; },
  get bg() { return curTheme().bg; },
  go(name) {
    const s = findScreen(name);
    app.note = s ? '' : ` no screen ${name}`;
    // The ring is one mode for the whole shell, as in ui_hot.c.
    // It shows on every screen while the last input was a key.
    if (s) stack.push({ scr: s, inter: { ...newInter(), ring: keyMode() } });
  },
  back() {
    const ring = keyMode();
    if (stack.length > 1) stack.pop();
    Object.assign(stack[stack.length - 1].inter, { down: 0, ring });
    app.note = '';
  },
  home() { const ring = keyMode(); stack.length = 1; stack[0].inter = { ...newInter(), ring }; app.note = ''; },
};
const topEntry = () => stack[stack.length - 1];
function selectScreen(scr) { stack = [{ scr, inter: newInter() }]; app.note = ''; }

function buildPicker() {
  $('picker').innerHTML = SCREEN_GROUPS.map((g) => `<h2 class="ph">${esc(g.label)}</h2>` +
    (g.items.length
      ? `<div class="plist">${g.items.map((s, i) =>
        `<button type="button" class="pick" id="pick-${g.id}-${esc(s.name)}" data-g="${g.id}" data-i="${i}">` +
        `<span>${esc(s.title)}</span><small>${esc(s.name)}</small></button>`).join('')}</div>`
      : '<p class="none">none yet</p>')).join('');
  $('picker').addEventListener('click', (e) => {
    const b = e.target.closest('.pick');
    if (!b) return;
    const g = SCREEN_GROUPS.find((x) => x.id === b.dataset.g);
    selectScreen(g.items[Number(b.dataset.i)]);
    renderScreen();
  });
}

function syncPicker() {
  const root = stack[0].scr;
  for (const g of SCREEN_GROUPS) g.items.forEach((s, i) => {
    const b = $(`pick-${g.id}-${s.name}`);
    if (b) b.setAttribute('aria-current', s === root ? 'true' : 'false');
  });
}

let lastUi = null, lastPanel = null, lastFont = null, scaleNow = 1, regionsOpen = false;

const verdict = (m) => (m < 7 ? 'small' : m < 9 ? 'tight' : 'ok');
const VERDICT_TEXT = { small: '▲ too small', tight: '◆ tight', ok: '● ok' };
const regionName = (id) => (id === ID_BACK ? 'BACK' : `#${id}`);

function regionList(ui, font, panel) {
  return ui.hots.map((h) => {
    const [w, hh] = rectMM(panel, font, h.w, h.h);
    const m = Math.min(w, hh);
    return { h, w, hh, m, v: verdict(m) };
  }).sort((a, b) => a.m - b.m);
}

function renderScreen() {
  const font = curFont(), panel = curPanel(), theme = curTheme();
  const [cols, rows] = gridOf(font, panel);
  const top = topEntry();
  $('screen-title').textContent = top.scr.title || top.scr.name;
  $('screen-name').textContent = top.scr.name;
  syncPicker();
  let ui = null, res = null, err = '';
  try {
    if (P.pose) ui = drawPosed(top.scr, cols, rows, app);
    else { ui = new Ui(cols, rows, top.inter, 0); top.scr.draw(ui, app); }
    res = render(ui.frame, font, theme.table, renderOpts(panel, true));
  } catch (e) { err = String(e && e.message ? e.message : e); }
  $('draw-error').hidden = !err;
  $('draw-error').textContent = err ? `Draw failed: ${err}` : '';
  if (err) { $('facts').innerHTML = breadcrumbHtml(); return; }
  lastUi = ui; lastPanel = panel; lastFont = font;
  paint($('dev'), res, panel.width, panel.height);
  $('dev').setAttribute('aria-label', `${top.scr.title || top.scr.name}, ${cols} by ${rows} cells`);
  layoutStage();
  $('facts').innerHTML = factsHtml(ui, font, panel, res);
  const det = $('facts').querySelector('details');
  if (det) det.addEventListener('toggle', () => { regionsOpen = det.open; });
}

function layoutStage() {
  if (!lastPanel) return;
  const W = lastPanel.width, H = lastPanel.height;
  // Whole multiples only: any other scale smears the pixel-exact glyphs.
  const cssW = (P.zoom === '2x' ? 2 : 1) * W;
  const cssH = Math.round(cssW * H / W);
  scaleNow = cssW / W;
  $('stage').style.width = `${cssW}px`;
  $('stage').style.height = `${cssH}px`;
  for (const id of ['dev', 'ov']) {
    $(id).style.width = `${cssW}px`;
    $(id).style.height = `${cssH}px`;
  }
  drawOverlay(cssW, cssH);
}

function drawOverlay(cssW, cssH) {
  const ov = $('ov'), dpr = window.devicePixelRatio || 1;
  ov.width = Math.max(1, Math.round(cssW * dpr));
  ov.height = Math.max(1, Math.round(cssH * dpr));
  const g = ov.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, cssW, cssH);
  if (!lastUi || (!P.grid && !P.touch)) return;
  const font = lastFont, panel = lastPanel, k = scaleNow;
  const cw = font.w * k, ch = font.h * k;
  const cs = getComputedStyle(document.documentElement);
  const tok = (n) => cs.getPropertyValue(n).trim();
  if (P.grid) {
    g.save();
    g.globalCompositeOperation = 'difference';
    g.strokeStyle = tok('--ov-grid');
    g.lineWidth = 1;
    g.beginPath();
    for (let c = 0; c <= lastUi.cols; c++) { const x = Math.round(c * cw) + 0.5; g.moveTo(x, 0); g.lineTo(x, lastUi.rows * ch); }
    for (let r = 0; r <= lastUi.rows; r++) { const y = Math.round(r * ch) + 0.5; g.moveTo(0, y); g.lineTo(lastUi.cols * cw, y); }
    g.stroke();
    g.restore();
  }
  if (P.touch) {
    const color = { ok: tok('--ov-ok'), tight: tok('--ov-tight'), small: tok('--ov-bad') };
    const dash = { ok: [], tight: [6, 3], small: [2, 3] };
    const tag = { ok: '', tight: '~ ', small: '! ' };
    g.font = `600 14px ${tok('--mono')}`;
    g.textBaseline = 'middle';
    for (const r of regionList(lastUi, font, panel)) {
      const x = r.h.x * cw, y = r.h.y * ch, w = r.h.w * cw, h = r.h.h * ch;
      g.lineWidth = 2;
      g.strokeStyle = color[r.v];
      g.setLineDash(dash[r.v]);
      g.strokeRect(x + 1, y + 1, Math.max(0, w - 2), Math.max(0, h - 2));
      g.setLineDash([]);
      const full = `${tag[r.v]}${mm1(r.w)} x ${mm1(r.hh)} mm`;
      const short = `${tag[r.v]}${mm1(r.w)} x ${mm1(r.hh)}`;
      const label = [full, short].find((t) => g.measureText(t).width + 8 <= w - 4);
      if (!label || h < 22) continue;
      const tw = g.measureText(label).width + 8;
      g.fillStyle = tok('--ov-chip-bg');
      g.fillRect(x + 3, y + 3, tw, 18);
      g.fillStyle = color[r.v];
      g.fillText(label, x + 7, y + 12);
    }
  }
}

function breadcrumbHtml() {
  const last = stack.length - 1;
  const items = stack.map((e, i) => i === last
    ? `<span class="here" aria-current="location">${esc(e.scr.name)}</span>`
    : `<button type="button" data-pop="${i}">${esc(e.scr.name)}</button>`);
  return `<div><h3>Navigation</h3><div class="crumbs">${items.join('<span class="sep" aria-hidden="true">/</span>')}` +
    '<button type="button" class="btn" id="reset-state">Reset state</button></div></div>';
}

function factsHtml(ui, font, panel, res) {
  const [cols, rows] = gridOf(font, panel);
  const [cwmm, chmm] = rectMM(panel, font, 1, 1);
  const regs = regionList(ui, font, panel);
  const small = regs[0];
  const clips = ui.issues.filter((i) => i.kind === 'clip');
  const miss = new Map();
  for (const m of res.missing) {
    const e = miss.get(m.cp) || { n: 0, col: m.col, row: m.row };
    e.n++; miss.set(m.cp, e);
  }
  let h = breadcrumbHtml();
  if (P.pose) h += '<div class="muted">Lab pose is on: the screen is static and does not react to taps.</div>';
  h += `<div><h3>Geometry</h3><dl>
    <dt>Grid</dt><dd>${cols} x ${rows} cells</dd>
    <dt>Cell</dt><dd>${font.w} x ${font.h} px, ${mm1(cwmm)} x ${mm1(chmm)} mm</dd>
    <dt>Panel</dt><dd>${panel.width} x ${panel.height} px, ${panel.mm[0]} x ${panel.mm[1]} mm</dd>
    <dt>Touch regions</dt><dd>${regs.length}</dd>
    <dt>Smallest</dt><dd>${small
    ? `${regionName(small.h.id)} ${mm1(small.w)} x ${mm1(small.hh)} mm <span class="badge ${small.v}">${VERDICT_TEXT[small.v]}</span>`
    : 'none'}</dd></dl>
    <p class="muted" style="margin:8px 0 0;font-size:.875rem">Smaller side under 7 mm is too small, under 9 mm is tight.</p></div>`;
  if (regs.length) {
    h += `<div><details id="regions-details"${regionsOpen ? ' open' : ''}><summary>All touch regions (${regs.length}), smallest first</summary>
      <div class="scroll"><table class="tbl"><thead><tr><th>Region</th><th>Cells</th><th>mm</th><th>Verdict</th></tr></thead><tbody>${
  regs.map((r) => `<tr><td>${regionName(r.h.id)}</td><td>${r.h.w} x ${r.h.h} at ${r.h.x},${r.h.y}</td>` +
    `<td>${mm1(r.w)} x ${mm1(r.hh)}</td><td><span class="badge ${r.v}">${VERDICT_TEXT[r.v]}</span></td></tr>`).join('')
}</tbody></table></div></details></div>`;
  }
  h += `<div><h3>Clipped text</h3>${clips.length
    ? `<ul>${clips.map((c) => `<li>"${esc(c.text)}" needs ${Array.from(c.text).length} cells, has ${c.w}</li>`).join('')}</ul>`
    : '<span class="muted">none</span>'}</div>`;
  h += `<div><h3>Missing glyphs</h3>${miss.size
    ? `<ul>${[...miss].map(([cp, e]) => `<li>U+${hex4(cp)} x ${e.n}, first at col ${e.col} row ${e.row}</li>`).join('')}</ul>
       <p class="muted" style="margin:6px 0 0;font-size:.875rem">Drawn as a magenta and yellow checker on the canvas.</p>`
    : '<span class="muted">none</span>'}</div>`;
  return h;
}

// Pointer and keyboard on the Screen canvas.

function hitFor(e) {
  const dev = $('dev'), b = dev.getBoundingClientRect();
  if (!lastUi || !b.width) return null;
  const px = (e.clientX - b.left) * dev.width / b.width;
  const py = (e.clientY - b.top) * dev.height / b.height;
  const col = Math.floor(px / lastFont.w), row = Math.floor(py / lastFont.h);
  if (col < 0 || row < 0 || col >= lastUi.cols || row >= lastUi.rows) return null;
  return lastUi.hotAt(col, row);
}

function fireTap(h) {
  if (h.act && h.act.tap) h.act.tap();
  else app.note = ` press ${h.id}`;
}

let press = null;
function endPress() {
  if (!press) return;
  clearTimeout(press.timer);
  press.inter.down = 0;
  press = null;
}

function wirePointer() {
  const dev = $('dev');
  dev.addEventListener('contextmenu', (e) => e.preventDefault());
  dev.addEventListener('pointerdown', (e) => {
    if (e.button > 0 || P.pose) return;
    $('stage').focus({ preventScroll: true });
    const h = hitFor(e);
    if (!h) return;
    e.preventDefault();
    try { dev.setPointerCapture(e.pointerId); } catch (err) { /* ignore */ }
    endPress();
    const inter = topEntry().inter;
    inter.down = h.id;
    inter.ring = false;
    const p = press = { id: h.id, act: h.act, inter, held: false, timer: 0 };
    p.timer = setTimeout(() => {
      if (press !== p || !(p.act && p.act.hold)) return;
      p.held = true;
      p.inter.down = 0;
      p.act.hold();
      renderScreen();
    }, 500);
    renderScreen();
  });
  dev.addEventListener('pointerup', (e) => {
    const p = press;
    if (!p) return;
    const h = hitFor(e);
    endPress();
    if (!p.held && h && h.id === p.id) {
      p.inter.focus = p.id;
      fireTap({ id: p.id, act: p.act });
    }
    renderScreen();
  });
  dev.addEventListener('pointercancel', () => { if (press) { endPress(); renderScreen(); } });

  $('stage').addEventListener('keydown', (e) => {
    if (P.pose || !lastUi || e.altKey || e.ctrlKey || e.metaKey) return;
    const inter = topEntry().inter;
    const dir = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' }[e.key];
    const keys = lastUi.keys || {};
    const focusHot = () => lastUi.hots.find((x) => x.id === inter.focus);
    if (keys.text && e.key.length === 1) {
      e.preventDefault();
      inter.ring = true;
      keys.text(e.key);
      renderScreen();
    } else if (keys[e.key] && !e.shiftKey) {
      e.preventDefault();
      inter.ring = true;
      keys[e.key]();
      renderScreen();
    } else if (e.key === ' ' || (e.key === 'Enter' && e.shiftKey)) {
      // The alternate action: what a hold does on the touch screen.
      e.preventDefault();
      const h = focusHot();
      if (h && h.act && h.act.hold) { inter.ring = true; h.act.hold(); }
      renderScreen();
    } else if (dir) {
      e.preventDefault();
      if (!inter.ring) {
        inter.ring = true;
        if (!lastUi.hots.some((h) => h.id === inter.focus)) inter.focus = hotMove(lastUi.hots, 0, dir);
      } else {
        const pg = lastUi.paged;
        const to = pg && inter.focus >= pg.base && inter.focus < pg.base + pg.count
          ? pageStep(dir, inter.focus - pg.base, pg.count, pg.cols, pg.per) : -1;
        if (to >= 0) { pg.go(to); inter.focus = pg.base + to; }
        else inter.focus = hotMove(lastUi.hots, inter.focus, dir);
      }
      renderScreen();
    } else if (e.key === 'Enter') {
      e.preventDefault();
      const h = lastUi.hots.find((x) => x.id === inter.focus);
      if (h) { inter.ring = true; fireTap(h); }
      renderScreen();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      inter.ring = true;
      app.back();
      renderScreen();
    }
  });

  $('facts').addEventListener('click', (e) => {
    const pop = e.target.closest('[data-pop]');
    if (pop) { stack.length = Number(pop.dataset.pop) + 1; app.note = ''; }
    else if (e.target.closest('#reset-state')) {
      app.state = {}; app.note = ''; selectScreen(stack[0].scr);
    } else return;
    renderScreen();
    $('stage').focus({ preventScroll: true });
  });
}

function renderCompare() {
  const panel = curPanel(), theme = curTheme(), top = topEntry();
  const host = $('cmp-grid');
  host.className = `cmp${P.cmp === 'side' ? ' side' : ''}`;
  host.textContent = '';
  for (const key of Object.keys(FONTS)) {
    const font = FONTS[key], [cols, rows] = gridOf(font, panel);
    const fig = document.createElement('figure');
    const cap = document.createElement('figcaption');
    const cv = document.createElement('canvas');
    cv.setAttribute('role', 'img');
    cv.setAttribute('aria-label', `${top.scr.title || top.scr.name} at ${key}`);
    let note = '';
    try {
      const ui = new Ui(cols, rows, newInter(), 0);
      top.scr.draw(ui, app);
      const res = render(ui.frame, font, theme.table, renderOpts(panel, true));
      paint(cv, res, panel.width, panel.height);
      const clips = ui.issues.filter((i) => i.kind === 'clip').length;
      note = ` | clipped ${clips} | missing glyphs ${res.missing.length}`;
    } catch (e) { note = ` | draw failed: ${e.message}`; }
    cap.textContent = `${key} | grid ${cols} x ${rows}${note}`;
    fig.append(cap, cv);
    host.appendChild(fig);
  }
}

function renderStyles() {
  const font = curFont(), theme = curTheme();
  const LAB = 12, GW = 14, SAMPLE = ' Ag▄▀ ';
  const cols = LAB + ACCENTS * GW, rows = 2 + STYLE_COUNT * 2 - 1;
  const ui = new Ui(cols, rows);
  ui.fill(rect(0, 0, cols, rows), 0x20, S.TEXT);
  for (let a = 0; a < ACCENTS; a++) {
    const x = LAB + a * GW;
    ui.puts(x, 0, ACCENT_NAMES[a], S.TEXT | BOLD);
    ui.puts(x, 1, 'regular', S.MUTED);
    ui.puts(x + 7, 1, 'bold', S.MUTED);
  }
  for (let s = 0; s < STYLE_COUNT; s++) {
    const y = 2 + s * 2;
    ui.puts(0, y, DATA.styles[s], S.TEXT);
    for (let a = 0; a < ACCENTS; a++) {
      ui.pen(a);
      ui.puts(LAB + a * GW, y, SAMPLE, s);
      ui.puts(LAB + a * GW + 7, y, SAMPLE, s | BOLD);
    }
    ui.pen(C.DEFAULT);
  }
  const W = cols * font.w, H = rows * font.h;
  const res = render(ui.frame, font, theme.table, { width: W, height: H, scanlines: P.scan, mono: P.mono, strict: false });
  paint($('styles-canvas'), res, W, H);

  let h = '<table class="tbl"><thead><tr><th>Style</th>' +
    ACCENT_NAMES.map((n) => `<th>${n}</th>`).join('') + '</tr></thead><tbody>';
  const sw = (p) => `<span class="sw" style="--sw:${css565(p)}"></span>`;
  for (let s = 0; s < STYLE_COUNT; s++) {
    h += `<tr><th>${esc(DATA.styles[s])}</th>`;
    for (let a = 0; a < ACCENTS; a++) {
      const fg = theme.table[(s * ACCENTS + a) * 2], bg = theme.table[(s * ACCENTS + a) * 2 + 1];
      h += `<td><span class="hexcell">${sw(fg)}fg ${hex4(fg)}</span><span class="hexcell">${sw(bg)}bg ${hex4(bg)}</span></td>`;
    }
    h += '</tr>';
  }
  $('styles-hex').innerHTML = `${h}</tbody></table>`;
}

const GLYPH_RANGES = [
  ['Basic Latin', 0x20, 0x7F], ['Latin-1', 0x80, 0xFF], ['Latin Extended', 0x100, 0x24F],
  ['Greek', 0x370, 0x3FF], ['Cyrillic', 0x400, 0x4FF],
  ['Punctuation, arrows, math', 0x2000, 0x23FF], ['Box drawing', 0x2500, 0x257F],
  ['Block elements', 0x2580, 0x259F], ['Geometric shapes', 0x25A0, 0x25FF],
  ['Braille', 0x2800, 0x28FF],
];
let glyphState = { font: null, buckets: [] };

function glyphBuckets(font, prefix) {
  const buckets = GLYPH_RANGES.map(([name, from, to]) => ({ name, from, to, list: [] }));
  const other = { name: 'Other', from: 0, to: 0, list: [] };
  for (const cp of font.codepoints().sort((a, b) => a - b)) {
    if (prefix && !hex4(cp).startsWith(prefix)) continue;
    (buckets.find((b) => cp >= b.from && cp <= b.to) || other).list.push(cp);
  }
  return [...buckets, other].filter((b) => b.list.length);
}

function renderGlyphs() {
  const font = curFont(), theme = curTheme(), box = $('glyph-groups');
  const prefix = $('glyph-filter').value.replace(/^\s*(u\+|0x)/i, '').replace(/[^0-9a-f]/gi, '').toUpperCase();
  const avail = Math.max(Math.floor(box.clientWidth), font.w * 4);
  const perRow = Math.max(2, Math.floor(avail / (2 * font.w)));
  const buckets = glyphBuckets(font, prefix);
  glyphState = { font, buckets, perRow };
  box.textContent = '';
  if (!buckets.length) { box.innerHTML = '<p class="none">No glyph matches.</p>'; return; }
  buckets.forEach((b, bi) => {
    const n = b.list.length, nr = Math.ceil(n / perRow), cols = perRow * 2, rows = nr * 2 - 1;
    const ui = new Ui(cols, rows);
    ui.fill(rect(0, 0, cols, rows), 0x20, S.TEXT);
    b.list.forEach((cp, i) => ui.putch((i % perRow) * 2, Math.floor(i / perRow) * 2, cp, S.TEXT));
    const W = cols * font.w, H = rows * font.h;
    const res = render(ui.frame, font, theme.table, { width: W, height: H, scanlines: P.scan, mono: P.mono, strict: true });
    const head = document.createElement('h3');
    head.className = 'ghead';
    head.innerHTML = `${esc(b.name)} <span>${n} glyph${n === 1 ? '' : 's'}${b.to ? `, U+${hex4(b.from)}-${hex4(b.to)}` : ''}</span>`;
    const cv = document.createElement('canvas');
    cv.dataset.bi = bi;
    cv.setAttribute('role', 'img');
    cv.setAttribute('aria-label', `${b.name} glyphs`);
    paint(cv, res, W, H);
    box.append(head, cv);
  });
}

function showGlyph(e) {
  const cv = e.target.closest && e.target.closest('canvas');
  if (!cv || cv.dataset.bi === undefined) return;
  const { font, buckets, perRow } = glyphState;
  const b = buckets[Number(cv.dataset.bi)], r = cv.getBoundingClientRect();
  const x = Math.floor((e.clientX - r.left) * cv.width / r.width / font.w);
  const y = Math.floor((e.clientY - r.top) * cv.height / r.height / font.h);
  if (x % 2 || y % 2) return;
  const cp = b.list[(y / 2) * perRow + x / 2];
  if (cp === undefined) return;
  const bold = font.hasBold(cp), cells = bold ? 2 : 1;
  const ui = new Ui(cells, 1);
  ui.fill(rect(0, 0, cells, 1), 0x20, S.TEXT);
  ui.putch(0, 0, cp, S.TEXT);
  if (bold) ui.putch(1, 0, cp, S.TEXT | BOLD);
  const W = cells * font.w, H = font.h;
  const res = render(ui.frame, font, curTheme().table, { width: W, height: H, scanlines: P.scan, mono: P.mono, strict: true });
  const big = $('ginfo-canvas');
  paint(big, res, W, H);
  big.style.width = `${W * 4}px`;
  big.style.height = `${H * 4}px`;
  $('ginfo-text').innerHTML = `U+${hex4(cp)} (${cp})<br>bold form: ${bold ? 'yes' : 'no'}<br>` +
    `<span class="muted">${esc(b.name)}${bold ? ', regular | bold' : ''}</span>`;
}

let queued = false;
function renderAll() {
  syncToolbar();
  if (P.tab === 'screen') renderScreen();
  else if (P.tab === 'compare') renderCompare();
  else if (P.tab === 'styles') renderStyles();
  else renderGlyphs();
}
function renderSoon() {
  if (queued) return;
  queued = true;
  requestAnimationFrame(() => { queued = false; renderAll(); });
}

function setTab(t) {
  P.tab = t;
  savePrefs();
  for (const n of TABS) {
    $(`view-${n}`).hidden = n !== t;
    if (n === t) $(`tab-${n}`).setAttribute('aria-current', 'page');
    else $(`tab-${n}`).removeAttribute('aria-current');
  }
  renderAll();
}

function init() {
  const first = MO_SKETCHES[0];
  if (first) selectScreen(first);
  buildToolbar();
  buildPicker();
  wirePointer();
  for (const n of TABS) {
    $(`tab-${n}`).addEventListener('click', (e) => {
      e.preventDefault();
      try { if (location.hash !== `#${n}`) history.replaceState(null, '', `#${n}`); } catch (err) { /* sandboxed */ }
      setTab(n);
    });
  }
  window.addEventListener('hashchange', () => {
    const t = location.hash.slice(1);
    if (TABS.includes(t) && t !== P.tab) setTab(t);
  });
  $('glyph-filter').addEventListener('input', renderGlyphs);
  $('glyph-groups').addEventListener('pointermove', (e) => { if (e.pointerType === 'mouse') showGlyph(e); });
  $('glyph-groups').addEventListener('click', showGlyph);
  if (typeof ResizeObserver === 'function') {
    new ResizeObserver(() => {
      if (P.tab !== 'glyphs') return;
      const f = curFont(), w = Math.floor($('glyph-groups').clientWidth);
      if (w > 0 && Math.max(2, Math.floor(w / (2 * f.w))) !== glyphState.perRow) renderSoon();
    }).observe($('glyph-groups'));
  }
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    if (P.tab === 'screen') layoutStage();
  });
  setTab(P.tab);
}

init();
