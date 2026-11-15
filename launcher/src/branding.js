// The player's logo and icon (launcher.json `logo` and `icon`; Settings > Game > Logo and icon). 'classic' is the bubble-letter wordmark
// and R icon of 0.10, anything else the current Recompia art (the same rule as src-tauri/src/branding.rs, which puts the icon in the exes,
// this window and the Steam art). Pages draw their pictures with brandImg(); setBranding() swaps every one already on screen.
import logoUrl from './art/logo.png';
import logoClassicUrl from './art/logo-classic.png';
import markUrl from './art/mark.png';
import markClassicUrl from './art/mark-classic.png';
import markTestingUrl from './art/mark-testing.png';
import markTestingClassicUrl from './art/mark-testing-classic.png';

const ART = {
  logo: { recompia: logoUrl, classic: logoClassicUrl },
  mark: { recompia: markUrl, classic: markClassicUrl },
  'mark-testing': { recompia: markTestingUrl, classic: markTestingClassicUrl },
};

/** Which choice each picture follows: the wordmark the logo, the square marks the icon. */
const FOLLOWS = { logo: 'logo', mark: 'icon', 'mark-testing': 'icon' };

const lookOf = (v) => (String(v ?? '').trim().toLowerCase() === 'classic' ? 'classic' : 'recompia');

const current = { logo: 'recompia', icon: 'recompia' };

/** The picture of `kind` (logo, mark, mark-testing) for the current choice. */
export const brandUrl = (kind) => ART[kind][current[FOLLOWS[kind]]];

/** An <img> of `kind` that setBranding keeps up to date. `attrs`: the rest of the tag (class, alt). */
export const brandImg = (kind, attrs = '') => `<img data-brand="${kind}" src="${brandUrl(kind)}" ${attrs} />`;

/** Takes the choice from a launcher config and redraws every picture on screen. */
export function setBranding(cfg) {
  current.logo = lookOf(cfg?.logo);
  current.icon = lookOf(cfg?.icon);
  for (const img of document.querySelectorAll('img[data-brand]')) img.src = brandUrl(img.dataset.brand);
}
