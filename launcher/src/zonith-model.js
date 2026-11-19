// Zonith, the pure part: what its Home's main button does and what its rail tile says, from zonith_status (src-tauri/src/zonith.rs) and
// the last zonith-progress. The launcher fetches Zonith's newest release, keeps it up to date and opens its setup window, which builds
// the game from the player's files and starts it. No DOM and no Tauri here, so node scripts/test-zonith-model.mjs runs it.
//
// status: { supported, releases: [{ tag, name, date, body, url, prerelease }], latest: { tag, name, date, url, asset, size } | null,
//           release_error: 'not_public' | 'offline' | null, installed: { tag, path } | null, update, job: progress | null, running }
// progress: { state: 'download' | 'unpack' | 'done' | 'error' | 'cancelled', done, total, pct, message }
import { t } from './i18n.js';

/** A download or unpack is under way. */
export const busy = (z) => !!z?.job && ['download', 'unpack'].includes(z.job.state);

/** The version a tag or release name stands for, without the v ('v0.5.0' -> '0.5.0'). */
export const versionOf = (tag) => String(tag ?? '').replace(/^v(?=\d)/i, '');

const pctOf = (job) => (job?.pct == null ? null : Math.max(0, Math.min(100, Math.round(job.pct))));

/**
 * The Home's main button: { label, icon, run: 'play' | 'download' | 'retry' | null, disabled, busy, hint }. `hint`: one short line under it
 * (the size of a download, why it is off), or ''.
 */
export function heroAction(z) {
  if (!z) return { label: t('Loading'), icon: 'loader-circle', run: null, disabled: true, busy: true, hint: '' };
  if (!z.supported) return { label: t('Not available'), icon: 'circle-x', run: null, disabled: true, busy: false, hint: t('Zonith runs on Windows and Linux.') };
  if (busy(z)) {
    const pct = pctOf(z.job);
    const what = z.job.state === 'unpack' ? t('Unpacking') : t('Downloading');
    return { label: pct == null ? what : `${what} ${pct}%`, icon: 'loader-circle', run: null, disabled: true, busy: true, hint: '' };
  }
  if (z.running) return { label: t('Running'), icon: 'loader-circle', run: null, disabled: true, busy: true, hint: '' };
  if (z.installed && z.update && z.latest) {
    return { label: t('Update'), icon: 'download', run: 'download', disabled: false, busy: false, hint: sizeLine(z.latest.size) };
  }
  if (z.installed) return { label: t('Play'), icon: 'play', run: 'play', disabled: false, busy: false, hint: '' };
  if (z.latest) return { label: t('Install'), icon: 'download', run: 'download', disabled: false, busy: false, hint: sizeLine(z.latest.size) };
  if (z.release_error === 'not_public') return { label: t('Coming soon'), icon: 'clock', run: null, disabled: true, busy: false, hint: t('Zonith is not out yet. It shows up here the day it is.') };
  return { label: t('Try again'), icon: 'refresh-cw', run: 'retry', disabled: false, busy: false, hint: t('Could not reach GitHub to find Zonith.') };
}

/** "1.5 GB download" for a size in bytes, or '' when unknown. */
export function sizeLine(bytes) {
  if (!bytes) return '';
  const gb = bytes / 2 ** 30;
  const size = gb >= 1 ? `${gb.toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 2 ** 20))} MB`;
  return t('{size} download', { size });
}

/** The rail tile's badge: { text, busy } or null. */
export function zonithBadge(z) {
  if (!z || !z.supported) return null;
  if (busy(z)) { const pct = pctOf(z.job); return { text: pct == null ? '...' : `${pct}%`, busy: true }; }
  if (z.installed && z.update) return { text: t('Update') };
  if (!z.installed && z.latest) return { text: t('New') };
  return null;
}

/** The releases What's New lists: newest first, prereleases left out unless asked for. */
export function notesOf(z, { prerelease = false } = {}) {
  return (z?.releases ?? []).filter((r) => prerelease || !r.prerelease);
}
