// The one source of app state. Owns every Tauri event listener, so a page never calls listen(); a page reads store.state
// and subscribes with ctx.on(fn). Long work is started here, not on a page, so it survives navigation.
import { listen } from '@tauri-apps/api/event';
import { api, confirm, errText, esc, icon, toast } from './ui.js';
import { canUpdate, needsFinish, taskOf } from './action.js';

const subs = new Set();
let autoFinishDone = false;   // `updated` is acted on once
const UPDATE_CANCELLED = 'Update cancelled.';   // update.rs CANCELLED: not a failure

/** Applies a patch, derives `task`, tells subscribers. `why`: 'app' | 'setup' | 'game' | 'news' | 'task' | 'update' | 'drop'. */
function set(patch, why) {
  Object.assign(store.state, patch);
  store.state.task = taskOf(store.state);
  for (const fn of [...subs]) {
    try {
      fn(store.state, why);
    } catch (e) {
      console.error(e);   // one broken subscriber must not stop the others
    }
  }
}

function preflightList(pre) {
  const rows = pre.checks.filter((c) => c.level !== 'ok').map((c) => {
    const bad = c.level === 'fail';
    return `<li class="${bad ? 'bad' : 'warn'}">${icon(bad ? 'circle-x' : 'triangle-alert')}<div><strong>${esc(c.title)}</strong><span>${esc(c.detail)}</span></div></li>`;
  });
  return `<ul class="modal-list">${rows.join('')}</ul>`;
}

/** preflight, shown in one dialog from wherever setup was started. All fine starts at once; warnings can be passed, a failed check cannot. */
async function passPreflight() {
  let pre;
  try {
    pre = await api('preflight');
  } catch (e) {
    toast(errText(e), 'error');
    return { go: true };
  }
  if (pre.blocked) {
    await confirm({ title: 'Set up cannot start yet', html: preflightList(pre), ok: 'Close', alert: true });
    return { go: false };
  }
  if (pre.warnings) {
    const go = await confirm({ title: 'Check these before you start', html: preflightList(pre), ok: 'Set up anyway' });
    return { go, gentle: pre.suggest_gentle || undefined };
  }
  return { go: true };
}

async function onSetupEvent(p) {
  const was = store.state.setup?.progress?.state;
  set({ setup: { ...store.state.setup, progress: p } }, 'setup');
  if (p.state === 'running') return;
  await store.refresh();
  if (was === 'running' && p.state === 'done') toast('Setup finished', 'ok');
  if (was === 'running' && p.state === 'error') toast('Setup stopped with an error', 'error');
}

export const store = {
  state: {
    app: null,            // launcher_state, null until it loads
    setup: null,          // setup_status
    game: null,           // game_status
    news: { releases: [], update: null },
    task: null,           // derived: { kind: 'setup'|'update'|'nup'|'dlss', label, pct (null: unknown) } | null
    update: null,         // last update-progress while an update runs
    updateError: null,
    nup: null,            // last nup-progress while a .nup unpacks
    dlss: null,           // last dlss-progress while DLSS downloads
    starting: false,      // a start click is on its way to Rust
    dragging: false,      // files are being dragged over the window
    dropped: null,        // the paths of the drop that fired why 'drop'
    loadError: null,
  },

  /** ctx.on(fn) wraps this for pages. fn(state, why). */
  subscribe(fn) {
    subs.add(fn);
    return () => subs.delete(fn);
  },

  /** Attaches the listeners, then loads everything. Call once, before the first page. */
  async start() {
    const on = (event, fn) => listen(event, (ev) => fn(ev.payload)).catch((e) => console.error(event, e));
    await Promise.all([
      on('game-status', (p) => set({ game: p }, 'game')),
      on('setup-status', onSetupEvent),
      on('update-progress', (p) => set({ update: p }, 'update')),
      on('dlss-progress', (p) => store.state.dlss && set({ dlss: p }, 'task')),
      on('nup-progress', (p) => store.state.nup && set({ nup: p }, 'task')),
      on('tauri://drag-enter', () => set({ dragging: true, dropped: null }, 'drop')),
      on('tauri://drag-leave', () => set({ dragging: false, dropped: null }, 'drop')),
      on('tauri://drag-drop', (p) => set({ dragging: false, dropped: p.paths ?? [] }, 'drop')),
    ]);
    await store.refresh();
    store.refreshNews(false).then(() => store.refreshNews(true));
    afterUpdate();
  },

  /** Reloads launcher_state, setup_status and game_status. A failed part keeps its last value. */
  async refresh() {
    const [app, setup, game] = await Promise.allSettled([api('launcher_state'), api('setup_status'), api('game_status')]);
    const patch = { loadError: app.status === 'rejected' ? errText(app.reason) : null };
    if (app.status === 'fulfilled') patch.app = app.value;
    if (setup.status === 'fulfilled') patch.setup = setup.value;
    if (game.status === 'fulfilled') patch.game = game.value;
    set(patch, 'app');
  },

  /** get_releases. refresh=false serves the cache, true asks GitHub. Silent on failure: patch notes are optional. */
  async refreshNews(refresh = false) {
    try {
      const news = await api('get_releases', { refresh });
      set({ news: { releases: news?.releases ?? [], update: news?.update ?? null } }, 'news');
    } catch (e) {
      console.warn('get_releases', e);
    }
  },

  /** Runs setup through preflight. gentle: undefined keeps the saved choice. Resolves true when setup started. */
  async startSetup({ gentle } = {}) {
    const s = store.state;
    if (s.starting || s.setup?.progress?.state === 'running') return false;
    set({ starting: true }, 'task');
    try {
      const pass = await passPreflight();
      if (!pass.go) return false;
      await api('start_setup', { gentle: gentle ?? pass.gentle });
      const setup = await api('setup_status').catch(() => null);
      if (setup) set({ setup }, 'setup');
      return true;
    } catch (e) {
      toast(errText(e), 'error');
      store.refresh();
      return false;
    } finally {
      set({ starting: false }, 'task');
    }
  },

  /** Pause: finished work is kept, startSetup continues it. */
  async pauseSetup() {
    try {
      await api('cancel_setup');
    } catch (e) {
      toast(errText(e), 'error');
    }
  },

  async play() {
    if (store.state.starting) return;
    set({ starting: true }, 'task');
    try {
      await api('play');
    } catch (e) {
      toast(errText(e), 'error');
      store.refresh();
    } finally {
      set({ starting: false }, 'task');
    }
  },

  /** Downloads and applies the update; on success Rust restarts the launcher, so this only returns on failure. */
  async installUpdate() {
    if (!canUpdate(store.state)) return;
    set({ update: { phase: 'download', done: 0, total: 0 }, updateError: null }, 'update');
    try {
      await api('install_update');
    } catch (e) {
      const updateError = errText(e) === UPDATE_CANCELLED ? null : errText(e);
      set({ update: null, updateError }, 'update');
      if (updateError) toast(updateError, 'error');
    }
  },

  async cancelUpdate() {
    try {
      await api('cancel_update');
    } catch (e) {
      toast(errText(e), 'error');
    }
  },

  /** Unpacks a .nup through the sidebar task. Resolves to the unpacked folder; rejects like the command. */
  async importNup(path) {
    set({ nup: { done: 0, total: 0, file: '' } }, 'task');
    try {
      return await api('import_nup', { path });
    } finally {
      set({ nup: null }, 'task');
    }
  },

  cancelNup: () => api('cancel_nup'),

  /** Downloads NVIDIA's DLSS file through the sidebar task; rejects like the command. */
  async fetchDlss() {
    set({ dlss: { done: 0, total: 0, message: '' } }, 'task');
    try {
      await api('fetch_dlss');
    } finally {
      set({ dlss: null }, 'task');
    }
  },

  /** Carries out primaryAction(state).run. `go` is the page's ctx.go. */
  perform(run, go) {
    switch (run) {
      case 'choose': return go('setup');
      case 'game-folder': return go('settings', { section: 'game' });
      case 'setup': return store.startSetup();
      case 'play': return store.play();
      case 'refresh': return store.refresh();
    }
  },
};

/** After a self-update restart: say so, and finish the update without being asked. */
function afterUpdate() {
  const app = store.state.app;
  if (!app?.updated || autoFinishDone) return;
  autoFinishDone = true;
  toast(`Updated to ${app.version}`, 'ok');
  if (needsFinish(app.readiness?.kind)) store.startSetup();
}
