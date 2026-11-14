// Save Editor: PokoHex (GPL-3.0, github.com/berichan/PokoHex), a save editor that runs in the page, shown in a frame. It is built into
// the launcher by scripts/build-pokohex.sh (with pokohex/launcher.patch: its host mode, and a build without the game's data); a
// launcher built without it says so. Host mode: PokoHex looks for window.pokohexHost on this window and offers "Load Game save" and
// "Write to Game", which go to the save editor commands (saveedit.rs): the active profile's save in, the edited save back with a
// before-edit backup first. The names and pictures it shows are made once from the player's own game files (saveedit_data.rs) and
// reach it through the pokohex-data protocol, host.gameData.
import './saveedit.css';
import { convertFileSrc } from '@tauri-apps/api/core';
import frameCss from './saveedit-frame.css?inline';
import fontCss from '@fontsource-variable/inter/opsz.css?url';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { errText, esc, icon, toast } from '../ui.js';

const FRAME = 'pokohex/index.html';
/** The launcher tokens the frame's stylesheet uses, copied onto the frame's root (a frame does not inherit custom properties). */
const TOKENS = [...new Set([...frameCss.matchAll(/var\((--[\w-]+)/g)].map((m) => m[1]))];

/** Gives the editor in the frame the launcher's look: its stylesheet, its font and the current theme's tokens. */
function dress(frame) {
  const doc = frame.contentDocument;
  if (!doc?.head) return;
  if (!doc.getElementById('launcher-look')) {
    const font = doc.createElement('link');
    font.rel = 'stylesheet';
    font.href = new URL(fontCss, location.href).href;
    const style = doc.createElement('style');
    style.id = 'launcher-look';
    style.textContent = frameCss;
    doc.head.append(font, style);
  }
  const from = getComputedStyle(document.documentElement);
  for (const name of TOKENS) doc.documentElement.style.setProperty(name, from.getPropertyValue(name));
}

/** PokoHex shows a rejected promise's message in its own error bar, so the launcher's words go in an Error. */
const asError = (e) => new Error(errText(e));

/** Where the editor loads the game data from: the pokohex-data protocol (http://pokohex-data.localhost/ on Windows). */
function gameDataUrl() {
  try {
    return convertFileSrc('', 'pokohex-data');
  } catch {
    return 'pokohex-data://localhost/';
  }
}

const host = {
  name: 'Game',
  gameData: null,
  read: () => api.saveEditRead().catch((e) => { throw asError(e); }),
  async write(files) {
    try {
      await api.saveEditWrite({ files });
    } catch (e) {
      throw asError(e);
    }
    toast(t('The edited save is written. The save as it was is kept in Settings > Saves > Backups.'), 'ok');
    return t('Written to the game. A backup was made first.');
  },
};

export async function render(el, ctx) {
  const bundled = await fetch(FRAME, { method: 'HEAD' }).then((r) => r.ok).catch(() => false);
  if (!bundled) {
    el.innerHTML = `<div class="page">
      <h1>${esc(t('Save Editor'))}</h1>
      <div class="banner">${icon('info')}<div>${esc(t('This launcher was built without the save editor. Build it with scripts/build-pokohex.sh, then build the launcher again.'))}</div></div>
    </div>`;
    return undefined;
  }
  let gone = false;
  let themes = null;

  const showEditor = () => {
    host.gameData = gameDataUrl();
    window.pokohexHost = host;
    // a pad moves through the editor as through the page (gamepad-nav.js, data-pad-frame)
    el.innerHTML = `<div class="page saveedit-page">
      <div class="saveedit-sheet glass"><iframe class="saveedit-frame" data-pad-frame src="${FRAME}" title="${esc(t('Save Editor'))}"></iframe></div>
    </div>`;
    const frame = el.querySelector('iframe');
    frame.addEventListener('load', () => dress(frame));
    // the colours button changes data-theme on the launcher's root: the frame follows
    themes = new MutationObserver(() => dress(frame));
    themes.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  };

  ctx.on((s, why) => {
    const bar = el.querySelector('#saveedit-bar');
    if (bar && why === 'saveedit' && s.saveEditData?.total) bar.value = (100 * s.saveEditData.done) / s.saveEditData.total;
  });

  // The names and pictures are made from the game files the first time (a few seconds), and again after the game files change.
  const prepare = async () => {
    el.innerHTML = `<div class="page saveedit-page">
      <h1>${esc(t('Save Editor'))}</h1>
      <div class="banner">${icon('info')}<div>
        <p>${esc(t("Making the editor's names and pictures from your game files. This happens once."))}</p>
        <progress max="100" id="saveedit-bar"></progress>
      </div></div>
    </div>`;
    try {
      await api.saveEditPrepare();
    } catch (e) {
      if (gone) return;
      el.innerHTML = `<div class="page saveedit-page">
        <h1>${esc(t('Save Editor'))}</h1>
        <div class="banner danger">${icon('circle-x')}<div>${esc(errText(e))}</div></div>
        <button class="btn" id="saveedit-retry">${esc(t('Try again'))}</button>
      </div>`;
      el.querySelector('#saveedit-retry').addEventListener('click', prepare);
      return;
    }
    if (!gone) showEditor();
  };

  if (await api.saveEditDataReady()) showEditor();
  else prepare();
  return () => {
    gone = true;
    themes?.disconnect();
    if (window.pokohexHost === host) delete window.pokohexHost;
  };
}
