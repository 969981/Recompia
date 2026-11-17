// The player's logo and icon (launcher.json `logo` and `icon`; Settings > Game > Logo and icon). 'classic' is the bubble-letter wordmark
// and R icon of 0.10, anything else the current Recompia art (the same rule as src-tauri/src/branding.rs, which puts the icon in the exes,
// this window and the Steam art; those stay Recompia's in every game). Pages draw their pictures with brandImg(); setBranding() swaps every one already on screen.
import logoUrl from './art/logo.png';
import logoClassicUrl from './art/logo-classic.png';
import markUrl from './art/mark.png';
import markClassicUrl from './art/mark-classic.png';
import markTestingUrl from './art/mark-testing.png';
import markTestingClassicUrl from './art/mark-testing-classic.png';

const RECOMPIA_ART = {
  logo: { recompia: logoUrl, classic: logoClassicUrl },
  mark: { recompia: markUrl, classic: markClassicUrl },
  'mark-testing': { recompia: markTestingUrl, classic: markTestingClassicUrl },
};

/** Each game's pictures (games.js ids). Co-op has no art of its own yet: it shows Recompia's, with its badge (games.js) beside them. */
const GAME_ART = { recompia: RECOMPIA_ART, coop: RECOMPIA_ART };

/** Which choice each picture follows: the wordmark the logo, the square marks the icon. */
const FOLLOWS = { logo: 'logo', mark: 'icon', 'mark-testing': 'icon' };

const lookOf = (v) => (String(v ?? '').trim().toLowerCase() === 'classic' ? 'classic' : 'recompia');

const current = { logo: 'recompia', icon: 'recompia', game: 'recompia' };

/** The picture of `kind` (logo, mark, mark-testing) of the current game and choice; `game` asks for another game's (the switcher). */
export const brandUrl = (kind, game = current.game) => (GAME_ART[game] ?? RECOMPIA_ART)[kind][current[FOLLOWS[kind]]];

/** An <img> of `kind` that setBranding keeps up to date. `attrs`: the rest of the tag (class, alt). `game`: a fixed game's picture
 *  instead of the current game's. */
export const brandImg = (kind, attrs = '', game = '') => `<img data-brand="${kind}"${game ? ` data-brand-game="${game}"` : ''} src="${brandUrl(kind, game || current.game)}" ${attrs} />`;

const redraw = () => {
  for (const img of document.querySelectorAll('img[data-brand]')) img.src = brandUrl(img.dataset.brand, img.dataset.brandGame || current.game);
};

/** Takes the choice from a launcher config and redraws every picture on screen. */
export function setBranding(cfg) {
  current.logo = lookOf(cfg?.logo);
  current.icon = lookOf(cfg?.icon);
  redraw();
}

/** Shows game `id`'s pictures (games.js) on every picture already on screen. */
export function setBrandGame(id) {
  current.game = id;
  redraw();
}
