// The launcher's logo and icon pictures, per game (games.js ids). src-tauri/src/branding.rs puts the icon in the exes, this window and
// the Steam art; those stay Recompia's in every game. Pages draw their pictures with brandImg(); setBrandGame() swaps every one already on screen.
import logoUrl from './art/logo.png';
import logoCoopUrl from './art/logo-coop.png';
import markUrl from './art/mark.png';
import markTestingUrl from './art/mark-testing.png';
import zonithLogoUrl from './art/zonith-logo.webp';
import zonithMarkUrl from './art/zonith-mark.png';

const RECOMPIA_ART = { logo: logoUrl, mark: markUrl, 'mark-testing': markTestingUrl };

/** Each game's pictures. Co-op has its own wordmark (with the Co-op badge in it) and Recompia's marks. Zonith has its own of each, from
 *  Zonith's assets/Zonith-Logo.png and assets/Zonith-Icon.png (its Testing mark is its mark: the channel is Recompia's). */
const GAME_ART = {
  recompia: RECOMPIA_ART,
  coop: { ...RECOMPIA_ART, logo: logoCoopUrl },
  zonith: { logo: zonithLogoUrl, mark: zonithMarkUrl, 'mark-testing': zonithMarkUrl },
};

let currentGame = 'recompia';
/** The player's theme (themes.js): { game: { kind: address } }. Only Recompia and Co-op are ever listed; Zonith keeps its own. */
let themeArt = {};

/** The picture of `kind` (logo, mark, mark-testing) of the current game; `game` asks for another game's. */
export const brandUrl = (kind, game = currentGame) => themeArt[game]?.[kind] ?? (GAME_ART[game] ?? RECOMPIA_ART)[kind];

/** An <img> of `kind` that setBrandGame keeps up to date. `attrs`: the rest of the tag (class, alt). `game`: a fixed game's picture
 *  instead of the current game's. */
export const brandImg = (kind, attrs = '', game = '') => `<img data-brand="${kind}"${game ? ` data-brand-game="${game}"` : ''} src="${brandUrl(kind, game || currentGame)}" ${attrs} />`;

function repaint() {
  for (const img of document.querySelectorAll('img[data-brand]')) img.src = brandUrl(img.dataset.brand, img.dataset.brandGame || currentGame);
}

/** Shows game `id`'s pictures (games.js) on every picture already on screen. */
export function setBrandGame(id) {
  currentGame = id;
  repaint();
}

/** The pictures of the chosen theme (themes-model.js brandOf), null for none; every picture already on screen follows. */
export function setBrandTheme(art) {
  themeArt = art ?? {};
  repaint();
}
