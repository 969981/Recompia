// What the launcher offers, in two levels. A product is one thing a player installs: Recompia (pkp.exe and its shader cache, set up by this
// launcher) or Zonith (totk.exe, built by Zonith's own setup, which the launcher downloads and drives: src-tauri/src/zonith*.rs). A game is
// one entry on the rail: a product's way to play, with its own Home, pages and Play button. Recompia has two: Recompia (solo) and
// Recompia: Co-op, which is the same install played together on a co-op save of its own, so it sits under Recompia on the rail and has a
// hero of its own on Recompia's Home. Zonith has one.
//
// Game row: { id, product, name, tab, icon, home, pages, mods, install }
//   product  the PRODUCTS id it belongs to (which install, readiness and update it uses)
//   name     the full name, for screen readers and messages ('Recompia' and 'Zonith' are names and stay in English)
//   tab      the short word on its rail entry
//   icon     the rail entry's icon (ui.js icon name), shown when the product has no mark of its own for that game
//   home     the page the game opens on (and the pad's B goes back to)
//   pages    the page ids of main.js PAGES it offers, in no particular order (the top bar keeps PAGES' order)
//   mods     whose mod set the Mods page edits: 'profiles' (the global defaults or a singleplayer profile), 'coop' (one co-op group) or
//            null (the game has no Mods page)
//   install  the game that looks after its product's install (Set up, Update, Repair, Builds); another game of the product leads there
//
// Settings sections, groups and rows (settings-schema.js) may name `games: [...]`; one without it is shown in every game that has a
// Settings page (shownIn). Zonith has none: its game settings live in its own in-game menu and totk.ini.
// Pure: no DOM and no Tauri, so node scripts/test-games.mjs runs it.
import { t } from './i18n.js';

export const PRODUCTS = [
  { id: 'recompia', name: 'Recompia', games: ['recompia', 'coop'] },
  { id: 'zonith', name: 'Zonith', games: ['zonith'] },
];

export const GAMES = [
  {
    id: 'recompia', product: 'recompia', name: 'Recompia', tab: t('Solo'), icon: 'user', home: 'home', mods: 'profiles', install: true,
    pages: ['home', 'linkplay', 'mods', 'achievements', 'saveedit', 'news', 'settings', 'setup'],
  },
  {
    // co-op saves are played only from here (coop_save.rs refuses one as the singleplayer save); the install itself is Recompia's
    id: 'coop', product: 'recompia', name: t('Recompia: Co-op'), tab: t('Co-op'), icon: 'users', home: 'coop', mods: 'coop', install: false,
    pages: ['coop', 'mods', 'news', 'settings'],
  },
  {
    // downloaded, built and started through Zonith's own setup (zonith.rs); nothing of Recompia's install, mods or saves applies
    id: 'zonith', product: 'zonith', name: 'Zonith', tab: 'Zonith', icon: 'gamepad-2', home: 'zonith', mods: null, install: true,
    pages: ['zonith', 'zonith-news'],
  },
];

export const DEFAULT_GAME = GAMES[0].id;

/** The row of a saved choice; anything unknown (an older launcher.json has none) is Recompia. */
export const gameOf = (id) => GAMES.find((g) => g.id === id) ?? GAMES.find((g) => g.id === DEFAULT_GAME);

/** The product row of a game (or of a product id). */
export const productOf = (game) => PRODUCTS.find((p) => p.id === (typeof game === 'string' ? game : game.product));

/** The games of a product, in rail order. */
export const gamesOf = (product) => GAMES.filter((g) => g.product === (typeof product === 'string' ? product : product.id));

/** The game offers page `page`. */
export const hasPage = (game, page) => game.pages.includes(page);

/** The game a page is shown in: `game` itself when it has the page, else the first game of the same product that does, else any game
 *  that does (null: none). Settings from Zonith opens Recompia's, which also holds the launcher's own options. */
export const gameForPage = (game, page) => {
  if (hasPage(game, page)) return game;
  return GAMES.find((g) => g.product === game.product && hasPage(g, page)) ?? GAMES.find((g) => hasPage(g, page)) ?? null;
};

/** The next game on the rail from `id`, wrapping (step +1 or -1): the pad's LT and RT. */
export const stepGame = (id, step) => {
  const i = GAMES.findIndex((g) => g.id === id);
  return GAMES[((i < 0 ? 0 : i) + step + GAMES.length) % GAMES.length];
};

/** A settings section, group or row is shown in game `id`: it names no games, or names this one. */
export const shownIn = (item, id) => !item.games || item.games.includes(id);
