// Tests for the games registry (src/games.js), what each game shows in Settings (settings-schema.js sectionsIn) and whose mod set the
// Mods page edits in each (mods-model.js scope helpers). Run: node scripts/test-games.mjs   (exit 1 on a failure)
import { DEFAULT_GAME, GAMES, PRODUCTS, gameForPage, gameOf, gamesOf, hasPage, productOf, shownIn, stepGame } from '../src/games.js';
import { FIELDS, SECTIONS, gameplayKeys, sectionsIn } from '../src/settings-schema.js';
import { keepScope, scopeArgs, scopeOf, scopeOptions } from '../src/mods-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

// the registry
const ids = GAMES.map((g) => g.id);
check(new Set(ids).size === ids.length, 'game ids are unique');
check(DEFAULT_GAME === 'recompia' && gameOf('recompia').id === 'recompia', 'Recompia is the default');
check(gameOf(undefined).id === 'recompia' && gameOf('').id === 'recompia' && gameOf('nope').id === 'recompia', 'an older launcher.json (no `game`) or an unknown one is Recompia');
for (const g of GAMES) {
  check(hasPage(g, g.home), `${g.id}: its Home is one of its pages`);
  check(['profiles', 'coop', null].includes(g.mods) && (g.mods === null) === !hasPage(g, 'mods'), `${g.id}: mods scope ${g.mods}`);
  check(PRODUCTS.some((p) => p.id === g.product && p.games.includes(g.id)), `${g.id}: belongs to its product`);
}
// the two levels: Recompia is one install played two ways; Zonith is an install of its own
check(PRODUCTS.flatMap((p) => p.games).sort().join() === ids.slice().sort().join(), 'every game is in exactly one product');
check(gamesOf('recompia').map((g) => g.id).join() === 'recompia,coop' && gamesOf('zonith').map((g) => g.id).join() === 'zonith', 'Recompia holds Solo and Co-op, Zonith itself');
check(productOf(gameOf('coop')).id === 'recompia' && productOf('zonith').name === 'Zonith', 'productOf');
check(GAMES.filter((g) => g.product === 'recompia').every((g) => hasPage(g, 'settings') && hasPage(g, 'news')), 'Recompia games have Settings and News');
check(stepGame('recompia', 1).id === 'coop' && stepGame('coop', 1).id === 'zonith' && stepGame('zonith', 1).id === 'recompia' && stepGame('recompia', -1).id === 'zonith', 'LT and RT walk the rail and wrap');
const zonith = gameOf('zonith');
check(zonith.pages.join() === 'zonith,zonith-news' && zonith.mods === null, "Zonith: its Home and its What's New, nothing of Recompia's");
check(gameForPage(zonith, 'settings') === gameOf('recompia'), "Settings from Zonith opens Recompia's, which holds the launcher's options");
check(gameOf('zonith').id === 'zonith', 'a saved Zonith choice loads');
const coop = gameOf('coop');
const recompia = gameOf('recompia');
check(coop.name === 'Recompia: Co-op' && GAMES.every((g) => g.tab && g.icon), 'Co-op is named "Recompia: Co-op"; every game has a sidebar tab with an icon');
check(['coop', 'mods', 'news', 'settings'].every((p) => hasPage(coop, p)) && coop.pages.length === 4, 'Co-op: Home, Mods, Settings, News only');
for (const p of ['setup', 'linkplay', 'saveedit', 'achievements']) check(!hasPage(coop, p) && hasPage(recompia, p), `${p} is Recompia only`);
check(PRODUCTS.every((p) => gamesOf(p).filter((g) => g.install).length === 1) && recompia.install && !coop.install && hasPage(recompia, 'setup'), 'one game per product looks after its install: Recompia (not Co-op), Zonith');
check(!hasPage(recompia, 'coop'),'the co-op Home is not in Recompia');
check(gameForPage(coop, 'setup') === recompia, 'Set up from Co-op is shown in Recompia');
check(gameForPage(coop, 'mods') === coop && gameForPage(recompia, 'coop') === coop, 'a page stays in a game that has it, else moves to its game');
check(gameForPage(coop, 'kit') === null, 'a page of no game stays where it is');

// Settings: the games filter
check(shownIn({}, 'coop') && shownIn({ games: ['recompia'] }, 'recompia') && !shownIn({ games: ['recompia'] }, 'coop'), 'shownIn');
const rowIds = (id) => sectionsIn(id).flatMap((s) => s.groups.flatMap((g) => g.rows.map((r) => r.id)));
const all = SECTIONS.flatMap((s) => s.groups.flatMap((g) => g.rows.map((r) => r.id)));
check(rowIds('recompia').length === all.length, 'Recompia shows every row');
const co = new Set(rowIds('coop'));
for (const id of ['online_presence', 'online_name', 'online_events', 'save_dir', 'backup_minutes', 'profiles_note', 'game_clock', 'setup_again', 'game_files', 'repair', 'without_mods', 'channel', 'updates', 'builds', 'steam_game', 'game_dir']) {
  check(!co.has(id), `Co-op hides ${id}`);
}
for (const id of ['window_mode', 'fps_limit', 'quality_preset', 'volume', 'sensitivity', 'prompt_style', 'language', 'version', 'minimize_on_play']) {
  check(co.has(id), `Co-op shows ${id}`);
}
// extras keep their switches in every game (PROJECT.md 6.1): the gameplay rows are never filtered
for (const key of gameplayKeys()) if (FIELDS[key]) check(co.has(key), `Co-op shows the gameplay row ${key}`);
check(!sectionsIn('coop').some((s) => s.id === 'saves'), 'Co-op has no Saves section');
check(sectionsIn('coop').every((s) => s.groups.length && s.groups.every((g) => g.rows.every((r) => shownIn(r, 'coop')))), 'Co-op sections hold only its rows');

// Mods: whose set the page edits
const groups = [{ id: 'g1', name: 'Island' }, { id: 'g2', name: 'Beach' }];
check(scopeOptions('coop', groups).map((o) => o[0]).join() === 'g1,g2', 'Co-op picks among its groups only, never the global defaults');
check(scopeOptions('profiles', [{ id: 'default', name: 'Me' }])[0][0] === '', 'Recompia starts with the global defaults');
check(keepScope('coop', groups, null) === 'g1' && keepScope('coop', groups, 'g2') === 'g2' && keepScope('coop', groups, 'gone') === 'g1', 'Co-op opens a group');
check(keepScope('coop', [], 'g1') === null && keepScope('profiles', groups, null) === null, 'no group: nothing; Recompia: global defaults');
check(JSON.stringify(scopeArgs('coop', 'g1')) === '{"coop":"g1"}' && JSON.stringify(scopeArgs('profiles', 'p')) === '{"profile":"p"}' && JSON.stringify(scopeArgs('coop', null)) === '{}', 'scope arguments');
check(scopeOf('coop', { coop: 'g1', profile: null }) === 'g1' && scopeOf('profiles', { profile: null }) === null, 'scope of a list');

console.log(bad ? `TEST FAILED (${bad})` : 'games tests passed');
process.exit(bad ? 1 : 0);
