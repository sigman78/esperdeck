// baseline.js — the lab's example screens (sim/uilab.c) on kit.js. A
// redesign starts from them. They also prove the port: kitcheck.mjs
// compares each one, cell for cell, with the lab's dump.
// A screen is draw(ui, app). `app` carries state and navigation, so a
// tap in the viewer moves between screens. `pose` is the lab's
// screenshot pose.

import {
  S, C, G, BOLD, CARD, INPUT, LEFT, WIDE, COMPACT,
  rect, cutTop, cutBottom, cutLeft, center, grid, gridCell, gridCapacity,
  pageCount, pageClamp, profAccent, spinnerGlyph,
} from './kit.js';

export const ID = {
  PROFILE: 100, NEW: 150, LOCK: 151, SETTINGS: 152, HUB: 200, VALUE: 300,
  ROW: 400, FIELD: 500, AUTH: 510, SAVE: 511, CANCEL: 512, KEY: 600,
  PIN: 700, OK: 800, NO: 801, PREV: 900, NEXT: 901,
};

export const PROFILES = [
  { name: 'homelab', where: 'root@192.168.1.10:22' },
  { name: 'build-box', where: 'ci@build.lan:22  key' },
  { name: 'pi-garage', where: 'pi@10.0.0.42:22' },
  { name: 'vps-fra', where: 'deploy@fra1.example.net:2222  key' },
  { name: 'router', where: 'admin@192.168.1.1:22' },
  { name: 'nas', where: 'backup@nas.lan:22  key' },
  { name: 'printer', where: 'octo@10.0.0.77:22' },
  { name: 'bastion', where: 'ops@bastion.example.org:22  key' },
];

export const TERM_LINES = [
  'deck@homelab:~$ journalctl -fu sshd',
  'Nov 12 14:02:11 homelab sshd[811]: Accepted publickey for deck',
  'Nov 12 14:02:11 homelab sshd[811]: pam_unix(sshd:session): opened',
  'Nov 12 14:03:40 homelab systemd[1]: Started Daily apt upgrade.',
  'Nov 12 14:04:02 homelab kernel: wlan0: associated',
  'deck@homelab:~$ htop',
  '  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+',
  '  811 deck       20   0 16.2M 6304K 5120K S  0.7  0.3  0:00.42',
  '    1 root       20   0  167M 11.9M 8448K S  0.0  0.6  0:02.10',
];

// Stand-in for the shell's StatusBar: chips left, clock right.
export function labStatus(ui, r, note = '') {
  ui.pen(C.BLUE);
  for (let x = r.x; x < r.x + r.w; x++) ui.putch(x, r.y, ' ', S.BAR);
  let x = r.x + 1;
  ui.pen(C.GREEN);
  ui.puts(x, r.y, ' NET ', S.FOCUS | BOLD);
  x += 5;
  const STAIR = [0x2582, 0x2584, 0x2586, 0x2588];
  for (let i = 0; i < 4; i++) ui.putch(x++, r.y, i < 3 ? STAIR[i] : ' ', S.FOCUS | BOLD);
  ui.putch(x++, r.y, ' ', S.FOCUS);
  ui.pen(C.BLUE);
  ui.putch(x++, r.y, ' ', S.BAR);
  ui.pen(C.CYAN);
  ui.puts(x, r.y, ' KBD ', S.FOCUS | BOLD);
  x += 5;
  ui.pen(C.AMBER);
  ui.putch(x++, r.y, 'C', S.FOCUS | BOLD);
  ui.pen(C.CYAN);
  ui.puts(x, r.y, 'N ', S.WELL | BOLD);
  x += 3;
  if (note) {
    ui.pen(C.BLUE);
    ui.text(rect(x, r.y, r.x + r.w - 9 - x, 1), note, S.BAR, LEFT);
  }
  ui.pen(C.BLUE);
  ui.puts(r.x + r.w - 8, r.y, ' 14:07 ', S.FOCUS | BOLD);
  ui.pen(C.DEFAULT);
}

// HOME at the storage limit (8 profiles), three ways. `row`: a bottom
// action row, else Settings is one more tile. `two`: two-line cards.
function homeVariant(ui, app, row, two) {
  const m = ui.metrics(), p = ui.page(), st = app.state;
  const body = { ...p.body };
  const acts = row ? cutBottom(body, m.unit) : rect(0, 0, 0, 0);
  const g = grid(body, m.cls === WIDE ? 3 : 2, two ? 4 : m.unit, m.gutter, 0);
  const count = PROFILES.length + (row ? 0 : 1);
  const per = gridCapacity(g), pages = pageCount(count, per);
  st.page = pageClamp(st.page | 0, count, per);

  const head = ui.pager(p.header, ID.PREV, ID.NEXT, st.page, pages, {
    prev: { tap: () => st.page-- }, next: { tap: () => st.page++ } });
  ui.header(head, null, 'CYBERDECK', pages > 1 ? null : 'v0.9', false);

  const settings = { tap: () => app.go('hub') };
  for (let slot = 0; slot < per; slot++) {
    const i = st.page * per + slot;
    if (i >= count) break;
    if (i === PROFILES.length) {
      ui.card(gridCell(g, slot), ID.SETTINGS, {
        title: 'Settings', detail: two ? 'wifi - profiles - keys' : null,
        accent: C.CYAN, flags: CARD.NAV }, settings);
      continue;
    }
    ui.card(gridCell(g, slot), ID.PROFILE + i, {
      title: PROFILES[i].name,
      detail: two ? PROFILES[i].where : null,
      value: two ? null : PROFILES[i].where,
      accent: profAccent(PROFILES[i].name) },
      { tap: () => app.go('connect'), hold: () => app.go('form') });
  }
  if (row) {
    const a = grid(acts, 3, m.unit, m.gutter, 0);
    ui.btn(gridCell(a, 0), ID.NEW, '+ New profile', C.GREEN, { tap: () => app.go('form') });
    ui.btn(gridCell(a, 1), ID.LOCK, 'Lock deck', C.AMBER, { tap: () => app.go('pin') });
    ui.btn(gridCell(a, 2), ID.SETTINGS, 'Settings', C.CYAN, settings);
  }
  labStatus(ui, p.status, app.note);
}

function hub(ui, app) {
  const HUB = [
    ['PROFILES', 'add - edit - order'], ['WIFI', 'HomeAP  -55 dBm'],
    ['KEYBOARD', 'K380 connected'], ['CRT FX', 'scanlines - phosphor'],
    ['MOTION FX', 'glow - wipe'], ['FONT', '8x16  100x30'],
    ['SYSTEM', 'saver - reset'], ['KEYSTORE', 'unlocked'],
  ];
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, null, 'SETTINGS', null, true, { tap: () => app.back() });
  const g = grid(p.body, 2, 4, m.gutter, 0);
  HUB.forEach(([t, d], i) => ui.card(gridCell(g, i), ID.HUB + i, {
    title: t, detail: d, accent: C.CYAN, flags: CARD.NAV },
    { tap: () => app.go(i === 0 ? 'list' : 'section') }));
  labStatus(ui, p.status, app.note);
}

function section(ui, app) {
  const ITEMS = [
    ['Scanlines', 'on', 0], ['Phosphor', 'color', 0], ['Bold pop', 'on', 0],
    ['Glow build', 'strong', 0], ['Screen saver', '5 min', 0],
    ['Edge scroll', 'off', CARD.DISABLED],
  ];
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, 'SETTINGS', 'CRT FX', null, true, { tap: () => app.back() });
  const g = grid(p.body, 2, m.unit, m.gutter, 0);
  ITEMS.forEach(([t, v, f], i) => ui.card(gridCell(g, i), ID.VALUE + i, {
    title: t, value: v, accent: C.CYAN, flags: f }));
  labStatus(ui, p.status, app.note);
}

function list(ui, app) {
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, 'PROFILES', 'EDIT', null, true, { tap: () => app.back() });
  const g = grid(p.body, 2, 4, m.gutter, 0);
  for (let i = 0; i < PROFILES.length && i < gridCapacity(g); i++)
    ui.card(gridCell(g, i), ID.ROW + i, {
      title: PROFILES[i].name, detail: PROFILES[i].where,
      accent: profAccent(PROFILES[i].name), flags: CARD.NAV },
      { tap: () => app.go('form') });
  labStatus(ui, p.status, app.note);
}

function form(ui, app) {
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, 'PROFILES', 'NEW PROFILE', null, true, { tap: () => app.back() });
  const body = { ...p.body }, acts = cutBottom(body, m.unit);
  const g = grid(body, 2, m.unit, m.gutter, 0);
  ui.input(gridCell(g, 0), ID.FIELD + 0, 'Name', 'homelab', 0, 0);
  ui.input(gridCell(g, 1), ID.FIELD + 1, 'Host', '192.168.1.', 10, INPUT.EDITING,
           { tap: () => app.go('osk') });
  ui.input(gridCell(g, 2), ID.FIELD + 2, 'Port', '22', 0, 0);
  ui.input(gridCell(g, 3), ID.FIELD + 3, 'User', 'root', 0, 0);
  ui.input(gridCell(g, 4), ID.AUTH, 'Auth', 'password', 0, INPUT.CYCLE);
  ui.input(gridCell(g, 5), ID.FIELD + 4, 'Pass', 'hunter2', 0, INPUT.MASK);
  const a = grid(center(acts, Math.trunc(acts.w * 2 / 3), m.unit), 2, m.unit, m.gutter, 0);
  ui.btn(gridCell(a, 0), ID.SAVE, 'Save', C.GREEN, { tap: () => app.back() });
  ui.btn(gridCell(a, 1), ID.CANCEL, 'Cancel', C.BLUE, { tap: () => app.back() });
  labStatus(ui, p.status, app.note);
}

// On-screen keyboard rows; \b backspace, \t shift, \n done, \x01 symbols.
const OSK_ROWS = ['qwertyuiop', 'asdfghjkl\b', '\tzxcvbnm.-', '\x01 @\n'];

function osk(ui, app) {
  const st = app.state;
  if (st.osk == null) st.osk = '192.168.1.';
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, 'NEW PROFILE', 'HOST', null, true, { tap: () => app.back() });
  let body = { ...p.body };
  ui.input(cutTop(body, m.unit), ID.FIELD, 'Host', st.osk, st.osk.length, INPUT.EDITING);
  const kw = Math.trunc(body.w / 10);
  body = center(body, kw * 10, 4 * m.unit);
  for (let r = 0; r < 4; r++) {
    const row = cutTop(body, m.unit);
    for (const k of OSK_ROWS[r]) {
      let label = k, flags = CARD.CENTER, accent = C.DEFAULT, w = kw;
      if (k >= 'a' && k <= 'z' && st.shift) label = k.toUpperCase();
      if (k === '\b') { label = '<-'; accent = C.AMBER; }
      if (k === '\t') { label = 'Aa'; accent = C.BLUE; }
      if (k === '\x01') { label = '123'; accent = C.BLUE; w = 2 * kw; }
      if (k === ' ') { label = 'space'; w = 5 * kw; }
      if (k === '\n') { label = 'Done'; accent = C.GREEN; flags |= CARD.ACTION; w = 2 * kw; }
      const key = cutLeft(row, w);
      key.w -= 1;
      ui.card(key, ID.KEY + k.charCodeAt(0), { title: label, accent, flags }, { tap: () => {
        if (k === '\b') st.osk = st.osk.slice(0, -1);
        else if (k === '\t') st.shift = !st.shift;
        else if (k === '\n') app.back();
        else if (k !== '\x01' && st.osk.length < 39) { st.osk += label === 'space' ? ' ' : label; st.shift = false; }
      } });
    }
  }
  labStatus(ui, p.status, app.note);
}

function pin(ui, app) {
  const PAD = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '<-', '0', 'OK'];
  const st = app.state;
  if (st.pin == null) st.pin = 3;
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, null, 'UNLOCK DECK', 'keystore', false);
  const body = { ...p.body }, entry = cutTop(body, m.unit);
  const slots = 6;
  let x = entry.x + Math.trunc((entry.w - (slots * 4 - 2)) / 2);
  for (let i = 0; i < slots; i++, x += 4) {
    const on = i < st.pin;
    ui.pen(on ? C.CYAN : C.BLUE);
    ui.putch(x, entry.y + 1, on ? G.LED_ON : G.LED_OFF, on ? S.TEXT : S.MUTED);
    ui.putch(x + 1, entry.y + 1, ' ', S.TEXT);
  }
  ui.pen(C.DEFAULT);
  const kw = m.cls === WIDE ? 14 : 12;
  const g = grid(center(body, 3 * kw + 2 * m.gutter, 4 * m.unit), 3, m.unit, m.gutter, 0);
  for (let i = 0; i < 12; i++) {
    let accent = C.DEFAULT, flags = CARD.CENTER;
    if (i === 9) accent = C.AMBER;
    if (i === 11) { accent = C.GREEN; flags |= CARD.ACTION; }
    ui.card(gridCell(g, i), ID.PIN + i, { title: PAD[i], accent, flags }, { tap: () => {
      if (i === 9) st.pin = Math.max(0, st.pin - 1);
      else if (i === 11) app.back();
      else st.pin = Math.min(slots, st.pin + 1);
    } });
  }
  labStatus(ui, p.status, app.note);
}

function dialog(ui, app) {
  ui.term(TERM_LINES, app.fg, app.bg);
  const m = ui.metrics(), w = m.cls === COMPACT ? 50 : 56;
  const inn = ui.dialog(w, 11, 'Disconnect session?', C.AMBER);
  ui.pen(C.DEFAULT);
  ui.text(rect(inn.x, inn.y, inn.w, 1), 'homelab is still running htop.', S.TEXT, LEFT);
  ui.text(rect(inn.x, inn.y + 1, inn.w, 1), 'The remote shell will be closed.', S.MUTED, LEFT);
  const acts = cutBottom(inn, m.unit), a = grid(acts, 2, m.unit, m.gutter, 0);
  ui.btn(gridCell(a, 0), ID.OK, 'Disconnect', C.RED, { tap: () => app.home() });
  ui.btn(gridCell(a, 1), ID.NO, 'Stay', C.BLUE, { tap: () => app.back() });
}

function connect(ui, app) {
  const STEPS = [
    ['wifi link', 1], ['tcp 192.168.1.10:22', 1],
    ['host key  SHA256:9fK2..Qm1c', 1], ['authenticating', 0],
  ];
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, null, 'CONNECTING', 'homelab', true, { tap: () => app.back() });
  const body = center(p.body, Math.trunc(p.body.w * 3 / 4), p.body.h);
  const acts = cutBottom(body, m.unit);
  cutTop(body, 1);
  for (const [t, done] of STEPS) {
    const row = cutTop(body, 2);
    ui.pen(done ? C.GREEN : C.AMBER);
    ui.putch(row.x, row.y, done ? G.CHECK : spinnerGlyph(3), S.TEXT | BOLD);
    ui.pen(C.DEFAULT);
    ui.text(rect(row.x + 3, row.y, row.w - 3, 1), t, done ? S.MUTED : S.TEXT | BOLD, LEFT);
  }
  ui.meter(cutTop(body, 1), 7, 10, C.GREEN);
  ui.btn(center(acts, Math.trunc(acts.w / 2), m.unit), ID.CANCEL, 'Cancel', C.BLUE,
         { tap: () => app.back() });
  labStatus(ui, p.status, app.note);
}

// Every card state side by side: the sheet that decides a rendition.
function kit(ui, app) {
  const m = ui.metrics(), p = ui.page();
  ui.header(p.header, 'KIT', 'CARD STATES', 'focus / press / off', true, { tap: () => app.back() });
  const g = grid(p.body, m.cls === WIDE ? 4 : 3, 4, m.gutter, 0);
  const CARDS = [
    ['Normal', 'title + detail', C.CYAN, 0], ['Focused', 'keyboard ring', C.CYAN, 0],
    ['Pressed', 'finger down', C.CYAN, 0], ['Disabled', 'no input', C.CYAN, CARD.DISABLED],
    ['Checked', 'current choice', C.GREEN, CARD.CHECKED],
    ['Opens', 'another page', C.MAGENTA, CARD.NAV],
    ['Amber', 'identity accent', C.AMBER, 0], ['Blue', 'identity accent', C.BLUE, 0],
  ];
  let n = 0;
  for (; n < CARDS.length && n < gridCapacity(g) - 3; n++)
    ui.card(gridCell(g, n), ID.HUB + n, {
      title: CARDS[n][0], detail: CARDS[n][1], accent: CARDS[n][2], flags: CARDS[n][3] });
  let b = gridCell(g, n++);
  ui.btn(rect(b.x, b.y, b.w, m.unit), ID.OK, 'Confirm', C.GREEN);
  b = gridCell(g, n++);
  ui.btn(rect(b.x, b.y, b.w, m.unit), ID.NO, 'Delete', C.RED);
  ui.card(gridCell(g, n++), ID.VALUE, { title: 'Scanlines', value: 'on', accent: C.CYAN });
  labStatus(ui, p.status, app.note);
}

function glyphs(ui, app) {
  const p = ui.page();
  ui.header(p.header, 'KIT', 'GLYPHS', 'U+2580..259F / box / marks', true, { tap: () => app.back() });
  const RANGES = [
    [0x2580, 0x259F], [0x2500, 0x251F], [0x2550, 0x256F], [0x25A0, 0x25CF],
    [0x2190, 0x2195], [0x2713, 0x2718], [0x2039, 0x203A], [0x2026, 0x2026],
    [0xE0B0, 0xE0B3],
  ];
  let x = p.body.x, y = p.body.y;
  for (const [from, to] of RANGES) {
    for (let cp = from; cp <= to; cp++) {
      ui.pen(C.CYAN);
      ui.putch(x, y, cp, S.TEXT);
      ui.putch(x + 1, y, cp, S.BAR);
      x += 3;
      if (x + 3 > p.body.x + p.body.w) { x = p.body.x; y += 2; }
    }
    x = p.body.x;
    y += 2;
    if (y >= p.body.y + p.body.h) break;
  }
  ui.pen(C.DEFAULT);
  labStatus(ui, p.status, app.note);
}

export const BASELINE = [
  { name: 'home', title: 'HOME A: action row, two-line cards', draw: (u, a) => homeVariant(u, a, true, true) },
  { name: 'home-tile', title: 'HOME B: Settings as a tile', draw: (u, a) => homeVariant(u, a, false, true) },
  { name: 'home-1line', title: 'HOME C: action row, one-line cards', draw: (u, a) => homeVariant(u, a, true, false) },
  { name: 'hub', title: 'Settings hub', draw: hub, pose: { focus: ID.HUB + 3 } },
  { name: 'section', title: 'Settings section (values)', draw: section, pose: { down: ID.VALUE + 2 } },
  { name: 'list', title: 'Profile picker', draw: list },
  { name: 'form', title: 'Profile editor', draw: form },
  { name: 'osk', title: 'On-screen keyboard (sketch)', draw: osk, pose: { down: ID.KEY + 0x67 } },
  { name: 'pin', title: 'Unlock pad', draw: pin },
  { name: 'dialog', title: 'Confirm dialog over a session', draw: dialog },
  { name: 'connect', title: 'Connect progress', draw: connect },
  { name: 'kit', title: 'Card states', draw: kit, pose: { focus: ID.HUB + 1, down: ID.HUB + 2 } },
  { name: 'glyphs', title: 'Block and box glyphs', draw: glyphs },
];
