// Save Editor: PokoHex (GPL-3.0, github.com/berichan/PokoHex), a save editor that runs in the page, shown in a frame. It is built into
// the launcher by scripts/build-pokohex.sh (with pokohex/launcher-host.patch, which adds its host mode); a launcher built without it
// says so. Host mode: PokoHex looks for window.pokohexHost on this window and offers "Load Game save" and "Write to Game", which go
// to the save editor commands (saveedit.rs): the active profile's save in, the edited save back with a before-edit backup first.
import './saveedit.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { errText, esc, icon, toast } from '../ui.js';

const FRAME = 'pokohex/index.html';

/** PokoHex shows a rejected promise's message in its own error bar, so the launcher's words go in an Error. */
const asError = (e) => new Error(errText(e));

const host = {
  name: 'Game',
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

export async function render(el) {
  const bundled = await fetch(FRAME, { method: 'HEAD' }).then((r) => r.ok).catch(() => false);
  if (!bundled) {
    el.innerHTML = `<div class="page">
      <h1>${esc(t('Save Editor'))}</h1>
      <div class="banner">${icon('info')}<div>${esc(t('This launcher was built without the save editor. Build it with scripts/build-pokohex.sh, then build the launcher again.'))}</div></div>
    </div>`;
    return undefined;
  }
  window.pokohexHost = host;
  el.innerHTML = `<div class="page saveedit-page">
    <div class="saveedit-sheet glass"><iframe class="saveedit-frame" src="${FRAME}" title="${esc(t('Save Editor'))}"></iframe></div>
  </div>`;
  return () => {
    if (window.pokohexHost === host) delete window.pokohexHost;
  };
}
