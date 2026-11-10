// The launcher's language. English text is the key: t('Play') returns the player's language, or 'Play' itself when a catalog has no
// entry, so a missing translation shows English and never a blank or a code. Catalogs are src/i18n/<code>.js ({ 'English': 'Texto' }).
// No DOM and no Tauri at import time, so `node --test` and the check scripts can load it (they get English).
//
// The language is read once at start and a change reloads the page (setLanguage), so strings built at module load, like the Settings
// schema's labels, are already in the right language. The choice is per PC, kept in localStorage like the colours: 'auto' follows the
// system, anything else is a code of LANGUAGES.
import en from './i18n/en.js';
import es from './i18n/es.js';
import ja from './i18n/ja.js';
import fr from './i18n/fr.js';
import it from './i18n/it.js';
import de from './i18n/de.js';
import ko from './i18n/ko.js';
import zhCN from './i18n/zh-CN.js';
import zhTW from './i18n/zh-TW.js';

/** The languages the launcher is written in. `name` is the language's own name, so a player can find theirs whatever is showing. */
export const LANGUAGES = [
  { code: 'en', name: 'English', catalog: en },
  { code: 'es', name: 'Español', catalog: es },
  { code: 'ja', name: '日本語', catalog: ja },
  { code: 'fr', name: 'Français', catalog: fr },
  { code: 'it', name: 'Italiano', catalog: it },
  { code: 'de', name: 'Deutsch', catalog: de },
  { code: 'ko', name: '한국어', catalog: ko },
  { code: 'zh-CN', name: '简体中文', catalog: zhCN },
  { code: 'zh-TW', name: '繁體中文', catalog: zhTW },
];

export const DEFAULT_LANGUAGE = 'en';
export const AUTO = 'auto';
const STORAGE_KEY = 'language';
const TRADITIONAL = ['tw', 'hk', 'mo', 'hant'];   // zh-TW, zh-HK, zh-MO and zh-Hant-* read Traditional; every other zh reads Simplified

/**
 * The code of LANGUAGES a system language tag stands for, or null. Only the language counts, so es-ES and es-MX are both 'es'.
 * Chinese is told apart by script or region: zh-Hant, zh-TW, zh-HK, zh-MO are Traditional, the rest Simplified.
 */
export function matchLanguage(tag) {
  const parts = String(tag ?? '').trim().toLowerCase().replace(/_/g, '-').split('-');
  const base = parts[0];
  if (base === 'zh') return parts.slice(1).some((p) => TRADITIONAL.includes(p)) ? 'zh-TW' : 'zh-CN';
  return LANGUAGES.some((l) => l.code === base) ? base : null;
}

/** The first of the system's languages (navigator.languages, most wanted first) that the launcher has; English when none or when unknown. */
export function systemLanguage(tags) {
  for (const tag of tags ?? []) {
    const code = matchLanguage(tag);
    if (code) return code;
  }
  return DEFAULT_LANGUAGE;
}

/** The language to show for a saved choice (AUTO, a code, or anything else) and the system's tags. */
export function resolveLanguage(choice, tags) {
  const saved = LANGUAGES.find((l) => l.code === choice);
  return saved ? saved.code : systemLanguage(tags);
}

const storage = () => { try { return globalThis.window?.localStorage ?? null; } catch { return null; } };

/** The saved choice: a code of LANGUAGES, or AUTO. */
export function savedLanguage() {
  let v = null;
  try { v = storage()?.getItem(STORAGE_KEY); } catch { /* unreadable storage: follow the system */ }
  return LANGUAGES.some((l) => l.code === v) ? v : AUTO;
}

const systemTags = () => globalThis.navigator?.languages?.length ? [...globalThis.navigator.languages] : [globalThis.navigator?.language].filter(Boolean);

export const language = resolveLanguage(savedLanguage(), systemTags());
const catalog = LANGUAGES.find((l) => l.code === language).catalog;
globalThis.document?.documentElement?.setAttribute('lang', language);

/** Saves the choice (AUTO or a code) and reloads, so every page and every string built at load starts again in it. */
export function setLanguage(choice) {
  try {
    if (choice === AUTO) storage()?.removeItem(STORAGE_KEY);
    else storage()?.setItem(STORAGE_KEY, choice);
  } catch { /* the choice lasts until the launcher closes */ }
  globalThis.location?.reload();
}

/**
 * The text in the player's language. `{name}` in the text is replaced from params; params are never translated, so a number, a
 * version or a name the player typed passes through as it is.
 */
export function t(text, params) {
  const out = catalog[text] ?? text;
  return params ? out.replace(/\{(\w+)\}/g, (m, k) => (k in params ? String(params[k]) : m)) : out;
}

/** Marks a text that is translated later with t() (a table of texts, see backend-text.js): the text itself, so check-i18n.mjs finds it. */
export const msg = (text) => text;

/** A whole number written the way the language does (1,234 or 1.234). */
export const num = (n) => Number(n).toLocaleString(language);

/** A date from an ISO string or timestamp, in the language's own format. */
export const date = (d, opts = { year: 'numeric', month: 'short', day: 'numeric' }) => new Date(d).toLocaleDateString(language, opts);
