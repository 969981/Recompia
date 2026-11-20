// What the co-op Home (pages/coop.js) says about a co-op save: who hosts this start and why, what to do before and while playing, and
// why a start found nobody. A save is the view coop_cmds.rs CoopView sends: { profile, host: { host, role, own, peers_known }, folder,
// creator, saved_at, created }; profile.split marks a split screen save, whose players all play on this PC, one window each (window i
// plays members[i - 1]). Pure: no DOM and no Tauri, so node scripts/test-coop-model.mjs runs it.
import { t } from './i18n.js';

/** How a player starts one session (coop_save.rs StartAs): the launcher's pick, or host / join because they said so. Never stored. */
export const START_AS = ['auto', 'host', 'visit'];
export const MAX_PLAYERS = 4;   // coop.rs MAX_MEMBERS

export const nameOf = (v, id) => v.profile.members.find((m) => m.id === id)?.name ?? '';
export const creatorName = (v) => v.profile.members[0]?.name ?? '';
/** The other players' names, in list order. */
export const others = (v) => v.profile.members.filter((m) => m.id !== v.profile.me).map((m) => m.name);
/** A save of one player cannot be played together yet: the creator adds the others first (the game would wait for nobody). */
export const needsPlayers = (v) => v.profile.members.length < 2;

/** A split screen save: every player plays on this PC, so there is no invite, no relay and no host to wait for. */
export const isSplit = (v) => !!v.profile.split;

/** The most controllers a split screen save hands out: one per window (coop.rs MAX_MEMBERS). */
const MAX_PADS = 4;
const padOk = (p) => Number.isInteger(p) && p >= 0 && p <= MAX_PADS;

/** What each of `n` windows plays with (coop_launch.rs SplitOptions pads): 0 the keyboard and mouse, k the k-th controller connected.
 *  `wanted` is what the player chose, possibly for another number of windows: a window keeps its choice unless an earlier window has
 *  it already, and a window without one gets the first controller nobody plays. Only one window has the keyboard and mouse. */
export function windowPads(n, wanted = []) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const w = wanted[i];
    out.push(padOk(w) && !out.includes(w) ? w : null);
  }
  return out.map((p, i) => p ?? (out[i] = [1, 2, 3, MAX_PADS].find((k) => !out.includes(k))));
}

/** Window `i` now plays with `pad`: a window that had it takes window i's old one, so no two windows share a controller. */
export function choosePad(n, pads, i, pad) {
  const now = windowPads(n, pads);
  const other = now.indexOf(pad);
  if (other >= 0 && other !== i) now[other] = now[i];
  now[i] = pad;
  return now;
}

/** This PC hosts this start. */
export const hosting = (v, startAs) => (startAs === 'auto' ? v.host.role === 'host' : startAs === 'host');

/** Who hosts this start and what this player does, before Play. With Auto every PC reaches the same answer (coop_save.rs choose_host):
 *  the creator while the copies' stamps are unknown, then the newest copy. */
export function roleLine(v, startAs) {
  if (isSplit(v)) return t('Each player plays in a window of their own on this PC. No internet needed.');
  if (startAs === 'host') return t('You host. Start first, and ask the others to pick Join, not Auto.');
  if (startAs === 'visit') return t('You join. Another player must host: press Play once they are in the game.');
  const host = nameOf(v, v.host.host);
  const byCreator = v.host.host === v.profile.members[0]?.id;
  if (v.host.role === 'host') {
    return byCreator ? t('You host, because you made this save. Start first; the others press Play once you are in the game.')
      : t('You host, because your copy of the world is the newest. Start first; the others press Play once you are in the game.');
  }
  return byCreator ? t('{name} hosts, because they made this save. Press Play once {name} is in the game.', { name: host })
    : t('{name} hosts, because their copy of the world is the newest. Press Play once {name} is in the game.', { name: host });
}

/** While the game of this save runs: the launcher cannot see the session, so it says what happens next, not that anyone is connected. */
export function runningLine(v, startAs) {
  if (isSplit(v)) return t('The windows find each other on their own once every player is in the game.');
  if (hosting(v, startAs)) return t('You are hosting. The others can press Play now.');
  const host = startAs === 'auto' ? nameOf(v, v.host.host) : '';
  return host ? t('Joining {name}. Your game waits until {name} is in the game.', { name: host }) : t('Joining. Your game waits until the host is in the game.');
}

/** `ask(key)` once per key: the same promise again while it holds. A failure is forgotten, so the next call asks again, unless `keep(error)`
 *  (an answer that will not change, like "no invite has this code"). */
export function askOnce(ask, keep) {
  const asked = new Map();
  return (key) => {
    if (!asked.has(key)) {
      const p = ask(key).catch((e) => {
        if (!keep(e) && asked.get(key) === p) asked.delete(key);
        throw e;
      });
      asked.set(key, p);
    }
    return asked.get(key);
  };
}

/** Why the last start of this save found no host to join (coop_diag.rs Finding), as a title and what to do. */
export function mismatchText(f) {
  if (f.mismatch === 'build') {
    return { title: t('The host runs another version of Recompia'), body: t('Update both PCs to the same version, then start again.') };
  }
  return {
    title: t('The host has different mods on'),
    body: t('Co-op only joins players with the same gameplay mods. On both PCs, open Mods in Co-op, pick this save and switch on the same mods, then start again.'),
  };
}
