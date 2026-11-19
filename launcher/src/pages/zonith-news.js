// Zonith's What's New: its releases from GitHub (zonith_status `releases`), in the same two panes as Recompia's (news.css): the list left,
// the note right. The note of a release newer than the one on this PC has the Update button; the one on this PC is marked.
import './news.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { errText, esc, icon, toast } from '../ui.js';
import { store } from '../store.js';
import { formatDate, render as renderNote } from '../markdown.js';
import { busy, notesOf, versionOf } from '../zonith-model.js';

const isGithub = (url) => /^https:\/\/github\.com\//.test(url ?? '');

function item(r, z) {
  const mark = z.installed?.tag === r.tag ? `<span class="note-mark">${esc(t('Installed'))}</span>`
    : z.update && z.latest?.tag === r.tag ? `<span class="note-mark new">${esc(t('Update'))}</span>`
    : r.prerelease ? `<span class="note-mark">${esc(t('Testing'))}</span>` : '';
  const date = formatDate(r.date);
  return `<button class="note-item" data-tag="${esc(r.tag)}">
    <span class="note-item-title clip"><strong>${esc(versionOf(r.tag))}</strong></span>
    <span class="note-item-sub">${date ? `<time>${esc(date)}</time>` : ''}${mark}</span>
  </button>`;
}

function head(r, z) {
  const date = formatDate(r.date, 'long');
  const newer = z.latest?.tag === r.tag && (z.update || !z.installed);
  const action = newer
    ? `<button class="btn primary" data-act="download" ${busy(z) || store.state.zonithStarting ? 'disabled' : ''}>${icon(busy(z) ? 'loader-circle' : 'download', busy(z) ? 'spin' : '')}${esc(z.installed ? t('Update') : t('Install'))}</button>`
    : '';
  const open = isGithub(r.url) ? `<button class="btn quiet icon-only" data-url="${esc(r.url)}" aria-label="${esc(t('Open on GitHub'))}" title="${esc(t('Open on GitHub'))}">${icon('external-link')}</button>` : '';
  return `<div class="grow"><h1>Zonith ${esc(versionOf(r.tag))}</h1>${date ? `<time>${esc(date)}</time>` : ''}</div>${open}${action}`;
}

export function render(el, ctx) {
  el.innerHTML = `<div class="page news-page">
    <div class="news glass" id="split" hidden>
      <div class="note-list" id="list" role="navigation" aria-label="${esc(t('Releases'))}"></div>
      <article class="note" id="note" tabindex="0">
        <header class="note-head" id="head"></header>
        <div class="md selectable" id="body"></div>
      </article>
    </div>
    <div class="empty glass" id="empty" hidden>
      <span id="empty-text"></span>
      <button class="btn" id="retry">${icon('refresh-cw')}${esc(t('Try again'))}</button>
    </div>
  </div>`;

  const part = (id) => el.querySelector(`#${id}`);
  const patch = (node, html) => {
    if (node.dataset.html !== html) {
      node.innerHTML = html;
      node.dataset.html = html;
    }
  };
  let tag = ctx.tag ?? null;
  let shown = null;
  let checking = false;
  const items = () => [...part('list').querySelectorAll('.note-item')];

  function draw(s) {
    const z = s.zonith;
    const list = notesOf(z, { prerelease: true });
    part('split').hidden = !list.length;
    part('empty').hidden = !!list.length || checking || !z;
    if (!list.length) {
      part('empty-text').textContent = z?.release_error === 'not_public' ? t('Zonith is not out yet. Its patch notes show up here the day it is.') : t('Patch notes load when you are online.');
      part('retry').hidden = z?.release_error === 'not_public';
      return;
    }
    const r = list.find((x) => x.tag === tag) ?? list[0];
    const changed = r.tag !== shown;
    shown = r.tag;
    patch(part('list'), list.map((x) => item(x, z)).join(''));
    patch(part('head'), head(r, z));
    patch(part('body'), renderNote(r.body, { title: versionOf(r.tag), warn: r.prerelease }));
    for (const b of items()) {
      const on = b.dataset.tag === shown;
      b.toggleAttribute('aria-current', on);
      b.tabIndex = on ? 0 : -1;
      if (on && changed) b.scrollIntoView({ block: 'nearest' });
    }
    if (changed) part('note').scrollTop = 0;
  }

  async function check() {
    checking = true;
    part('retry').disabled = true;
    draw(store.state);
    await store.refreshZonith(true);
    checking = false;
    part('retry').disabled = false;
    draw(store.state);
  }

  function select(next, focus) {
    tag = next;
    draw(store.state);
    if (focus) items().find((b) => b.dataset.tag === next)?.focus();
  }

  el.addEventListener('click', (e) => {
    const hit = e.target.closest('[data-tag], [data-url], [data-act], #retry');
    if (!hit) return;
    if (hit.id === 'retry') return check();
    if (hit.dataset.tag) return select(hit.dataset.tag);
    if (hit.dataset.act === 'download') return store.zonithDownload();
    if (hit.dataset.url) api.openLink({ url: hit.dataset.url }).catch((err) => toast(errText(err), 'error'));
  });

  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && e.target.matches?.('a[data-url]')) return e.target.click();
    if (!e.target.closest?.('#list')) return;
    const all = items();
    const at = all.findIndex((b) => b.dataset.tag === shown);
    const next = { ArrowDown: at + 1, ArrowUp: at - 1, Home: 0, End: all.length - 1 }[e.key];
    if (next === undefined) return;
    e.preventDefault();
    if (all[next]) select(all[next].dataset.tag, true);
  });

  ctx.on(draw);
  if (notesOf(store.state.zonith, { prerelease: true }).length) draw(store.state);
  else check();
}
