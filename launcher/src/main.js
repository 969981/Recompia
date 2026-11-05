import './theme.css';
import './layout.css';
import './components.css';
import { listen } from '@tauri-apps/api/event';
import { api, icon, refreshIcons, errText, toast } from './util.js';
import * as play from './pages/play.js';
import * as linkplay from './pages/linkplay.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';
import * as about from './pages/about.js';

const PAGES = {
  play: { mod: play, label: 'Play', icon: 'play' },
  linkplay: { mod: linkplay, label: 'Link Play', icon: 'users' },
  settings: { mod: settings, label: 'Settings', icon: 'settings' },
  setup: { mod: setup, label: 'Game files', icon: 'folder-open' },
  about: { mod: about, label: 'About', icon: 'info' },
};

const nav = document.getElementById('nav');
const view = document.getElementById('view');
let current = null;
let statusHandler = null;
let setupHandler = null;

const ctx = {
  go,
  onStatus(fn) { statusHandler = fn; },
  onSetup(fn) { setupHandler = fn; },
};

function drawNav() {
  nav.innerHTML =
    `<div class="brand">${icon('gamepad-2', 'mark')}<span class="name">Pokopia Launcher</span></div>` +
    Object.entries(PAGES)
      .map(([id, p]) => `<button class="nav-item ${id === current ? 'active' : ''}" data-page="${id}">${icon(p.icon)}<span class="label">${p.label}</span></button>`)
      .join('');
  nav.querySelectorAll('[data-page]').forEach((b) => b.addEventListener('click', () => go(b.dataset.page)));
  refreshIcons();
}

async function go(page, opts = {}) {
  current = page;
  statusHandler = null;
  setupHandler = null;
  drawNav();
  view.scrollTop = 0;
  try {
    await PAGES[page].mod.render(view, { ...ctx, ...opts });
  } catch (e) {
    view.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${errText(e)}</div></div></div>`;
    toast(errText(e), 'error');
  }
  refreshIcons();
}

async function boot() {
  await listen('game-status', (ev) => statusHandler && statusHandler(ev.payload));
  await listen('setup-status', (ev) => setupHandler && setupHandler(ev.payload));
  let cfg = { dump_dir: '' };
  try {
    cfg = await api('get_config');
  } catch (e) {
    toast(errText(e), 'error');
  }
  // first run: no game files chosen yet
  go(cfg.dump_dir ? 'play' : 'setup', { firstRun: !cfg.dump_dir });
}

boot();
