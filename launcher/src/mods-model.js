// The Mods page, the pure part: what a mod's row and a Browse card say, the questions asked before a mod's code first runs, and the
// value a setting's control sends back. No DOM and no Tauri, so `node scripts/test-mods-model.mjs` runs it. The page is pages/mods.js;
// the list and the catalog come from Rust (mods.rs ModView, mods_catalog.rs CardView).
import { t } from './i18n.js';

/** The small labels on an installed mod's row: [{ text, kind: '' | 'ok' | 'warn' | 'danger' }]. */
export function badges(mod) {
  const out = [];
  if (mod.state === 'error') out.push({ text: t('Problem'), kind: 'danger' });
  if (mod.needs_yes) out.push({ text: t('Not allowed yet'), kind: 'warn' });
  out.push(mod.gameplay ? { text: t('Changes gameplay'), kind: 'warn' } : { text: t('Looks only'), kind: '' });
  if (mod.network) out.push({ text: t('Goes online'), kind: '' });
  return out;
}

/** "By A, B. Licence: MIT." for a mod or a card (authors are a string from the core, a list from the catalog). */
export function byLine(m) {
  const authors = Array.isArray(m.authors) ? m.authors.join(', ') : m.authors;
  const licence = m.license || t('not stated');
  return authors ? t('By {authors}. Licence: {licence}.', { authors, licence }) : t('Licence: {licence}.', { licence });
}

/** The one line that tells the player which mods were updated by themselves: [{ name, from, to }] from the launcher. */
export function updatedLine(done) {
  return t('Updated by itself: {list}', { list: done.map((u) => `${u.name} ${u.from} → ${u.to}`).join(', ') });
}

/** What a Browse card's button is: 'install', 'update' (a newer version than the installed one), 'installed', 'soon' (no download
 *  yet) or 'closed' (the hub says nothing is installable yet). */
export function cardAction(card, preview) {
  if (!card.available) return 'soon';
  if (card.installed && !card.newer) return 'installed';
  if (preview) return 'closed';
  return card.installed ? 'update' : 'install';
}

/** The lines of the Mods page's two questions about what a mod does, in the order shown. */
function whatItDoes(m, codeLine) {
  const lines = [byLine(m), codeLine];
  if (m.gameplay) lines.push(t('It changes gameplay: everyone in a Link Play session needs it too.'));
  if (m.network) lines.push(typeof m.network === 'string' ? t('It goes online ({hosts}).', { hosts: m.network }) : t('It goes online.'));
  return lines;
}

/** Install (or update) from the hub: { title, lines, ok }. */
export function installQuestion(card) {
  const update = !!card.installed;
  return {
    title: update ? t('Update {name} to {version}?', { name: card.name, version: card.version }) : t('Install {name} {version}?', { name: card.name, version: card.version }),
    lines: whatItDoes(card, t('A mod with code runs that code on this PC with your rights, like any program you install.')),
    ok: update ? t('Update') : t('Install'),
  };
}

/** Switching on a mod whose code the player has not allowed (put into the mods folder by hand, or changed since): { title, lines, ok }. */
export function allowQuestion(mod) {
  return {
    title: t('Allow {name} {version} to run?', { name: mod.name || mod.id, version: mod.version }),
    lines: [...whatItDoes(mod, t('Its code is new on this PC, or changed since you last allowed it. It runs with your rights, like any program you install.')),
      t('It did not come through the launcher: only allow it if you trust where you got it.')],
    ok: t('Allow and switch on'),
  };
}

/** The value a setting's control sends to mods_set_setting: { number } for on/off, numbers and choices (by index), { text } for the rest. */
export function settingValue(s, control) {
  if (s.kind === 'bool') return { number: control.checked ? 1 : 0 };
  if (s.kind === 'number' || s.kind === 'choice') return { number: Number(control.value) };
  return { text: String(control.value) };
}

/** A number setting draws as a slider when it has a range, else as a field. */
export const isSlider = (s) => s.kind === 'number' && s.max > s.min;

/** Whether the mod at index i of the list can go up (-1) or down (+1). */
export const canMove = (list, i, delta) => i + delta >= 0 && i + delta < list.length;
