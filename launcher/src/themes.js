// The launcher's colours: its own three looks (theme.css) and the player's themes (src-tauri/src/themes.rs keeps and checks the packs; the
// style rule comes from themes-model.js). A player's theme is `<pack folder>/<id>`, saved in launcher.json `theme` (Rust needs it for the
// icon and the Steam pictures); one of the launcher's own looks is saved per PC in localStorage like before.
//
// The chosen theme is painted from a copy kept in localStorage (`userTheme`), so the window is right from its first frame; the list from
// Rust then confirms it, and a theme that is gone puts the launcher's own look back.
import { convertFileSrc } from '@tauri-apps/api/core';
import { api } from './api.js';
import { setBrandTheme } from './branding.js';
import { t } from './i18n.js';
import { brandOf, nextTheme, syncAction, themeCss } from './themes-model.js';
import { errText, toast } from './ui.js';

// Launcher colours: 'meadow' (a bright daytime scene made for the project, the default), 'sunrise' (amber to red; reads well with red-green colour blindness) or 'dusk' (the old violet).
// A value no longer in BUILTIN (such as an older launcher's 'koi') shows the default.
export const BUILTIN = ['sunrise', 'dusk', 'meadow'];
// theme names are proper names: they stay in English in every language
const BUILTIN_NAME = { sunrise: 'Sunrise', dusk: 'Dusk', meadow: 'Meadow' };
const CACHE = 'userTheme';
const SOFT_WIDTH = 160;   // the soft copy of the background, as wide as the launcher's own (art/*-soft.webp)
const SOFT_SMALL = 40;    // drawn this small first: scaling it back up is the blur (no pixel loops; ctx.filter is missing in WebKit)

let packs = [];       // themes.rs ThemeList.packs
let userKey = '';     // the chosen theme of a pack, '' for one of the launcher's own looks
let userName = '';
let pending = Promise.resolve();   // chooseTheme runs one at a time

function readBuiltin() {
  try { return BUILTIN.includes(localStorage.getItem('theme')) ? localStorage.getItem('theme') : 'meadow'; } catch { return 'meadow'; }
}

const readCache = () => { try { return JSON.parse(localStorage.getItem(CACHE)); } catch { return null; } };
const writeCache = (v) => { try { v ? localStorage.setItem(CACHE, JSON.stringify(v)) : localStorage.removeItem(CACHE); } catch { /* it is painted again at the next start */ } };

/** Where the window loads a pack's file from (the `theme-data` protocol); `version` makes a changed pack load again. */
function themeUrl(path, version) {
  let base;
  try { base = convertFileSrc(path, 'theme-data'); } catch { base = `theme-data://localhost/${encodeURIComponent(path)}`; }
  return `${base}?v=${version}`;
}

function findTheme(key) {
  for (const pack of packs) for (const theme of pack.themes) if (theme.key === key) return { pack, theme };
  return null;
}

/** The soft copy of a background: the picture drawn small, then up to SOFT_WIDTH with smoothing, as a data picture. Null when it cannot be read. */
function makeSoft(url) {
  return new Promise((resolve) => {
    const img = new Image();
    img.crossOrigin = 'anonymous';   // the protocol answers with an open origin; without it the canvas could not be exported
    img.onerror = () => resolve(null);
    img.onload = () => {
      try {
        const ratio = img.naturalHeight / img.naturalWidth;
        const canvas = (w) => Object.assign(document.createElement('canvas'), { width: w, height: Math.max(1, Math.round(w * ratio)) });
        const draw = (c, from) => {
          const ctx = c.getContext('2d');
          ctx.imageSmoothingQuality = 'high';
          ctx.drawImage(from, 0, 0, c.width, c.height);
          return c;
        };
        resolve(draw(canvas(SOFT_WIDTH), draw(canvas(SOFT_SMALL), img)).toDataURL('image/jpeg', 0.8));
      } catch { resolve(null); }
    };
    img.src = url;
  });
}

function styleTag() {
  let tag = document.getElementById('user-theme');
  if (!tag) {
    tag = document.createElement('style');
    tag.id = 'user-theme';
    document.head.append(tag);
  }
  return tag;
}

function install(css, brand) {
  styleTag().textContent = css;
  document.documentElement.dataset.theme = 'user';
  setBrandTheme(brand);
}

function uninstall() {
  document.getElementById('user-theme')?.remove();
  document.documentElement.dataset.theme = readBuiltin();
  setBrandTheme(null);
  userKey = '';
  userName = '';
  writeCache(null);
}

/** Paints theme `key` from the list and keeps a copy for the next start. False when the list has no such theme. */
async function paint(key) {
  const found = findTheme(key);
  if (!found) return false;
  const { pack, theme } = found;
  userKey = key;   // at once: the Settings page may draw while the picture is still being made
  userName = theme.name;
  const url = (file) => themeUrl(file, pack.version);
  const sharp = theme.background ? url(theme.background) : null;
  const soft = sharp ? await makeSoft(sharp) : null;
  const css = themeCss({ colors: theme.colors, art: { sharp, soft }, position: theme.background_position });
  const brand = brandOf(theme, url);
  install(css, brand);
  writeCache({ key, name: theme.name, version: pack.version, css, brand });
  return true;
}

function labelButton() {
  const b = document.getElementById('theme-btn');
  if (!b) return;
  const say = t('Colours: {name}', { name: userKey ? userName : BUILTIN_NAME[readBuiltin()] });
  b.querySelector('.nav-tip').textContent = say;
  b.setAttribute('aria-label', say);
}

/** At once, with no wait for Rust: the chosen look from what this PC kept. Call again after the rail is drawn (its button is new). */
export function applyTheme() {
  const cached = readCache();
  if (cached && typeof cached.css === 'string') {
    install(cached.css, cached.brand ?? null);
    userKey = cached.key;
    userName = cached.name ?? '';
  } else {
    document.documentElement.dataset.theme = readBuiltin();
  }
  labelButton();
}

/** Asks Rust for the packs and the chosen theme, and makes the window match (a changed pack is painted again, a gone theme drops). */
export async function syncThemes() {
  try {
    const [list, cfg] = await Promise.all([api.listThemes(), api.getConfig()]);
    packs = list.packs;
    const key = cfg.theme || '';
    const found = key ? findTheme(key) : null;
    const cached = readCache();
    const act = syncAction(key, found ? found.pack.version : null, cached, !!userKey);
    if (act === 'keep') { userKey = key; userName = found.theme.name; }
    else if (act === 'paint') await paint(key);
    else if (act === 'drop') uninstall();   // its pack is not there (now): the saved choice stays, so adding the pack again brings it back
  } catch (e) {
    console.warn('themes', e);   // the launcher's own look stays
  }
  labelButton();
}

/** After a pack was added or removed: the list again, and the chosen theme checked against it. */
export const reloadThemes = syncThemes;

export const getPacks = () => packs;
export const currentTheme = () => userKey || readBuiltin();

/** [value, label] of every look the player can pick: the launcher's own, then each theme of each pack. */
export function themeOptions() {
  const own = BUILTIN.map((id) => [id, BUILTIN_NAME[id]]);
  const theirs = packs.flatMap((p) => p.themes.map((th) => [th.key, p.themes.length === 1 && th.name === p.name ? th.name : `${p.name}: ${th.name}`]));
  return [...own, ...theirs];
}

/** Picks a look by its value (one of BUILTIN, or a theme key) and puts it everywhere: the window now, the icon and Steam pictures through Rust. */
export function chooseTheme(value) {
  const run = pending.then(() => choose(value));
  pending = run.catch(() => {});   // one pick at a time: a quick second click never writes an older choice over a newer
  return run;
}

async function choose(value) {
  if (BUILTIN.includes(value)) {
    try { localStorage.setItem('theme', value); } catch { /* the choice lasts until the launcher closes */ }
    uninstall();
    document.documentElement.dataset.theme = value;
    await api.setTheme({ theme: '' });
  } else {
    if (!(await paint(value))) return false;
    await api.setTheme({ theme: value });
  }
  labelButton();
  try {
    await api.applyBranding();   // the window icon, the exes and the Steam pictures follow the theme
  } catch (e) {
    toast(t('The new icon is not everywhere yet: {error}', { error: errText(e) }), 'error');
  }
  return true;
}

/** The Colours button: the next look (the launcher's own, then the player's themes). */
export function cycleTheme() {
  const next = nextTheme(BUILTIN, packs.flatMap((p) => p.themes.map((th) => th.key)), currentTheme());
  return chooseTheme(next).catch((e) => console.warn('theme', e));
}
