// export.mjs — refresh data/ from the C side. Runs the lab once per font:
// glyph tables, baked style tables, posed frames and their screenshots.
// Run it after any change to the kit, the theme or a font, then run
// check.mjs and kitcheck.mjs.
//
//   node tools/uimock/export.mjs [path to cyberdeck_uilab]

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const exe = process.platform === 'win32' ? 'cyberdeck_uilab.exe' : 'cyberdeck_uilab';
const lab = process.argv[2] || join(here, '..', '..', 'build-sim', 'sim', exe);
if (!existsSync(lab)) {
  console.error(`no lab binary at ${lab}\nbuild the cyberdeck_uilab target, or pass its path`);
  process.exit(1);
}

const data = join(here, 'data');
for (const size of ['8x16', '10x20', '12x24']) {
  const ref = join(data, `ref-${size}`);
  mkdirSync(ref, { recursive: true });
  const r = spawnSync(lab, ['--font', size, '--out', ref, '--dump', data],
                      { stdio: ['ignore', 'ignore', 'pipe'], encoding: 'utf8' });
  if (r.status !== 0) {
    console.error(r.stderr);
    console.error(`${size}: lab exited ${r.status}`);
    process.exit(1);
  }
  console.log(`${size}: dumped`);
}
