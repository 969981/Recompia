// Which warning a build needs and whether it can be confirmed: pure, no DOM, so a plain node check runs it (pages/builds.js draws it).
// A build is what the launcher's list_builds returns: { tag, name, date, notes, url, prerelease, installable, relation, risk, reasons }.

import { t } from './i18n.js';

/** The version of a tag or a build name: "v0.8.1-beta.6" gives "0.8.1-beta.6". */
export const versionOf = (b) => String(b?.tag ?? '').replace(/^v/i, '');

/** The builds a channel may show. Stable never lists a Testing build, whatever the list holds. */
export const forChannel = (builds, channel) => (Array.isArray(builds) ? builds : []).filter((b) => channel === 'testing' || !b.prerelease);

/**
 * What to ask before installing `build`: { kind: 'none' | 'low' | 'high', needsTick, ok }.
 * none: a newer build, installed at once. low: an older build of the same release line, one question. high: an older release line (or one
 * that crosses a feature the older version lacks), a strong warning whose button stays off until the player ticks the box.
 */
export function warningFor(build) {
  const kind = build?.relation === 'older' ? (build.risk === 'high' ? 'high' : 'low') : build?.risk === 'high' ? 'high' : 'none';
  return { kind, needsTick: kind === 'high', ok: kind === 'high' ? t('Go back anyway') : kind === 'low' ? t('Go back') : t('Install') };
}

/** The confirm button is on when no tick is needed, or the box is ticked. */
export const canConfirm = (warning, ticked) => !warning.needsTick || !!ticked;

/** The label on a build's button. */
export function actionLabel(build) {
  if (build.relation === 'installed') return t('Installed');
  if (!build.installable) return t('Not available yet');
  return build.relation === 'older' ? t('Go back to this') : t('Install this build');
}
