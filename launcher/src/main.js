import '@fontsource-variable/inter/opsz.css';
import './theme.css';
import './layout.css';
import './components.css';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { api } from './api.js';
import { brandImg, setBrandGame } from './branding.js';
import { GAMES, PRODUCTS, gameForPage, gameOf, gamesOf, hasPage, stepGame } from './games.js';
import { t } from './i18n.js';
import { errText, esc, icon } from './ui.js';
import { store } from './store.js';
import { isFirstRun } from './action.js';
import { welcomeOwed } from './cloud-signin.js';
import { padGlyph, startGamepadNav } from './gamepad-nav.js';
import { zonithBadge } from './zonith-model.js';
import { channelOf } from './version-model.js';
import { applyTheme, cycleTheme, syncThemes } from './themes.js';
import * as coop from './pages/coop.js';
import * as home from './pages/home.js';
import * as linkplay from './pages/linkplay.js';
import * as achievements from './pages/achievements.js';
import * as mods from './pages/mods.js';
import * as news from './pages/news.js';
import * as saveedit from './pages/saveedit.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';
import * as welcome from './pages/welcome.js';
import * as zonith from './pages/zonith.js';
import * as zonithNews from './pages/zonith-news.js';

// nav: shown as a tab in the top bar. art: 'sharp' shows the game's picture, otherwise the blurred copy. Which game offers a page is games.js.
const PAGES = {
  home: { mod: home, nav: t('Home'), art: 'sharp' },
  coop: { mod: coop, nav: t('Home'), art: 'sharp' },   // Recompia: Co-op's Home opens on its own hero
  zonith: { mod: zonith, nav: t('Home'), art: 'sharp' },
  linkplay: { mod: linkplay, nav: t('Link Play') },
  mods: { mod: mods, nav: t('Mods') },
  achievements: { mod: achievements, nav: t('Achievements') },
  saveedit: { mod: saveedit, nav: t('Save Editor') },
  news: { mod: news, nav: t("What's New") },
  'zonith-news': { mod: zonithNews, nav: t("What's New") },
  settings: { mod: settings, nav: t('Settings') },
  setup: { mod: setup },
  welcome: { mod: welcome, art: 'clear' },   // the online account's sign-in screen (pages/welcome.js; at start, and Home's account chip): the picture without scrims
};

// Dev in a plain browser only: the mock stands in for Rust, ?page=<id> opens a page directly (&from=chip: the sign-in screen as Home's
// account chip opens it). Vite drops this from production.
let startPage = null;
let startOpts = {};
if (import.meta.env.DEV && !window.__TAURI_INTERNALS__) {
  await import('./dev/mock.js');
  PAGES.kit = { mod: await import('./dev/kit.js') };
  const q = new URLSearchParams(location.search);
  startPage = q.get('page');
  if (q.get('from')) startOpts = { from: q.get('from') };
  if (q.get('section')) startOpts = { ...startOpts, section: q.get('section') };   // &section=<id>: Settings opens on that section
}

// where the task pill leads (a page of another game switches to that game, see go)
const TASK_PAGE = { setup: 'setup', unpack: 'setup', update: 'home', dlss: 'settings', repack: 'home', zonith: 'zonith' };
// the tasks whose page always shows their progress, so the pill would say it twice there (Settings shows DLSS only on one tab)
const SHOWN_ON_PAGE = new Set(['setup', 'unpack', 'update', 'repack', 'zonith']);

const SWAP_WAIT_MS = 120;   // a page that loads fast swaps in whole; a slow one shows what it has by then
const ART_SWAP_MS = 280;    // the art fades between games for one slow beat (layout.css body[data-swapping])

const rail = document.getElementById('rail');
const nav = document.getElementById('nav');
const tools = document.getElementById('tools');
const view = document.getElementById('view');
let current = null;
let token = 0;           // the newest navigation; older ones drop what they rendered
let leaving = [];        // run when the page goes
let shown = gameOf();    // the game the launcher shows (games.js; launcher.json `game`)
let welcomeAsked = false;   // the sign-in screen is a question for the first Home of a launcher start only

// Hide sidebar: a per-PC preference kept in localStorage like the theme. When hidden the rail and the tabs go so the art is
// unobstructed; the floating #show-sidebar button (index.html) is the only way back. data-sidebar="hide" on <html> drives the CSS.
function readSidebar() {
  try { return localStorage.getItem('sidebar') === 'hide'; } catch { return false; }
}
function applySidebar(hidden) {
  if (hidden) document.documentElement.setAttribute('data-sidebar', 'hide');
  else document.documentElement.removeAttribute('data-sidebar');
}
applyTheme();
applySidebar(readSidebar());

const tip = (label) => `<span class="nav-tip" aria-hidden="true">${esc(label)}</span>`;

/** One game's tile: its mark when it has one of its own (Recompia, Zonith), else its icon (Co-op, under Recompia). */
function railGame(g, sub) {
  const tile = sub ? `<span class="rail-tile">${icon(g.icon)}</span>` : `<span class="rail-tile mark ${g.id}">${brandImg('mark', 'alt=""', g.id)}</span>`;
  return `<button class="rail-game${sub ? ' sub' : ''}" data-game="${g.id}" aria-current="${g.id === shown.id}" aria-label="${esc(g.name)}">${tile}<span class="rail-label">${esc(sub ? g.tab : g.name)}</span><span class="rail-badge" hidden></span>${tip(g.name)}</button>`;
}

/** The rail: the launcher's mark, then one group per product (its first game, then the rest hanging under it), then the launcher's own buttons. */
function drawRail() {
  const groups = PRODUCTS.map((p) => {
    const [first, ...rest] = gamesOf(p);
    return `<div class="rail-group" role="group" aria-label="${esc(p.name)}">${railGame(first, false)}${rest.map((g) => railGame(g, true)).join('')}</div>`;
  }).join('<div class="rail-sep" aria-hidden="true"></div>');
  rail.setAttribute('aria-label', t('Games'));
  rail.innerHTML = `
    <div class="rail-brand" data-tauri-drag-region><span class="brand-tag">${esc(t('Experimental'))}</span></div>
    ${groups}
    <div class="rail-spacer"></div>
    <button class="rail-tool" id="theme-btn">${icon('palette')}${tip('')}</button>
    <button class="rail-tool" id="ui-hide-btn" aria-label="${esc(t('Hide sidebar'))}">${icon('eye-off')}${tip(t('Hide sidebar'))}</button>
    <button class="rail-tool" id="settings-btn" data-page="settings" aria-label="${esc(t('Settings'))}">${icon('settings')}${tip(t('Settings'))}</button>`;
  applyTheme();
}

/** The shown game's pages in the top bar, with the pad's bumper glyphs at the two ends. */
function drawNav() {
  const items = Object.entries(PAGES).filter(([id, p]) => p.nav && hasPage(shown, id)).map(([id, p]) =>
    `<button class="nav-tab" data-page="${id}"${id === shown.home ? ' data-home' : ''}>${esc(p.nav)}</button>`);
  setBrandGame(shown.id);
  document.documentElement.dataset.game = shown.id;
  document.title = shown.product === 'zonith' ? 'Zonith' : 'Recompia';
  nav.setAttribute('aria-label', t('{game} pages', { game: shown.name }));
  nav.innerHTML = `${padGlyph('LB', 'nav-bumper')}${items.join('')}${padGlyph('RB', 'nav-bumper')}`;
  for (const b of rail.querySelectorAll('[data-game]')) b.setAttribute('aria-current', String(b.dataset.game === shown.id));
  if (current) drawChrome();
}

function drawTools() {
  tools.innerHTML = `<button class="task-pill" id="task-pill" hidden>
      ${icon('loader-circle', 'spin')}<span class="task-name clip"></span><span class="task-pct"></span>
      <span class="task-track"><span class="task-fill"></span></span>
    </button>`;
}

/** Bound once: the draw functions replace the markup inside the elements, not the elements. */
function listen() {
  rail.addEventListener('click', (e) => {
    const pick = e.target.closest('.rail-game')?.dataset.game;   // not '[data-game]': <html> carries data-game too, which made every rail button a game switch
    if (pick) return switchGame(pick);
    if (e.target.closest('#theme-btn')) {
      cycleTheme();
    }
    if (e.target.closest('#ui-hide-btn')) {
      const hide = !readSidebar();
      try { localStorage.setItem('sidebar', hide ? 'hide' : 'show'); } catch { /* the choice lasts until the launcher closes */ }
      applySidebar(hide);
    }
    if (e.target.closest('#settings-btn') && current !== 'settings') go('settings');
  });
  nav.addEventListener('click', (e) => {
    const page = e.target.closest('[data-page]')?.dataset.page;
    if (page && page !== current) go(page);
  });
  tools.addEventListener('click', (e) => {
    const target = e.target.closest('#task-pill') && TASK_PAGE[pillTask()?.kind];
    if (target && target !== current) go(target);
  });
}

/** Shows game `id` (games.js): the choice is saved to launcher.json `game`, then the rail, the tabs, the pictures and the page follow.
 *  `page`: where to land (the game's Home when absent). */
async function switchGame(id, page, opts) {
  const next = gameOf(id);
  if (next.id !== shown.id) {
    const art = gameOf(shown.id).product !== next.product;
    shown = next;
    if (art && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
      document.body.dataset.swapping = '';
      setTimeout(() => delete document.body.dataset.swapping, ART_SWAP_MS);
    }
    drawNav();
    try {
      await api.saveConfig({ config: { ...(await api.getConfig()), game: next.id } });
    } catch (e) {
      console.warn('save_config', e);   // the choice lasts until the launcher closes
    }
  }
  return go(page ?? next.home, opts);
}

/** The Experimental channel is marked on the rail (a tag under the mark), so a tester can tell this launcher from a Stable one at a glance. */
async function syncLook() {
  try {
    const c = await api.getConfig();
    document.documentElement.dataset.channel = channelOf(c);
  } catch (e) {
    console.warn('get_config', e);
  }
}

/** The task the pill shows: Recompia's (store.state.task), else Zonith's download. */
function pillTask() {
  const s = store.state;
  if (s.task) return s.task;
  const job = s.zonith?.job;
  if (job && ['download', 'unpack'].includes(job.state)) {
    return { kind: 'zonith', label: job.state === 'unpack' ? t('Unpacking Zonith') : t('Downloading Zonith'), pct: job.pct == null ? null : Math.round(job.pct) };
  }
  return null;
}

function drawChrome() {
  for (const b of document.querySelectorAll('#nav [data-page], #rail [data-page]')) {
    if (b.dataset.page === current) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  // the update has its own dot beside What's New, where its notes are
  const item = nav.querySelector('[data-page="news"]');
  if (item) {
    const hasDot = !!item.querySelector('.nav-dot');
    if (hasDot !== !!store.state.news.update) {
      if (hasDot) item.querySelector('.nav-dot').remove();
      else item.insertAdjacentHTML('beforeend', '<span class="nav-dot"></span>');
    }
  }
  // a badge on a rail tile says what waits there without opening it: Zonith's download or update, Recompia's update
  for (const b of rail.querySelectorAll('[data-game]')) {
    const badge = b.querySelector('.rail-badge');
    const g = gameOf(b.dataset.game);
    const say = g.product === 'zonith' ? zonithBadge(store.state.zonith)
      : g.id === 'recompia' && store.state.news.update ? { text: t('Update') } : null;
    badge.hidden = !say;
    if (say) { badge.textContent = say.text; badge.classList.toggle('busy', !!say.busy); }
  }

  const pill = document.getElementById('task-pill');
  // the page the task belongs to shows it already (a Home's Play button and bar): the pill is for every other page
  const task = pillTask();
  pill.hidden = !task || (SHOWN_ON_PAGE.has(task.kind) && TASK_PAGE[task.kind] === current);
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

/** Shows a page. opts land on the page's ctx (ctx.firstRun, ctx.tag...); firstRun on setup and the sign-in screen hide the tabs (the rail
 *  stays). */
async function go(page, opts = {}) {
  if (!PAGES[page]) return;
  // the first Home of a launcher start, when it is one of Recompia's (the online account is for its Cloud Islands; a start on Zonith is not
  // asked): Cloud Islands switched on and signed out (and not skipped), the sign-in screen comes first and the start questions (freeing shader
  // space) wait behind it (cloud-signin.js welcomeOwed, pages/welcome.js)
  const firstHome = !welcomeAsked && GAMES.find((g) => g.home === page);
  if (firstHome) {
    welcomeAsked = true;
    if (firstHome.product === 'recompia') {
      let release;
      store.holdQuestions(new Promise((r) => { release = r; }));
      const before = token;
      if (await welcomeOwed().catch(() => false) && token === before) return go('welcome', { next: page, nextOpts: opts, release });
      release();
    }
  }
  // a page the shown game lacks (Set up from Co-op, Settings from Zonith, the task pill, first run) is shown in the game that has it
  const owner = gameForPage(shown, page);
  if (owner && owner !== shown) return switchGame(owner.id, page, opts);
  const id = ++token;
  leaving.forEach((fn) => fn());
  leaving = [];
  current = page;
  document.body.dataset.shell = (page === 'setup' && opts.firstRun) || page === 'welcome' ? 'full' : '';
  document.body.dataset.art = PAGES[page].art || 'soft';
  sayBack(page, opts);
  drawChrome();

  const host = document.createElement('div');
  host.className = 'page-host';
  const ctx = {
    ...opts,
    go,
    switchGame,
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
  const bar = document.getElementById('topbar');
  bar.addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' || e.target.closest('button')) return;
    e.preventDefault();
    quiet(win.startDragging());
  });
  // the bar lies over the page, so the wheel over it scrolls the page as it would anywhere else
  bar.addEventListener('wheel', (e) => view.scrollBy({ top: e.deltaY, left: 0 }), { passive: true });
  const max = document.getElementById('win-max');
  const syncMax = () => quiet(win.isMaximized().then((on) => {
    max.innerHTML = icon(on ? 'copy' : 'square');
    max.title = on ? t('Restore') : t('Maximise');
    max.setAttribute('aria-label', max.title);
  }));
  quiet(win.onResized(syncMax));
  syncMax();
}

/** What the pad's buttons do, along the bottom while a pad is in use. The Start button is the shown Home's main button. A full-window
 *  screen (data-shell full) shows no Page or Play hint (layout.css); B's word follows the page (sayBack). */
function drawPadBar() {
  const hint = (key, buttons, label) => `<span class="pad-hint" data-hint="${key}">${buttons.map(([b, cls]) => padGlyph(b, cls)).join('')}<span class="pad-say">${esc(label)}</span></span>`;
  document.getElementById('padbar').innerHTML = hint('game', [['LT'], ['RT']], t('Game')) + hint('page', [['LB'], ['RB']], t('Page'))
    + hint('play', [['Start']], t('Play')) + hint('back', [['B', 'round b']], t('Back')) + hint('select', [['A', 'round a']], t('Select'));
}

/** B's hint: Skip on the sign-in screen at start (B presses its Skip, gamepad-nav.js data-pad-back), else Back. */
function sayBack(page, opts) {
  const say = document.querySelector('#padbar [data-hint="back"] .pad-say');
  if (say) say.textContent = page === 'welcome' && opts.from !== 'chip' ? t('Skip') : t('Back');
}

async function start() {
  // the static shell of index.html is English; the language of the player goes on here
  document.getElementById('winbtns').setAttribute('aria-label', t('Window'));
  startGamepadNav({
    switchGame: (step) => switchGame(stepGame(shown.id, step).id),
    play: () => document.querySelector('#view [data-hero-play]:not(:disabled)')?.click(),
  });
  shown = gameOf(await api.getConfig().then((c) => c?.game, () => ''));
  listen();
  drawRail();
  syncThemes();
  drawTools();
  drawNav();
  drawPadBar();
  drawWindowButtons();
  const showSidebar = document.getElementById('show-sidebar');
  showSidebar.innerHTML = `${icon('eye')}<span>${esc(t('Show sidebar'))}</span>`;
  showSidebar.setAttribute('aria-label', t('Show sidebar'));
  showSidebar.addEventListener('click', () => {
    try { localStorage.setItem('sidebar', 'show'); } catch { /* the choice lasts until the launcher closes */ }
    applySidebar(false);
  });
  store.subscribe(() => { if (current) drawChrome(); });
  store.subscribe((_s, why) => { if (why === 'news') syncLook(); });
  syncLook();
  store.start().catch((e) => console.error(e));
  const once = store.subscribe((s, why) => {
    if (why !== 'app') return;
    once();
    if (startPage) go(startPage, startOpts);
    // first run sets up Recompia; a player who chose Zonith opens on it, it has nothing to set up here
    else if (shown.product === 'zonith') go(shown.home);
    else go(isFirstRun(s) ? 'setup' : shown.home, { firstRun: isFirstRun(s) });
  });
}

start().catch((e) => console.error(e));
