// The channel and version picker: two dropdowns, next to Play on Recompia's Home and in Settings > About. What they offer is
// version-model.js; this file loads the lists, draws the dropdowns and does what a pick means:
//   Stable, Latest                 saved; the Update button offers the newest release again
//   Stable, a version              asked, saved, then installed (store.installVersion: download, check, restart); no update after it
//   Experimental                   the opt-in question, then the list of Experimental builds with their notes; the picked one is
//                                  saved and installed. Nothing is saved before a build is picked, so Cancel keeps the channel as it was
//   Experimental, another build    asked, saved, installed
// The choice is saved before the install starts (the new launcher reads it when it starts); an install that fails or is cancelled
// puts the old choice back. The lists come from the session's copy of the releases (no request to GitHub per pick).
import { api } from '../api.js';
import { t } from '../i18n.js';
import { store } from '../store.js';
import { confirm, errText, esc, icon, toast } from '../ui.js';
import { canInstall, gamePhase } from '../action.js';
import { versionOf } from '../builds-model.js';
import { EXPERIMENTAL, LATEST, STABLE, channelOf, channelOptions, pickKind, pickedOf, versionChoice } from '../version-model.js';
import { confirmBuild, loadBuilds, pickBuild } from './builds.js';
import { askExperimental } from './testing-notice.js';

const state = { config: null, builds: { [STABLE]: [], [EXPERIMENTAL]: [] }, busy: false };
const subs = new Set();
const tell = () => subs.forEach((fn) => fn());

/** Redraws `fn` whenever the picker's state changes; returns the unsubscribe. */
export function onPicker(fn) {
  subs.add(fn);
  return () => subs.delete(fn);
}

/** The saved channel and version, and both lists (from the session's copy unless `refresh`). */
export async function loadPicker(refresh = false) {
  try {
    const [config, stable, experimental] = await Promise.all([api.getConfig(), loadBuilds(STABLE, refresh), loadBuilds(EXPERIMENTAL, refresh)]);
    Object.assign(state, { config, builds: { [STABLE]: stable, [EXPERIMENTAL]: experimental } });
  } catch (e) {
    console.warn('version picker', e);
  }
  tell();
}

// a new release list (Check for updates, the refresh at start): the dropdowns follow it
let lastNews = store.state.news;
store.subscribe((s) => {
  if (s.news === lastNews) return;
  lastNews = s.news;
  if (subs.size) loadPicker();
});

/** Whether the dropdowns can be used now: nothing is installing, setting up or starting, and the game is not running. */
export function pickerLocked(s = store.state) {
  const phase = gamePhase(s.game);
  return state.busy || !state.config || !canInstall(s) || ['starting', 'running', 'finishing'].includes(phase);
}

const optionsHtml = (opts, value) => opts.map((o) => `<option value="${esc(o.value)}" ${o.value === value ? 'selected' : ''} ${o.disabled ? 'disabled' : ''}>${esc(o.label)}</option>`).join('');

const selectHtml = (id, label, opts, value, off, cls) => `<span class="select ${cls}">
    <select id="${id}" aria-label="${esc(label)}" title="${esc(label)}" ${off ? 'disabled' : ''}>${optionsHtml(opts, value)}</select>${icon('chevron-down')}
  </span>`;

/** The two dropdowns. `cls`: an extra class on each (the Home draws them on the art). Stable markup for the same state, so a page that
 *  patches on change keeps an open dropdown open. */
export function pickerHtml(cls = '') {
  const channel = channelOf(state.config);
  const choice = versionChoice(channel, state.builds[channel], pickedOf(state.config));
  const off = pickerLocked();
  return selectHtml('channel-pick', t('Update channel'), channelOptions(), channel, off, cls)
    + selectHtml('version-pick', t('Version'), choice.options, choice.value, off || choice.options.every((o) => o.disabled), cls);
}

export const pickerChannel = () => channelOf(state.config);
export const pickerVersion = () => pickedOf(state.config);

async function save(channel, picked) {
  const config = { ...(await api.getConfig()), channel, picked_version: picked };
  await api.saveConfig({ config });
  const look = channelOf(state.config) !== channel;
  state.config = config;
  await store.refreshNews(false);   // the Update button follows the choice (update_cmds.rs update_current)
  if (look) {
    try {
      await api.applyBranding();   // Experimental has the Testing icons
    } catch (e) {
      toast(t('The new icon is not everywhere yet: {error}', { error: errText(e) }), 'error');
    }
  }
}

/** Saves the choice and installs `b`; an install that comes back (it failed or was cancelled) puts the old choice back. */
async function install(channel, b, accepted) {
  if (!canInstall(store.state)) return;
  const before = { channel: channelOf(state.config), picked: pickedOf(state.config) };
  await save(channel, b.tag);
  tell();
  await store.installVersion(b.tag, channel, accepted);   // on success the launcher restarts and this never returns
  await save(before.channel, before.picked).catch((e) => console.warn('save_config', e));
}

/** The question before a newer build goes in from the dropdown (an older one gets confirmBuild's warning instead). */
async function confirmPick(b) {
  if (b.relation === 'older') return confirmBuild(b);
  const yes = await confirm({
    title: t('Install {version}?', { version: versionOf(b) }),
    html: `<p>${esc(t('The launcher downloads this version, installs it and restarts. Your saves and settings are kept.'))}</p>`,
    ok: t('Install'),
  });
  return yes ? { accepted: false } : null;
}

async function pickVersion(tag) {
  const channel = channelOf(state.config);
  const kind = pickKind(state.builds[channel], tag);
  if (kind === 'none') return;
  if (kind === 'keep') return save(channel, tag);
  const b = state.builds[channel].find((x) => x.tag === tag);
  const ok = await confirmPick(b);
  if (ok) await install(channel, b, ok.accepted);
}

/** Joining Experimental: the question, then its builds with their notes; nothing is saved until one is picked. */
async function joinExperimental() {
  if (!(await askExperimental())) return;
  const builds = await loadBuilds(EXPERIMENTAL, false);
  state.builds[EXPERIMENTAL] = builds;
  const picked = await pickBuild({
    title: t('Experimental builds'),
    intro: t('Each Experimental build tests its own new features. Read the notes and pick the one you want to try: it installs like an update, and nothing updates by itself on Experimental.'),
    builds,
    keep: true,
  });
  if (!picked) return;
  if (pickKind(builds, picked.tag) === 'keep') return save(EXPERIMENTAL, picked.tag);
  const ok = await confirmBuild(picked);
  if (ok) await install(EXPERIMENTAL, picked, ok.accepted);
}

async function pickChannel(channel) {
  if (channel === channelOf(state.config)) return;
  if (channel === EXPERIMENTAL) return joinExperimental();
  return save(STABLE, LATEST);   // back to Stable: Latest; an older release can then be picked in the version dropdown
}

/** A change of one of the dropdowns (`target`: the select). Resolves when the pick is done; the dropdowns are drawn again after it,
 *  showing what is saved (a cancelled question puts them back). Returns false for any other control. */
export function onPickerChange(target) {
  if (target.id !== 'channel-pick' && target.id !== 'version-pick') return false;
  if (state.busy) return true;
  state.busy = true;
  tell();
  const run = target.id === 'channel-pick' ? pickChannel(target.value) : pickVersion(target.value);
  Promise.resolve(run)
    .catch((e) => toast(errText(e), 'error'))
    .finally(() => {
      state.busy = false;
      loadPicker();
    });
  return true;
}
