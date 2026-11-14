// build.mjs — bundle the viewer into one Artifact page and a preview file.
//
//   node build.mjs      -> dist/uimock.html, dist/preview.html
//
// Needs data/ from `node export.mjs`. No dependencies.

import { readFileSync, writeFileSync, mkdirSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const here = dirname(fileURLToPath(import.meta.url));
const fail = (msg) => { console.error(`build: ${msg}`); process.exit(1); };
const read = (p) => readFileSync(join(here, p), 'utf8').replace(/\r\n/g, '\n');

// ---- data ------------------------------------------------------------------

const SIZES = ['8x16', '10x20', '12x24'];
const needed = SIZES.flatMap((s) => [`data/font-${s}.json`, `data/lab-${s}.json`]);
const absent = needed.filter((f) => !existsSync(join(here, f)));
if (absent.length) fail(`missing ${absent.join(', ')}. Run \`node export.mjs\` first.`);

const fonts = {};
for (const s of SIZES) fonts[s] = JSON.parse(read(`data/font-${s}.json`));

const labs = SIZES.map((s) => JSON.parse(read(`data/lab-${s}.json`)));
const themes = labs[0].themes, styles = labs[0].styles;
for (const l of labs) {
  if (JSON.stringify(l.themes) !== JSON.stringify(themes)) fail('themes differ between lab files; re-export.');
  if (JSON.stringify(l.styles) !== JSON.stringify(styles)) fail('style names differ between lab files; re-export.');
}
if (!themes || !themes.default || !themes.hazard) fail('lab files lack themes default/hazard.');
for (const [k, t] of Object.entries(themes))
  if (t.table.length !== styles.length * 16) fail(`theme ${k}: table is ${t.table.length} entries, want ${styles.length * 16}.`);

// ---- modules -----------------------------------------------------------------

const MODULES = ['model.js', 'kit.js', 'theme.js', 'baseline.js', 'sketches-mo.js', 'sketches.js', 'viewer.js'];

// Names declared at column 0: `const a = 1, b = 2;` yields a and b.
function topLevelNames(src) {
  const names = [];
  for (const line of src.split('\n')) {
    const m = /^(?:export\s+)?(?:async\s+)?(const|let|var|function\*?|class)\s+([A-Za-z_$][\w$]*)/.exec(line);
    if (!m) continue;
    names.push(m[2]);
    if (m[1] !== 'const' && m[1] !== 'let' && m[1] !== 'var') continue;
    let depth = 0, quote = '';
    for (let i = m[0].length; i < line.length; i++) {
      const ch = line[i];
      if (quote) { if (ch === '\\') i++; else if (ch === quote) quote = ''; continue; }
      if (ch === '\'' || ch === '"' || ch === '`') quote = ch;
      else if ('([{'.includes(ch)) depth++;
      else if (')]}'.includes(ch)) depth--;
      else if (ch === ',' && depth === 0) {
        const n = /^\s*([A-Za-z_$][\w$]*)\s*=/.exec(line.slice(i + 1));
        if (n) names.push(n[1]);
      }
    }
  }
  return names;
}

const owner = new Map();
const clashes = [];
const sources = MODULES.map((f) => {
  const src = read(f);
  for (const n of topLevelNames(src)) {
    if (owner.has(n)) clashes.push(`${n}: ${owner.get(n)} and ${f}`);
    else owner.set(n, f);
  }
  return [f, src];
});
if (clashes.length) fail(`top-level name collisions:\n  ${clashes.join('\n  ')}`);

function flatten(file, src) {
  let out = src.replace(/^import\b[^;]*?\bfrom\s*['"][^'"]+['"];?[ \t]*\n/gm, '');
  out = out.replace(/^export\s+(?=(?:async\s+)?(?:const|let|var|function|class)\b)/gm, '');
  const left = /^(import|export)\b.*$/m.exec(out);
  if (left) fail(`${file}: unhandled statement: ${left[0]}`);
  if (/<!--/.test(out)) fail(`${file}: contains "<!--", unsafe inside an inline script.`);
  return `// ---- ${file}\n${out.replace(/<\/script/gi, '<\\/script')}`;
}
const js = sources.map(([f, s]) => flatten(f, s)).join('\n');

// Syntax check of the exact text that ships.
const tmp = mkdtempSync(join(tmpdir(), 'uimock-build-'));
try {
  writeFileSync(join(tmp, 'bundle.mjs'), js);
  const r = spawnSync(process.execPath, ['--check', join(tmp, 'bundle.mjs')], { encoding: 'utf8' });
  if (r.status !== 0) fail(`bundle does not parse:\n${r.stderr}`);
} finally { rmSync(tmp, { recursive: true, force: true }); }

// ---- page --------------------------------------------------------------------

const BS = String.fromCharCode(92);
const json = JSON.stringify({ fonts, themes, styles })
  .replace(/</g, BS + 'u003c')
  .replace(new RegExp(String.fromCharCode(91, 0x2028, 0x2029, 93), 'g'), (c) => BS + 'u' + c.charCodeAt(0).toString(16));
const bundle = `<script type="application/json" id="model-data">${json}</script>\n` +
  `<script type="module">\n${js}</script>\n`;

const tpl = read('viewer.html');
if (!tpl.includes('<!--BUNDLE-->')) fail('viewer.html lacks the <!--BUNDLE--> placeholder.');
const bad = /<!doctype\b|<(?:html|head|body)[ \t\n>]/i.exec(tpl);
if (bad) fail(`viewer.html must not contain ${bad[0]}; the host wraps the page.`);
const page = tpl.replace('<!--BUNDLE-->', () => bundle);

const preview = '<!doctype html>\n<html lang="en"><head><meta charset="utf-8">' +
  '<meta name="viewport" content="width=device-width, initial-scale=1"></head>\n<body>\n' +
  `${page}</body></html>\n`;

mkdirSync(join(here, 'dist'), { recursive: true });
writeFileSync(join(here, 'dist', 'uimock.html'), page);
writeFileSync(join(here, 'dist', 'preview.html'), preview);
console.log(`dist/uimock.html ${(page.length / 1024).toFixed(0)} KB, dist/preview.html ${(preview.length / 1024).toFixed(0)} KB`);
