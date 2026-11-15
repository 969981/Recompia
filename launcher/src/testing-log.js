// The Testing changelog: which entries make up the Testing branch's history, and the one note that sums them up. Pure functions on the
// release list the launcher already has (cached, so no network): the page in pages/news.js only draws what they return.
// A Stable player's list has no prereleases, so for them every function here returns nothing and What's New is as it always was.
import { t } from './i18n.js';
import { formatDate, isNewer, parseReleaseName, section } from './markdown.js';

/** The tag of the overview entry. Not a real release: news.js lists it first among the Testing builds. */
export const OVERVIEW_TAG = '__testing';

const versionOf = (r) => parseReleaseName(r.name, r.tag).version;

const newestFirst = (a, b) => (isNewer(versionOf(a), versionOf(b)) ? -1 : isNewer(versionOf(b), versionOf(a)) ? 1 : 0);

/** Every prerelease, from every test line (Testing betas, alpha side lines), newest first. */
export function allTestingBuilds(releases) {
  return (Array.isArray(releases) ? releases : []).filter((r) => r.prerelease).sort(newestFirst);
}

/** The Testing builds the overview sums up: prereleases newer than the newest stable release, newest first. */
export function testingBuilds(releases) {
  const list = Array.isArray(releases) ? releases : [];
  const stable = list.filter((r) => !r.prerelease).map(versionOf).reduce((best, v) => (best === null || isNewer(v, best) ? v : best), null);
  return allTestingBuilds(list).filter((r) => stable === null || isNewer(versionOf(r), stable));
}

/**
 * The note that sums up every Testing build since the last stable release, newest first: for each, the date, what changed in that
 * build, and its regressions and known problems. A build whose notes have none of them gets its first lines instead.
 */
export function overviewNote(builds) {
  if (!builds.length) return '';
  const parts = builds.map((r) => {
    const date = formatDate(r.date);
    const regressions = section(r.body, 'Regressions') || section(r.body, 'Known problems');
    const changes = section(r.body, 'Changes in this build');
    const asked = section(r.body, 'What to test');
    const body = [regressions, asked, changes].filter(Boolean).join('\n\n') || String(r.body ?? '').trim().split('\n').slice(0, 8).join('\n');
    return `### ${versionOf(r)}${date ? ` (${date})` : ''}\n\n${body}`;
  });
  return `${parts.join('\n\n---\n\n')}\n`;
}

/** The overview as an entry for the list, or null when there are no Testing builds. */
export function overviewEntry(releases) {
  const builds = testingBuilds(releases);
  if (!builds.length) return null;
  return {
    tag: OVERVIEW_TAG,
    virtual: true,
    name: t('All Testing changes'),
    date: builds[0].date,
    prerelease: true,
    body: overviewNote(builds),
    count: builds.length,
  };
}

/**
 * The list as the page draws it: [{ id, label, items }]. With prereleases: "Testing" (the overview of the builds since the last stable
 * release when there are any, then every prerelease of every test line) and "Releases" (everything else). Without: one unlabelled group
 * with the list as it is.
 */
export function groups(releases) {
  const list = Array.isArray(releases) ? releases : [];
  const builds = allTestingBuilds(list);
  if (!builds.length) return [{ id: 'releases', label: '', items: list }];
  const tags = new Set(builds.map((r) => r.tag));
  const overview = overviewEntry(list);
  return [
    { id: 'testing', label: t('Testing'), items: [...(overview ? [overview] : []), ...builds] },
    { id: 'releases', label: t('Releases'), items: list.filter((r) => !tags.has(r.tag)) },
  ];
}
