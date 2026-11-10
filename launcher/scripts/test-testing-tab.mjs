// Run: node scripts/test-testing-tab.mjs (no browser, no Tauri). The Testing tab's pure parts.
import assert from 'node:assert/strict';
import { FIELDS, SECTIONS, readAll, rowsOf, sectionsFor } from '../src/settings-schema.js';
import { REPORT_URL, filterLines, sizeText, zipName } from '../src/testing-model.js';

// the tab is for the Testing channel only
assert.ok(sectionsFor('testing').some((s) => s.id === 'testing'));
assert.ok(!sectionsFor('stable').some((s) => s.id === 'testing'));
assert.equal(sectionsFor('stable').length, SECTIONS.length - 1);

// the experimental toggles are in the tab, off by default, and no field id is used twice for different keys
const sec = SECTIONS.find((s) => s.id === 'testing');
const ids = sec.groups.flatMap((g) => g.rows).map((r) => r.id);
const EXTRAS = ['camera_aim', 'jump_momentum', 'inventory_sort', 'craft_search', 'title_quit', 'menu_in_game', 'taller_fov', 'touch_button', 'compile_note'];
for (const id of EXTRAS) {
  assert.ok(ids.includes(id), id);
  assert.equal(FIELDS[id].def, '0', `${id} defaults off`);
}
const empty = { pkp: {}, controls: {}, config: { channel: 'testing' }, controls_exists: false };
const testing = readAll(empty);
assert.equal(testing.camera_aim, false);
assert.equal(readAll({ ...empty, pkp: { 'pkp.camera_aim': '1' } }).camera_aim, true);

// each extra shows in the Testing tab on Testing and in Game > Gameplay otherwise, never in both
const shown = (secId, values) => SECTIONS.find((s) => s.id === secId).groups.flatMap((g) => rowsOf(g, values, {})).map((r) => r.id);
const onTesting = { ...testing, channel: 'testing' };
const onStable = { ...testing, channel: 'stable' };
for (const id of EXTRAS) {
  assert.ok(shown('testing', onTesting).includes(id) && !shown('game', onTesting).includes(id), `${id} on testing`);
  assert.ok(!shown('testing', onStable).includes(id) && shown('game', onStable).includes(id), `${id} on stable`);
}

// the model
assert.match(REPORT_URL, /^https:\/\/github\.com\/.+\/issues\/new\?template=bug_report\.yml$/);
assert.ok(!REPORT_URL.includes('&'), 'the launcher opens links without &');
assert.deepEqual([sizeText(0), sizeText(900), sizeText(2048), sizeText(3 << 20)], ['0 B', '900 B', '2 KB', '3.0 MB']);
assert.equal(filterLines('a Error x\nfine\nERROR y\nwarn error', 'error'), 'a Error x\nERROR y\nwarn error');
assert.equal(filterLines('a b\nb\na', 'a b'), 'a b');
assert.equal(filterLines('keep\nall', '  '), 'keep\nall');
assert.equal(zipName('C:\\g\\testing-logs\\x.zip'), 'x.zip');
assert.equal(zipName('/g/testing-logs/x.zip'), 'x.zip');
console.log('testing tab tests passed');
