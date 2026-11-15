// Pure state logic: the hero's one button and the background task. No Tauri import, so `node --test` runs it.
// Only these keys of the store state are read: app, setup, game, news, update, unpack, dlss, repack, starting, loadError.

import { num, t } from './i18n.js';

const FINISH_UPDATE = new Set(['rebuild', 'retranslate', 'rebase']);

/** The installed update needs setup to run again before Play. */
export const needsFinish = (kind) => FINISH_UPDATE.has(kind);

const setupRunning = (s) => s.setup?.progress?.state === 'running';

/**
 * Where the game is. Rust reports state "running" for three different moments, told apart by play.rs's message:
 * the shader step before launch, the game itself ("pkp started (process N)" / "pkp.exe started ..."), the shader step after
 * it exits. macOS has no shader step: its play.rs sends "Starting the game..." and then the game's message.
 */
export function gamePhase(game) {
  if (game?.state === 'exited') return 'exited';
  if (game?.state !== 'running') return 'idle';
  const m = game.message || '';
  if (m.startsWith('pkp.exe started') || m.startsWith('pkp started')) return 'running';
  if (m.startsWith('Translating the shaders the game built')) return 'finishing';
  return 'starting';
}

/** Full-window onboarding instead of the shell. */
export function isFirstRun(s) {
  const kind = s.app?.readiness?.kind;
  return kind === 'no_files' || (kind === 'bad_files' && !s.setup?.started);
}

const pctOf = (p) => (p?.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : null);

/**
 * Where a setup phase sits in its step: [step, the step's percent at the phase's start, at its end]. Steps: 0 build game,
 * 1 translate shaders, 2 first run (setup.js names them). The two phases that take the time get the room; a phase whose
 * count runs 0..total then moves the bar across its own range, which is why the lifter reports its passes
 * (pkp_build.py, `@lift`): before that, `lift` was one tick per module and the bar stood still for minutes.
 * Phases: main.rs check, tools, warmup; pkp_build.py verify, lift, compile, link, finish; shaders.rs extract, recheck,
 * translate.
 */
const PHASES = {
  check: [0, 0, 0], tools: [0, 0, 5], verify: [0, 5, 10], lift: [0, 10, 35], compile: [0, 35, 95], link: [0, 95, 100], finish: [0, 100, 100],
  extract: [1, 0, 5], recheck: [1, 5, 10], translate: [1, 5, 100],
  warmup: [2, 0, 0],
};

/** Where setup is: the step and its percent in that step, null when the phase has no count (an unknown phase is step 0).
 *  The setup page shows it with the steps listed beside it; the pill and the Home card show it next to the phase's own
 *  message, which names the step ("Translating the game code: main (1 of 4), working out the flags"), so the number is the
 *  progress of what the message says, and it starts over with the next step's message. */
export function setupStepOf(p) {
  const [step, from, to] = PHASES[p.phase] ?? [0, 0, 0];
  return { step, pct: p.total ? Math.min(100, Math.floor(from + ((to - from) * p.done) / p.total)) : null };
}

const UPDATE_LABEL = {
  download: t('Downloading update'),
  verify: t('Checking update'),
  extract: t('Unpacking update'),
  wait: t('Update ready: close the game to install it'),
  apply: t('Installing update'),
  adopt: t('Installing update'),
  restart: t('Restarting'),
};

// update.rs "prepare": the update's game build runs (pkp_build.py --prepare), its own phase in `step`
const PREPARE_LABEL = {
  verify: () => t('Preparing update: checking game files'),
  lift: () => t('Preparing update: translating game code'),
  compile: (p) => t('Preparing update: compiling {done} of {total}', { done: num(p.done), total: num(p.total) }),
  link: () => t('Preparing update: linking'),
  finish: () => t('Preparing update: linking'),
};

// Only these update phases move files: the game cannot start during them (Rust's Need::Swap). Before them Play stays usable.
const SWAP_PHASES = new Set(['apply', 'adopt', 'restart']);

const updateLabel = (u) => (u.phase === 'prepare' ? (PREPARE_LABEL[u.step] ?? (() => t('Preparing update')))(u) : UPDATE_LABEL[u.phase] || t('Updating'));

/** The update is downloading or preparing the game: the installed game stays playable meanwhile. */
export const updatePlayable = (s) => !!s.update && !SWAP_PHASES.has(s.update.phase);

/** The one background task shown in the sidebar pill, or null. An update outranks the rest: it restarts the launcher.
 *  ponytail: one pill, one task; a second task running at once waits behind it until the first ends. */
export function taskOf(s) {
  if (s.update) return { kind: 'update', label: updateLabel(s.update), pct: ['restart', 'wait'].includes(s.update.phase) ? null : pctOf(s.update) };
  if (setupRunning(s)) return { kind: 'setup', label: t('Setting up'), pct: setupStepOf(s.setup.progress).pct };
  if (s.unpack) return { kind: 'unpack', label: t('Unpacking game files'), pct: pctOf(s.unpack) };
  if (s.dlss) return { kind: 'dlss', label: t('Downloading DLSS'), pct: pctOf(s.dlss) };
  if (s.repack) return { kind: 'repack', label: s.repack.kind === 'expand' ? t('Unpacking shaders') : t('Packing shaders'), pct: pctOf(s.repack) };
  return null;
}

/** The Update button is usable: Rust refuses an update while setup runs. The game may run: the update waits for it before files move. */
export function canUpdate(s) {
  return !!s.news?.update && !taskOf(s) && !s.starting;
}

/** A build the player picked can be installed (no newer update needs to exist: it may be an older one). Nothing else is running. */
export function canInstall(s) {
  return !s.update && !taskOf(s) && !s.starting;
}

/** Rust stops an update until files start to move (preparing the game and waiting for it to close included); after that it must finish. */
export const canCancelUpdate = (s) => ['download', 'verify', 'extract', 'prepare', 'wait'].includes(s.update?.phase);

const off = (label, icon, busy = true) => ({ label, icon, run: null, disabled: true, busy });
const on = (label, icon, run) => ({ label, icon, run, disabled: false, busy: false });

/**
 * The hero's button. `run` names what a click does, for store.perform(): 'choose' | 'game-folder' | 'setup' | 'play' |
 * 'refresh', or null when disabled. Anything running wins over readiness; Play is only ever offered for "ready".
 */
export function primaryAction(s) {
  if (!s.app) return s.loadError ? on(t('Try again'), 'refresh-cw', 'refresh') : off(t('Loading'), 'loader-circle');
  if (s.starting) return off(t('One moment'), 'loader-circle');
  if (setupRunning(s)) return off(t('Setting up'), 'loader-circle');
  switch (gamePhase(s.game)) {
    case 'starting': return off(t('Starting'), 'loader-circle');
    case 'running': return off(t('Running'), 'gamepad-2', false);
    case 'finishing': return off(t('Finishing up'), 'loader-circle');
  }
  if (s.update && !updatePlayable(s)) return off(t('Updating'), 'loader-circle');
  if (s.unpack) return off(t('Unpacking'), 'loader-circle');
  if (s.repack) return off(s.repack.kind === 'expand' ? t('Unpacking shaders') : t('Packing shaders'), 'loader-circle');

  const kind = s.app.readiness?.kind;
  const stopped = ['cancelled', 'error'].includes(s.setup?.progress?.state);
  if (kind === 'no_files' || kind === 'bad_files') return on(t('Choose game files'), 'folder-open', 'choose');
  if (kind === 'no_exe' || kind === 'no_runtime') return on(t('Choose game folder'), 'folder-open', 'game-folder');
  if (kind === 'mixed_release') return off(t('Not ready'), 'circle-alert', false);   // nothing to click: the detail says to put the whole release in place
  if (kind === 'setup') return on(stopped ? t('Continue setup') : t('Set up'), 'wrench', 'setup');
  if (kind === 'resume') return on(t('Continue setup'), 'wrench', 'setup');
  if (needsFinish(kind)) return on(t('Finish update'), 'wrench', 'setup');
  if (kind === 'mods') return on(t('Set up again'), 'wrench', 'setup');   // the mods' hook points changed (Mods page)
  if (kind === 'ready') return on(t('Play'), 'play', 'play');
  return off(t('Not ready'), 'circle-alert', false);   // a kind this build does not know must not fall through to Play
}
