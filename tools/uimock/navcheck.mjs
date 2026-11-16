// navcheck.mjs — proves a sketch works from the keyboard alone. From the
// first focus it follows the four arrow keys through every region.
// A region the arrows never reach fails the check.
//
//   node navcheck.mjs [prefix]      default prefix: mo-

import { Ui, newInter, hotMove, ID_BACK } from './kit.js';
import { MO_SKETCHES } from './sketches-mo.js';
import { PROFILES32, WIFI_UP, WIFI_DOWN, WIFI_FULL, KBD_NONE, KBD_FULL } from './fixtures.js';

const GRIDS = [['12x24', 66, 20], ['10x20', 80, 24], ['8x16', 100, 30]];
// States that show parts the first draw hides.
const STATES = {
  'mo-home': [{}, { hosts: PROFILES32 }, { hosts: PROFILES32, page: 1, pair: true },
    { hosts: PROFILES32, page: 3 }],
  'mo-section': [0, 1, 2, 3, 4, 5, 6].map((cat) => ({ cat }))
    .concat([{ cat: 5, spage: 1 }, { cat: 5, spage: 2 }, { cat: 6, spage: 1 },
      { cat: 0, hosts: PROFILES32.slice(0, 5) }, { cat: 0, hosts: PROFILES32, max: 40 },
      { cat: 0, hosts: PROFILES32, max: 40, spage: 1 }, { cat: 0, hosts: [] },
      { cat: 0, live: true }, { cat: 1, wifi: WIFI_DOWN }, { cat: 1, wifi: WIFI_FULL },
      { cat: 1, wifi: WIFI_FULL, spage: 1 }, { cat: 1, wifi: WIFI_FULL, spage: 3 },
      { cat: 2, kbd: KBD_NONE }, { cat: 2, kbd: KBD_FULL }, { cat: 2, kbd: KBD_FULL, spage: 1 },
      { cat: 2, kbd: KBD_FULL, spage: 3 }]),
  'mo-kbd': [{ kbdi: 0 }, { kbdi: 1 }, { kbdi: 3, kbd: KBD_FULL }],
  'mo-net': [{ net: 0 }, { net: 1 }, { net: 2 }, { net: 0, wifi: { ...WIFI_UP, nets: WIFI_UP.nets.slice(0, 1) } }],
  'mo-list': [{ lpage: 0 }, { lpage: 1 }],
  'mo-pair': [{}, { n: 2 }, { n: 8 }, { n: 8, page: 1 }],
  'mo-unlock': [{}, { code: '12' }, { flow: true, phase: 'new', code: 'ab7' }],
  'mo-wifiadd': [{}, { wa: { ssid: 'Workshop', pass: 'short', show: false } },
    { wa: { ssid: 'Workshop', pass: 'longenough', show: true } }],
  'mo-edit': [{}, { host: -1 }, { host: 3 }, { host: 3, keys: [] }],
};
const prefix = process.argv[2] || 'mo-';

let total = 0, bad = 0;
for (const scr of MO_SKETCHES.filter((s) => s.name.startsWith(prefix))) {
  for (const [font, cols, rows] of GRIDS) {
    for (const state of STATES[scr.name] || [{}]) {
      total++;
      const app = { state: { ...state }, note: '', fg: 0xFFFF, bg: 0, go() {}, back() {}, home() {} };
      const inter = newInter(), ui = new Ui(cols, rows, inter);
      scr.draw(ui, app);
      const ids = [...new Set(ui.hots.map((h) => h.id))];
      const seen = new Set([inter.focus]), queue = [inter.focus];
      while (queue.length) {
        const at = queue.pop();
        for (const dir of ['up', 'down', 'left', 'right']) {
          const to = hotMove(ui.hots, at, dir);
          if (!seen.has(to)) { seen.add(to); queue.push(to); }
        }
      }
      // Escape is the back bar's key, so the arrows need not reach it.
      const lost = ids.filter((id) => !seen.has(id) && id !== ID_BACK);
      if (lost.length) {
        bad++;
        console.log(`FAIL ${scr.name} ${font} ${JSON.stringify(state)}: arrows never reach ${lost.join(', ')}`);
      }
    }
  }
}
console.log(`${total - bad}/${total} poses: every region reachable by arrow keys`);
process.exit(bad ? 1 : 0);
