// Zonith's Home: its logo, one button that does the next right thing (Install, Update, Play: zonith-model.js heroAction) and its newest
// releases. Play opens Zonith's own setup window, which builds the game from the player's files and starts it.
import './home.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { brandImg } from '../branding.js';
import { errText, esc, icon, toast } from '../ui.js';
import { store } from '../store.js';
import { formatDate, highlights } from '../markdown.js';
import { busy, heroAction, notesOf, versionOf } from '../zonith-model.js';

const CARDS = 3;

function releaseCard(r, z) {
  const date = formatDate(r.date);
  const line = highlights(r.body, 1)[0] ?? '';
  const kicker = z.installed?.tag === r.tag ? t('Installed') : z.latest?.tag === r.tag ? t('Latest') : '';
  return `<button class="release glass hover" data-tag="${esc(r.tag)}">
    ${kicker ? `<span class="release-kicker">${esc(kicker)}</span>` : ''}
    <span class="release-head"><span class="release-title clip"><strong>Zonith ${esc(versionOf(r.tag))}</strong></span>${date ? `<time>${esc(date)}</time>` : ''}</span>
    ${line ? `<span class="release-line">${esc(line)}</span>` : ''}
  </button>`;
}

export function render(el, ctx) {
  el.innerHTML = `<div class="page zonith-home hero-page">
    <section class="hero">
      <h1 class="hero-title">${brandImg('logo', 'class="hero-logo zonith-logo" alt="Zonith"', 'zonith')}</h1>
      <div class="hero-actions">
        <button class="btn primary play" id="primary" data-hero-play></button>
        <button class="btn big on-art" id="cancel" data-act="cancel" hidden>${icon('x')}${esc(t('Cancel'))}</button>
        <button class="btn big on-art icon-only" id="folder" data-act="folder" hidden aria-label="${esc(t('Open folder'))}" title="${esc(t('Open folder'))}">${icon('folder-open')}</button>
      </div>
      <div class="hero-progress" id="progress" hidden><span class="hero-progress-fill" id="fill"></span></div>
      <p class="hero-sub" id="sub"></p>
    </section>
    <section class="home-feature"><div class="home-news" id="news" aria-label="${esc(t('Newest releases'))}"></div></section>
  </div>`;

  const part = (id) => el.querySelector(`#${id}`);
  const patch = (node, html) => {
    if (node.dataset.html !== html) {
      node.innerHTML = html;
      node.dataset.html = html;
    }
  };

  function draw(s) {
    const z = s.zonith;
    const a = heroAction(z);
    const button = part('primary');
    patch(button, `${icon(a.icon, a.busy ? 'spin' : '')}${esc(a.label)}`);
    button.disabled = a.disabled || s.zonithStarting;
    button.dataset.run = a.run ?? '';
    button.setAttribute('aria-busy', String(a.busy));

    part('cancel').hidden = !busy(z);
    part('folder').hidden = !z?.installed || busy(z);
    const job = busy(z) ? z.job : null;
    part('progress').hidden = !job;
    if (job) part('fill').style.width = `${Math.max(2, Math.min(100, job.pct ?? 0))}%`;
    // one short line: what is on this PC, else what the button needs to know
    const from = z?.installed ? versionOf(z.installed.tag) : '';
    part('sub').textContent = a.run === 'download' && from && z.latest ? `${from}  →  ${versionOf(z.latest.tag)}`
      : from && !busy(z) ? t('Version {version}', { version: from }) : a.hint;

    patch(part('news'), z ? notesOf(z).slice(0, CARDS).map((r) => releaseCard(r, z)).join('') : '');
  }

  el.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b || b.disabled) return;
    if (b.id === 'primary') {
      const run = b.dataset.run;
      if (run === 'play') return store.zonithPlay();
      if (run === 'download') return store.zonithDownload();
      if (run === 'retry') return store.refreshZonith(true);
    }
    if (b.dataset.tag) return ctx.go('zonith-news', { tag: b.dataset.tag });
    if (b.dataset.act === 'cancel') return store.zonithCancel();
    if (b.dataset.act === 'folder') api.zonithOpenDir().catch((err) => toast(errText(err), 'error'));
  });

  ctx.on(draw);
  draw(store.state);
}
