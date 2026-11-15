// Choose a build: the list of builds a channel offers, with their notes, and the questions asked before an older one goes in. Used when
// someone joins Testing (the Testing builds are listed right away) and from Settings > About > Choose a version. The install itself is
// store.installVersion (the same download, check and restart as Update); the backend refuses an older build that was not confirmed.
import './news.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { store } from '../store.js';
import { confirm, esc, icon } from '../ui.js';
import { formatDate, render as renderNote } from '../markdown.js';
import { actionLabel, canConfirm, forChannel, versionOf, warningFor } from '../builds-model.js';

const relationMark = (b) => (b.relation === 'installed' ? t('Installed') : b.relation === 'older' ? t('Older') : b.prerelease ? t('Testing') : '');

/** The builds of a channel (works for Testing before the channel is saved). Resolves [] when they cannot be loaded. */
export async function loadBuilds(channel, refresh = false) {
  try {
    return forChannel(await api.listBuilds({ channel, refresh }), channel);
  } catch (e) {
    console.warn('list_builds', e);
    return [];
  }
}

function row(b) {
  const mark = relationMark(b);
  const off = b.relation === 'installed' || !b.installable;
  return `<li class="build-row" data-tag="${esc(b.tag)}">
    <div class="build-head">
      <div class="build-title">
        <span class="build-name"><strong>${esc(versionOf(b))}</strong>${mark ? `<span class="note-mark">${esc(mark)}</span>` : ''}</span>
        <time>${esc(formatDate(b.date))}</time>
      </div>
      <button class="btn ${b.relation === 'newer' && b.installable ? 'primary' : ''}" type="button" data-install="${esc(b.tag)}" ${off ? 'disabled' : ''}>${esc(actionLabel(b))}</button>
    </div>
    <details class="build-notes"><summary>${esc(t('Notes'))}</summary><div class="md selectable">${renderNote(b.notes, { title: versionOf(b), warn: !!b.prerelease })}</div></details>
  </li>`;
}

/** The list dialog. Resolves the picked build, or null (Not now, Esc, click outside). */
export function pickBuild({ title, intro, builds, close = t('Not now') }) {
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong builds-dialog';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <div class="modal-body">
        <h2 id="modal-title">${esc(title)}</h2>
        <p>${esc(intro)}</p>
        ${builds.length ? `<ul class="build-list">${builds.map(row).join('')}</ul>` : `<p>${esc(t('No builds could be loaded. Check your connection and try again.'))}</p>`}
      </div>
      <div class="modal-actions"><button class="btn quiet" type="button" data-answer="">${esc(close)}</button></div>`;
    document.body.appendChild(dlg);
    let picked = null;
    dlg.addEventListener('click', (e) => {
      const install = e.target.closest('[data-install]');
      if (install && !install.disabled) {
        picked = builds.find((b) => b.tag === install.dataset.install) ?? null;
        dlg.close('picked');
      } else if (e.target.closest('[type="button"][data-answer]') || e.target === dlg) dlg.close('');
    });
    dlg.addEventListener('close', () => {
      dlg.remove();
      resolve(dlg.returnValue === 'picked' ? picked : null);
    });
    dlg.showModal();
  });
}

const reasons = (b) => `<ul class="modal-list">${(b.reasons ?? []).map((r) => `<li>${icon('triangle-alert')}<span>${esc(r)}</span></li>`).join('')}</ul>`;
const SAFE = () => t('Your saves and settings are kept, and a copy of them is made first in the rollback-backups folder next to the game, so you can put them back by hand.');

/** The strong warning: the button stays off until the box is ticked. Resolves true when confirmed. */
function strongWarning(b) {
  const w = warningFor(b);
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <div class="modal-body">
        <h2 id="modal-title">${esc(t('Go back to {version}?', { version: versionOf(b) }))}</h2>
        <p><strong>${esc(t('This is a long way back.'))}</strong> ${esc(t('Things from the newer version may not work in the older one, and your saves, profiles or the folder layout can end up in a state the older version does not understand.'))}</p>
        ${reasons(b)}
        <p>${esc(SAFE())}</p>
        <label class="build-tick"><input type="checkbox" id="build-tick" /><span>${esc(t('I understand my saves, profiles and settings may not work in the older version.'))}</span></label>
      </div>
      <div class="modal-actions">
        <button class="btn quiet" type="button" data-answer="">${esc(t('Cancel'))}</button>
        <button class="btn danger" type="button" data-answer="ok" disabled>${esc(w.ok)}</button>
      </div>`;
    document.body.appendChild(dlg);
    const tick = dlg.querySelector('#build-tick'), go = dlg.querySelector('[data-answer="ok"]');
    tick.addEventListener('change', () => { go.disabled = !canConfirm(w, tick.checked); });
    dlg.addEventListener('click', (e) => {
      const a = e.target.closest('[data-answer]');
      if (a && !a.disabled) dlg.close(a.dataset.answer);
      else if (e.target === dlg) dlg.close('');
    });
    dlg.addEventListener('close', () => {
      dlg.remove();
      resolve(dlg.returnValue === 'ok' && canConfirm(w, tick.checked));
    });
    dlg.showModal();
    dlg.querySelector('[data-answer=""]').focus();
  });
}

/** The question before installing `b`. Resolves { accepted } to go on, or null to stop. A newer build needs no question. */
export async function confirmBuild(b) {
  const w = warningFor(b);
  if (w.kind === 'none') return { accepted: false };
  if (w.kind === 'high') return (await strongWarning(b)) ? { accepted: true } : null;
  const yes = await confirm({
    title: t('Go back to {version}?', { version: versionOf(b) }),
    html: `<p>${esc(t('The game and launcher files are replaced by the ones of this older build.'))} ${esc(SAFE())}</p>${reasons(b)}`,
    ok: w.ok,
    danger: true,
  });
  return yes ? { accepted: true } : null;
}

/** The builds of a channel with their notes; installs the one the player picks, after the question an older build needs. */
export async function chooseBuild(channel, join) {
  const builds = await loadBuilds(channel, true);
  const picked = await pickBuild(join
    ? { title: t('Testing builds'), intro: t('You are on the Testing channel now. Pick a build to install, or choose Not now and pick one later in Settings > About > Choose a version.'), builds }
    : { title: t('Choose a version'), intro: channel === 'testing' ? t('Testing builds and the normal releases. Newer builds install like an update; an older one asks first.') : t('The releases you can install. A newer one installs like an update; an older one asks first.'), builds, close: t('Close') });
  if (!picked) return;
  const ok = await confirmBuild(picked);
  if (!ok) return;
  await store.installVersion(picked.tag, channel, ok.accepted);
}
