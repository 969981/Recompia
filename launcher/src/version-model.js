// The channel and version picker next to Play (and in Settings > About): which channel the player is on, which version they picked and
// what the two dropdowns offer. Pure, no DOM, so a plain node check runs it (pages/version-picker.js draws it and does the installs).
//
// launcher.json (config.rs): `channel` "stable" | "experimental" (the retired "testing" reads as Stable, as in config.rs migrate) and
// `picked_version`, a release tag. Stable with no picked version is Latest: the Update button offers the newest release. A picked Stable
// version stays where it is. Experimental has no Latest: the player picks one prerelease, and nothing updates by itself there.
// A build is what list_builds returns: { tag, name, date, notes, url, prerelease, installable, relation, risk, reasons }.

import { t } from './i18n.js';
import { versionOf } from './builds-model.js';

export const STABLE = 'stable';
export const EXPERIMENTAL = 'experimental';
/** The Stable entry that follows the newest release. */
export const LATEST = '';

/** The channel launcher.json names: Experimental only when it says so. */
export const channelOf = (config) => (String(config?.channel ?? '').trim().toLowerCase() === EXPERIMENTAL ? EXPERIMENTAL : STABLE);

const bare = (v) => String(v ?? '').trim().replace(/^v/i, '');

/**
 * The version the player picked, '' for none (Latest on Stable, nothing picked yet on Experimental). A pick counts only while it is the
 * version running (`installed`), as in update_cmds.rs follows_latest: a pick whose install never finished, or one an older launcher
 * updated away from, is none. A retired Testing channel has none.
 */
export function pickedOf(config, installed) {
  if (String(config?.channel ?? '').trim().toLowerCase() === 'testing') return LATEST;
  const picked = String(config?.picked_version ?? '').trim();
  return picked && bare(picked) === bare(installed) ? picked : LATEST;
}

export const channelOptions = () => [{ value: STABLE, label: t('Stable') }, { value: EXPERIMENTAL, label: t('Experimental') }];

const labelOf = (b) => (b.relation === 'installed' ? t('{version} (installed)', { version: versionOf(b) }) : versionOf(b));

/** A build can be picked when it can be installed here, or when it is the one installed (picking it only keeps it). */
export const canPick = (b) => b.relation === 'installed' || !!b.installable;

/**
 * The version dropdown: { options: [{ value, label, disabled }], value }. Stable: Latest first (with the newest version it means), then
 * every release. Experimental: every prerelease; with none picked yet, a "Pick a build" line first that cannot be chosen again.
 * `builds`: the channel's list (list_builds), newest first.
 */
export function versionChoice(channel, builds, picked) {
  const list = (Array.isArray(builds) ? builds : []).filter((b) => (channel === EXPERIMENTAL ? b.prerelease : !b.prerelease));
  const items = list.map((b) => ({ value: b.tag, label: labelOf(b), disabled: !canPick(b) }));
  if (channel !== EXPERIMENTAL) {
    const newest = list.find((b) => b.installable);
    const latest = { value: LATEST, label: newest ? t('Latest ({version})', { version: versionOf(newest) }) : t('Latest'), disabled: false };
    const known = picked === LATEST || items.some((o) => o.value === picked);
    return { options: [latest, ...items, ...(known ? [] : [{ value: picked, label: picked.replace(/^v/i, ''), disabled: true }])], value: picked };
  }
  if (!list.length) return { options: [{ value: LATEST, label: t('No Experimental builds right now'), disabled: true }], value: LATEST };
  const known = items.some((o) => o.value === picked);
  if (picked && known) return { options: items, value: picked };
  const head = picked ? { value: picked, label: picked.replace(/^v/i, ''), disabled: true } : { value: LATEST, label: t('Pick a build'), disabled: true };
  return { options: [head, ...items], value: head.value };
}

/** What a pick means. 'keep': only the choice is saved (Latest, or the version already installed); 'install': that build goes in. */
export function pickKind(builds, tag) {
  if (tag === LATEST) return 'keep';
  const b = (Array.isArray(builds) ? builds : []).find((x) => x.tag === tag);
  if (!b || !canPick(b)) return 'none';
  return b.relation === 'installed' ? 'keep' : 'install';
}

/** The line under the dropdowns (Settings), or '' when there is nothing to say. */
export function pickerNote(channel, picked) {
  if (channel === EXPERIMENTAL) {
    return picked
      ? t('Experimental builds test new features and may have bugs. Nothing updates by itself here: pick each build yourself. Please tell us what you find and share pkp.log with Report a problem below. To leave, switch to Stable and pick Latest or a version.')
      : t('Pick an Experimental build to install. Until you do, you keep the version you have and nothing is installed.');
  }
  return picked ? t('You picked {version}: the launcher stays on it and offers no updates. Pick Latest to get updates again.', { version: picked.replace(/^v/i, '') }) : '';
}
