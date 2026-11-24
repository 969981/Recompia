// Tests for what the co-op Home says (src/coop-model.js). Run: node scripts/test-coop-model.mjs   (exit 1 on a failure)
import { askOnce, choosePad, chooseScreen, hosting, isSplit, mismatchText, needsPlayers, others, roleLine, runningLine, windowPads, windowScreens } from '../src/coop-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const A = { id: 'a', name: 'Ash' }, B = { id: 'b', name: 'Misty' }, C = { id: 'c', name: 'Brock' };
const save = (members, me, host, peers = false) => ({ profile: { id: 'g', name: 'Island', members, me }, host: { host, role: host === me ? 'host' : 'visit', peers_known: peers }, creator: members[0].id === me });

// the creator hosts while nothing is known; the reason says so on both PCs
const asA = save([A, B], 'a', 'a');
const asB = save([A, B], 'b', 'a');
check(roleLine(asA, 'auto').startsWith('You host, because you made this save'), 'creator hosts: says why');
check(roleLine(asB, 'auto') === 'Ash hosts, because they made this save. Press Play once Ash is in the game.', 'visitor names the creator as host');
// the newest copy hosts once stamps are known
const newest = save([A, B, C], 'b', 'c', true);
check(roleLine(newest, 'auto').startsWith('Brock hosts, because their copy of the world is the newest'), 'newest copy named');
check(roleLine(save([A, B], 'b', 'b', true), 'auto').startsWith('You host, because your copy'), 'own newest copy');
// an override wins and says what the others must pick
check(hosting(asB, 'host') && !hosting(asA, 'visit') && hosting(asA, 'auto') && !hosting(asB, 'auto'), 'hosting follows the override');
check(roleLine(asB, 'host').includes('Join, not Auto'), 'host override asks the others for Join: on Auto the creator would host too');
check(roleLine(asA, 'visit').startsWith('You join.'), 'join override line');
// while running
check(runningLine(asA, 'auto').startsWith('You are hosting'), 'running as host');
check(runningLine(asB, 'auto') === 'Joining Ash. Your game waits until Ash is in the game.', 'running as visitor names the host');
check(runningLine(asA, 'visit') === 'Joining. Your game waits until the host is in the game.', 'a forced join does not guess the host');
// players
check(needsPlayers(save([A], 'a', 'a')) && !needsPlayers(asA), 'one player is not enough');
check(others(newest).join() === 'Ash,Brock', 'the others, in list order');
// a split screen save: every player here, nobody to wait for
{
  const couch = save([A, B, C], 'a', 'a');
  couch.profile.split = true;
  check(isSplit(couch) && !isSplit(asA), 'split screen save is marked');
  check(windowPads(3).join() === '1,2,3', 'controllers in window order by default');
  check(windowPads(3, [0]).join() === '0,1,2', 'keyboard for window 1: the controllers start at window 2');
  check(windowPads(2, [2, 2]).join() === '2,1', 'a controller twice: the later window gets a free one');
  check(windowPads(2, [0, 0, 3]).join() === '0,1', 'one keyboard; a choice for a window that is not there is dropped');
  check(windowPads(2, [9, 'x']).join() === '1,2', 'nonsense is replaced');
  check(choosePad(3, [1, 2, 3], 1, 0).join() === '1,0,3', 'window 2 takes the keyboard');
  check(choosePad(3, [0, 1, 2], 2, 0).join() === '2,1,0', 'the keyboard moves: the window that had it gets the controller back');
  check(windowScreens(3).join() === '1,2,3', 'screens in window order by default');
  check(windowScreens(3, [2, 2, 9]).join() === '2,2,3', 'two windows may share a screen; nonsense is replaced');
  check(chooseScreen(2, [], 0, 2).join() === '2,2', 'window 1 moves to screen 2, window 2 stays');
  check(roleLine(couch, 'auto').startsWith('Each player plays in a window of their own') && runningLine(couch, 'auto').startsWith('The windows find each other'), 'split lines name no host');
}
// a hidden host
check(mismatchText({ mismatch: 'mods' }).title.includes('mods') && mismatchText({ mismatch: 'build' }).title.includes('version'), 'mismatch titles');

// a lookup is asked once per text; a failure is asked again, except one that is kept
{
  let calls = 0;
  const answers = { ok: () => Promise.resolve('long'), down: () => Promise.reject({ code: null }), gone: () => Promise.reject({ code: 'invite_not_found' }) };
  const ask = askOnce((k) => { calls++; return answers[k](); }, (e) => e?.code === 'invite_not_found');
  const run = async (k) => ask(k).then((v) => v, (e) => e);
  for (const k of ['ok', 'ok', 'down', 'down', 'gone', 'gone']) await run(k);
  check(calls === 4, `asked once for ok and gone, twice for down (${calls} calls)`);
}

console.log(bad ? `${bad} failed` : 'coop model tests passed');
process.exit(bad ? 1 : 0);
