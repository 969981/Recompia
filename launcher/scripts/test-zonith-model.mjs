// Tests for Zonith's Home button and rail badge (src/zonith-model.js). Run: node scripts/test-zonith-model.mjs   (exit 1 on a failure)
import { busy, heroAction, notesOf, playable, sizeLine, versionOf, zonithBadge } from '../src/zonith-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const latest = { tag: 'v0.5.1', name: 'Zonith 0.5.1', size: 157_045_707 };
const base = { supported: true, releases: [], latest, release_error: null, installed: null, game: null, update: false, job: null, running: false };
const st = (patch) => ({ ...base, ...patch });

check(heroAction(null).disabled && heroAction(null).busy, 'loading: off and spinning');
check(heroAction(st({ supported: false })).disabled, 'macOS: off');
check(heroAction(st({})).run === 'download' && heroAction(st({})).label === 'Install', 'out, not here: Install');
check(heroAction(st({})).hint === '150 MB download', 'Install says the download size');
const installed = { tag: 'v0.5.1', path: 'x' };
const game = { path: 'g', release: '0.5.1', ready: true, stale: false };
check(heroAction(st({ installed })).run === 'setup' && heroAction(st({ installed })).label === 'Set up', 'the setup is here, no game yet: Set up');
check(heroAction(st({ installed, game: { ...game, ready: false } })).label === 'Set up', 'a game that is not built yet: Set up');
check(heroAction(st({ installed, game })).run === 'play' && heroAction(st({ installed, game })).label === 'Play', 'the game is built: Play starts it');
const old = st({ installed, game: { ...game, release: '0.5.0', stale: true } });
check(heroAction(old).run === 'setup' && heroAction(old).label === 'Finish update', 'the game is from an older setup: Finish update');
check(playable(st({ installed, game })) && !playable(old) && !playable(st({ game })) && !playable(null), 'playable');
check(heroAction(st({ installed: { tag: 'v0.5.0' }, update: true })).run === 'download' && heroAction(st({ installed: { tag: 'v0.5.0' }, update: true })).label === 'Update', 'here and older: Update');
const dl = st({ job: { state: 'download', pct: 41.6 } });
check(busy(dl) && heroAction(dl).disabled && heroAction(dl).label === 'Downloading 42%', 'downloading: off, with the percent');
check(heroAction(st({ job: { state: 'unpack', pct: null } })).label === 'Unpacking', 'unpacking with no percent');
check(!busy(st({ job: { state: 'done' } })) && !busy(st({ job: { state: 'error' } })), 'a finished job is not busy');
check(heroAction(st({ installed, game, running: true })).disabled, 'running: off');
check(heroAction(st({ latest: null, release_error: 'not_public' })).label === 'Coming soon' && heroAction(st({ latest: null, release_error: 'not_public' })).disabled, 'not public yet: Coming soon, off');
check(heroAction(st({ latest: null, release_error: 'offline' })).run === 'retry', 'offline: Try again');
check(heroAction(st({ latest: null, release_error: 'offline', installed, game })).run === 'play', 'offline but here: still Play');

check(zonithBadge(st({})).text === 'New' && zonithBadge(st({ installed })) === null, 'badge: New until it is here, then none');
check(zonithBadge(st({ installed: { tag: 'v0.5.0' }, update: true })).text === 'Update', 'badge: Update');
check(zonithBadge(old).text === 'Update' && zonithBadge(st({ installed, game })) === null, 'badge: Update until the setup ran on the game');
check(zonithBadge(dl).text === '42%' && zonithBadge(dl).busy, 'badge: the percent while downloading');
check(zonithBadge(st({ latest: null, release_error: 'not_public' })) === null && zonithBadge(null) === null, 'badge: none when not out or not loaded');

check(versionOf('v0.5.0') === '0.5.0' && versionOf('0.5.0') === '0.5.0' && versionOf('vNext') === 'vNext', 'versionOf');
check(sizeLine(0) === '' && sizeLine(3 * 2 ** 30) === '3.0 GB download', 'sizeLine');
const rel = [{ tag: 'v0.6.0-beta.1', prerelease: true }, { tag: 'v0.5.1', prerelease: false }];
check(notesOf(st({ releases: rel })).length === 1 && notesOf(st({ releases: rel }), { prerelease: true }).length === 2 && notesOf(null).length === 0, 'notesOf');

console.log(bad ? `TEST FAILED (${bad})` : 'zonith model tests passed');
process.exit(bad ? 1 : 0);
