// The games the launcher shows, one row each. They are one install, one pkp.exe, one build and one shader cache: a game is which pages
// the sidebar offers, which picture and name the brand block shows, which Settings rows apply and whose mod set the Mods page edits. The
// Rust side does not know them; the choice is launcher.json `game` (config.rs), read by main.js. A later game is another row here.
//
// Row: { id, name, badge, home, pages, mods }
//   name   the full name, in the switcher and the page titles ('Recompia' is a name and stays in English)
//   badge  the short word beside the shared wordmark while the game has no art of its own (branding.js), or ''
//   home   the page the game opens on (and the pad's B goes back to)
//   pages  the page ids of main.js PAGES it offers, in no particular order (the sidebar keeps PAGES' order)
//   mods   whose mod set the Mods page edits: 'profiles' (the global defaults or a singleplayer profile) or 'coop' (one co-op group)
//
// Settings sections, groups and rows (settings-schema.js) may name `games: [...]`; one without it is shown in every game (shownIn).
// Pure: no DOM and no Tauri, so node scripts/test-games.mjs runs it.
import { t } from './i18n.js';

export const GAMES = [
  {
    id: 'recompia', name: 'Recompia', badge: '', home: 'home', mods: 'profiles',
    pages: ['home', 'linkplay', 'mods', 'achievements', 'saveedit', 'news', 'settings', 'setup'],
  },
  {
    // co-op saves are played only from here (coop_save.rs refuses one as the singleplayer save); the install itself is Recompia's
    id: 'coop', name: t('Recompia: Co-op'), badge: t('Co-op'), home: 'coop', mods: 'coop',
    pages: ['coop', 'mods', 'news', 'settings'],
  },
];

export const DEFAULT_GAME = GAMES[0].id;

/** The row of a saved choice; anything unknown (an older launcher.json has none) is Recompia. */
export const gameOf = (id) => GAMES.find((g) => g.id === id) ?? GAMES[0];

/** The game offers page `page`. */
export const hasPage = (game, page) => game.pages.includes(page);

/** The game a page is shown in: `game` itself when it has the page, else the first game that does (null: none). */
export const gameForPage = (game, page) => (hasPage(game, page) ? game : GAMES.find((g) => hasPage(g, page)) ?? null);

/** A settings section, group or row is shown in game `id`: it names no games, or names this one. */
export const shownIn = (item, id) => !item.games || item.games.includes(id);
