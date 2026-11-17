import '@fontsource-variable/inter/opsz.css';
import './theme.css';
import './layout.css';
import './components.css';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { api } from './api.js';
import { brandImg, setBrandGame, setBranding } from './branding.js';
import { GAMES, gameForPage, gameOf, hasPage } from './games.js';
import { t } from './i18n.js';
import { errText, esc, icon } from './ui.js';
import { store } from './store.js';
import { isFirstRun } from './action.js';
import { startGamepadNav } from './gamepad-nav.js';
import * as coop from './pages/coop.js';
import * as home from './pages/home.js';
import * as linkplay from './pages/linkplay.js';
import * as achievements from './pages/achievements.js';
import * as mods from './pages/mods.js';
import * as news from './pages/news.js';
import * as saveedit from './pages/saveedit.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';

// nav: shown in the sidebar. art: 'sharp' shows the island, otherwise the blurred copy. Which game offers a page is games.js.
const PAGES = {
  home: { mod: home, nav: t('Home'), icon: 'house', art: 'sharp' },
  coop: { mod: coop, nav: t('Home'), icon: 'house' },   // Recompia: Co-op's Home: cards to read, so the soft art
  linkplay: { mod: linkplay, nav: t('Link Play'), icon: 'link' },   // 'users' is the Co-op tab's
  mods: { mod: mods, nav: t('Mods'), icon: 'puzzle' },
  achievements: { mod: achievements, nav: t('Achievements'), icon: 'trophy' },
  saveedit: { mod: saveedit, nav: t('Save Editor'), icon: 'pencil' },
  news: { mod: news, nav: t("What's New"), icon: 'newspaper' },
  settings: { mod: settings, nav: t('Settings'), icon: 'settings' },
  setup: { mod: setup },
};

// Dev in a plain browser only: the mock stands in for Rust, ?page=<id> opens a page directly. Vite drops this from production.
let startPage = null;
if (import.meta.env.DEV && !window.__TAURI_INTERNALS__) {
  await import('./dev/mock.js');
  PAGES.kit = { mod: await import('./dev/kit.js') };
  startPage = new URLSearchParams(location.search).get('page');
}

// where the sidebar pill leads (a page of another game switches to that game, see go)
const TASK_PAGE = { setup: 'setup', unpack: 'setup', update: 'home', dlss: 'settings', repack: 'home' };

const SWAP_WAIT_MS = 120;   // a page that loads fast swaps in whole; a slow one shows what it has by then

const nav = document.getElementById('nav');
const view = document.getElementById('view');
let current = null;
let token = 0;           // the newest navigation; older ones drop what they rendered
let leaving = [];        // run when the page goes
let shown = gameOf();    // the game the sidebar shows (games.js; launcher.json `game`)

// Launcher colours: 'koi' (Koi's Camp: a bright daytime picture, the default), 'sunrise' (amber to red; reads well with red-green colour blindness), 'dusk' (the old violet) or 'meadow' (a bright daytime scene made for the project).
// Per PC, kept in localStorage: nothing else needs it, and losing it just shows the default.
const THEMES = ['sunrise', 'dusk', 'koi', 'meadow'];
// theme names are proper names: they stay in English in every language
const THEME_NAME = { sunrise: 'Sunrise', dusk: 'Dusk', koi: "Koi's Camp", meadow: 'Meadow' };
function readTheme() {
  try { return THEMES.includes(localStorage.getItem('theme')) ? localStorage.getItem('theme') : 'koi'; } catch { return 'koi'; }
}
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  const b = document.getElementById('theme-btn');
  if (b) b.querySelector('.nav-label').textContent = b.querySelector('.nav-tip').textContent = t('Colours: {name}', { name: THEME_NAME[theme] });
}
applyTheme(readTheme());

// Hide sidebar: a per-PC preference kept in localStorage like the theme. When hidden the nav collapses so the art is
// unobstructed; the floating #show-sidebar button (index.html) is the only way back. data-sidebar="hide" on <html> drives the CSS.
function readSidebar() {
  try { return localStorage.getItem('sidebar') === 'hide'; } catch { return false; }
}
function applySidebar(hidden) {
  if (hidden) document.documentElement.setAttribute('data-sidebar', 'hide');
  else document.documentElement.removeAttribute('data-sidebar');
}
applySidebar(readSidebar());

const navButton = (attrs, ic, label) =>
  `<button class="nav-item" ${attrs} aria-label="${esc(label)}">${ic}<span class="nav-label">${esc(label)}</span><span class="nav-tip" aria-hidden="true">${esc(label)}</span></button>`;

/** The brand block, and under it one tab per game (games.js): Solo and Co-op are always in sight, so nobody has to find co-op in a menu. */
function brandHtml() {
  const tabs = GAMES.map((g) => `<button class="game-tab" data-game="${g.id}" aria-pressed="${g.id === shown.id}" aria-label="${esc(g.name)}">${icon(g.icon)}<span class="nav-label">${esc(g.tab)}</span><span class="nav-tip" aria-hidden="true">${esc(g.name)}</span></button>`).join('');
  return `<div class="brand" data-tauri-drag-region>
      ${brandImg('mark', 'class="brand-mark" alt=""')}${brandImg('mark-testing', `class="brand-mark-testing" alt="${esc(t('Testing'))}"`)}${brandImg('logo', 'class="brand-name brand-logo" alt="Recompia"')}<span class="brand-tag">${esc(t('Testing'))}</span>
    </div>
    <div class="game-tabs" role="group" aria-label="${esc(t('Choose a game'))}">${tabs}</div>`;
}

function drawNav() {
  const items = Object.entries(PAGES).filter(([id, p]) => p.nav && hasPage(shown, id)).map(([id, p]) =>
    navButton(`data-page="${id}"${id === shown.home ? ' data-home' : ''}`, icon(p.icon), p.nav));
  setBrandGame(shown.id);
  nav.innerHTML = `
    ${brandHtml()}
    ${items.join('')}
    <div class="nav-spacer"></div>
    <button class="nav-item" id="theme-btn" aria-label="${esc(t('Change colours'))}">${icon('palette')}<span class="nav-label"></span><span class="nav-tip" aria-hidden="true"></span></button>
    <button class="nav-item" id="ui-hide-btn" aria-label="${esc(t('Hide sidebar'))}">${icon('eye-off')}<span class="nav-label">${esc(t('Hide sidebar'))}</span><span class="nav-tip" aria-hidden="true">${esc(t('Hide sidebar'))}</span></button>
    <button class="task-pill" id="task-pill" hidden>
      ${icon('loader-circle', 'spin')}<span class="task-name clip"></span><span class="task-pct"></span>
      <span class="task-track"><span class="task-fill"></span></span>
    </button>`;
  applyTheme(readTheme());
  if (current) drawChrome();
}

/** Bound once: drawNav replaces the markup inside #nav, not the element. */
function listenNav() {
  nav.addEventListener('click', (e) => {
    const pick = e.target.closest('.game-tabs [data-game]')?.dataset.game;
    if (pick) return switchGame(pick);
    if (e.target.closest('#theme-btn')) {
      const next = THEMES[(THEMES.indexOf(readTheme()) + 1) % THEMES.length];
      try { localStorage.setItem('theme', next); } catch { /* the choice lasts until the launcher closes */ }
      applyTheme(next);
    }
    if (e.target.closest('#ui-hide-btn')) {
      const hide = !readSidebar();
      try { localStorage.setItem('sidebar', hide ? 'hide' : 'show'); } catch { /* the choice lasts until the launcher closes */ }
      applySidebar(hide);
    }
    const page = e.target.closest('[data-page]')?.dataset.page;
    if (page && page !== current) go(page);
    const target = e.target.closest('#task-pill') && TASK_PAGE[store.state.task?.kind];
    if (target && target !== current) go(target);
  });
}

/** Shows game `id` (games.js): the choice is saved to launcher.json `game`, then the sidebar, the pictures and the page follow. `page`:
 *  where to land (the game's Home when absent). */
async function switchGame(id, page, opts) {
  const next = gameOf(id);
  if (next.id !== shown.id) {
    shown = next;
    try {
      await api.saveConfig({ config: { ...(await api.getConfig()), game: next.id } });
    } catch (e) {
      console.warn('save_config', e);   // the choice lasts until the launcher closes
    }
    drawNav();
  }
  return go(page ?? next.home, opts);
}

/** The Testing channel is marked in the sidebar (the Testing icon and a tag), so a tester can tell this launcher from a Stable one at a
 *  glance; the pictures follow the player's logo and icon choice. */
async function syncLook() {
  try {
    const c = await api.getConfig();
    document.documentElement.dataset.channel = c?.channel === 'testing' ? 'testing' : 'stable';
    setBranding(c);
  } catch (e) {
    console.warn('get_config', e);
  }
}

function drawChrome() {
  for (const b of nav.querySelectorAll('[data-page]')) {
    if (b.dataset.page === current) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  // the update has its own dot beside What's New, where its notes are
  const item = nav.querySelector('[data-page="news"]');
  const hasDot = !!item.querySelector('.nav-dot');
  if (hasDot !== !!store.state.news.update) {
    if (hasDot) item.querySelector('.nav-dot').remove();
    else item.insertAdjacentHTML('beforeend', '<span class="nav-dot"></span>');
  }

  const pill = document.getElementById('task-pill');
  const task = store.state.task;
  pill.hidden = !task;
  if (!task) return;
  const pct = task.pct === null ? '' : `${task.pct}%`;
  const say = pct ? `${task.label}, ${pct}` : task.label;
  pill.querySelector('.task-name').textContent = task.label;
  pill.querySelector('.task-pct').textContent = pct;
  pill.title = say;
  pill.setAttribute('aria-label', say);
  pill.style.setProperty('--pct', pct || '0%');
  pill.toggleAttribute('data-indeterminate', task.pct === null);
}

/** Shows a page. opts land on the page's ctx (ctx.firstRun, ctx.tag...); firstRun on setup hides the sidebar. */
async function go(page, opts = {}) {
  if (!PAGES[page]) return;
  // a page the shown game lacks (Set up from Co-op, the task pill, first run) is shown in the game that has it
  const owner = gameForPage(shown, page);
  if (owner && owner !== shown) return switchGame(owner.id, page, opts);
  const id = ++token;
  leaving.forEach((fn) => fn());
  leaving = [];
  current = page;
  document.body.dataset.shell = page === 'setup' && opts.firstRun ? 'full' : '';
  document.body.dataset.art = PAGES[page].art || 'soft';
  drawChrome();

  const host = document.createElement('div');
  host.className = 'page-host';
  const ctx = {
    ...opts,
    go,
    game: shown,
    on(fn) {
      if (id !== token) return () => {};
      const off = store.subscribe(fn);
      leaving.push(off);
      return off;
    },
  };
  const rendered = (async () => {
    try {
      const dispose = await PAGES[page].mod.render(host, ctx);
      if (typeof dispose === 'function') (id === token ? leaving.push(dispose) : dispose());
    } catch (e) {
      console.error(e);
      host.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${esc(errText(e))}</div></div></div>`;
    }
  })();
  await Promise.race([rendered, new Promise((r) => setTimeout(r, SWAP_WAIT_MS))]);
  if (id !== token) return;

  host.classList.add('entering');
  host.addEventListener('animationend', (e) => { if (e.target === host) host.classList.remove('entering'); });
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) host.classList.remove('entering');   // no animation, no animationend (#260)
  view.replaceChildren(host);
  view.scrollTop = 0;
  view.focus({ preventScroll: true });
}

function drawWindowButtons() {
  const host = document.getElementById('winbtns');
  const win = getCurrentWindow();
  const button = (id, name, label, cls = '') => `<button class="win-btn ${cls}" id="${id}" aria-label="${label}" title="${label}">${icon(name)}</button>`;
  const quiet = (p) => p.catch((e) => console.warn(e));
  host.innerHTML = button('win-min', 'minus', t('Minimise')) + button('win-max', 'square', t('Maximise')) + button('win-close', 'x', t('Close'), 'close');
  document.getElementById('win-min').addEventListener('click', () => quiet(win.minimize()));
  document.getElementById('win-max').addEventListener('click', () => quiet(win.toggleMaximize()));
  document.getElementById('win-close').addEventListener('click', () => quiet(win.close()));
  document.getElementById('titlebar').addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' || e.target.closest('button')) return;
    e.preventDefault();
    quiet(win.startDragging());
  });
  const max = document.getElementById('win-max');
  const syncMax = () => quiet(win.isMaximized().then((on) => {
    max.innerHTML = icon(on ? 'copy' : 'square');
    max.title = on ? t('Restore') : t('Maximise');
    max.setAttribute('aria-label', max.title);
  }));
  quiet(win.onResized(syncMax));
  syncMax();
}

async function start() {
  // the static shell of index.html is English; the language of the player goes on here
  nav.setAttribute('aria-label', t('Main'));
  document.getElementById('winbtns').setAttribute('aria-label', t('Window'));
  startGamepadNav();
  shown = gameOf(await api.getConfig().then((c) => c?.game, () => ''));
  listenNav();
  drawNav();
  drawWindowButtons();
  const showSidebar = document.getElementById('show-sidebar');
  showSidebar.innerHTML = `${icon('eye')}<span>${esc(t('Show sidebar'))}</span>`;
  showSidebar.setAttribute('aria-label', t('Show sidebar'));
  showSidebar.addEventListener('click', () => {
    try { localStorage.setItem('sidebar', 'show'); } catch { /* the choice lasts until the launcher closes */ }
    applySidebar(false);
  });
  store.subscribe(drawChrome);
  store.subscribe((_s, why) => { if (why === 'news') syncLook(); });
  syncLook();
  drawChrome();
  store.start().catch((e) => console.error(e));
  const once = store.subscribe((s, why) => {
    if (why !== 'app') return;
    once();
    if (startPage) go(startPage);
    else go(isFirstRun(s) ? 'setup' : shown.home, { firstRun: isFirstRun(s) });
  });
}

start().catch((e) => console.error(e));
