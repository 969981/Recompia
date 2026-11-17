// What the co-op Home (pages/coop.js) says about a co-op save: who hosts this start and why, what to do before and while playing, and
// why a start found nobody. A save is the view coop_cmds.rs CoopView sends: { profile, host: { host, role, own, peers_known }, folder,
// creator, saved_at, created }. Pure: no DOM and no Tauri, so node scripts/test-coop-model.mjs runs it.
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

/** This PC hosts this start. */
export const hosting = (v, startAs) => (startAs === 'auto' ? v.host.role === 'host' : startAs === 'host');

/** Who hosts this start and what this player does, before Play. With Auto every PC reaches the same answer (coop_save.rs choose_host):
 *  the creator while the copies' stamps are unknown, then the newest copy. */
export function roleLine(v, startAs) {
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
  if (hosting(v, startAs)) return t('You are hosting. The others can press Play now.');
  const host = startAs === 'auto' ? nameOf(v, v.host.host) : '';
  return host ? t('Joining {name}. Your game waits until {name} is in the game.', { name: host }) : t('Joining. Your game waits until the host is in the game.');
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
