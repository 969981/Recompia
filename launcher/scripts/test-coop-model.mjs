// Tests for what the co-op Home says (src/coop-model.js). Run: node scripts/test-coop-model.mjs   (exit 1 on a failure)
import { askOnce, hosting, mismatchText, needsPlayers, others, roleLine, runningLine } from '../src/coop-model.js';

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
