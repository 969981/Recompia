// The player's own themes, pure part (themes.js draws them, src-tauri/src/themes.rs checks and keeps the packs). A theme names colours
// and pictures; this turns the colours into the style rule of `:root[data-theme="user"]`. Nothing of a pack's text reaches the style
// sheet except through here: a colour is parsed to numbers and written again from them, a picture address must match a fixed shape.
// No DOM and no Tauri, so node runs it (scripts/test-themes-model.mjs).

/** The tokens (theme.css) a pack may set by name, in `colors`. Anything else it names is ignored. */
export const TOKENS = new Set([
  'color-bg', 'color-menu', 'color-solid', 'color-solid-raised', 'color-text', 'color-text-2', 'color-text-3', 'color-selection',
  'color-accent', 'color-accent-hover', 'color-accent-press', 'color-on-accent', 'color-ok', 'color-warn', 'color-danger', 'color-info',
  'color-focus', 'tint-accent', 'tint-warn', 'tint-danger', 'tint-info', 'surface-sunken', 'glass-tint', 'glass-tint-strong',
  'backdrop', 'rail-bg', 'line', 'line-strong',
]);

const HEX = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
const FUNC = /^rgba?\(\s*([\d.]+)(%?)\s*[, ]\s*([\d.]+)(%?)\s*[, ]\s*([\d.]+)(%?)\s*(?:[,/ ]\s*([\d.]+)(%?)\s*)?\)$/i;

const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));

/** { r, g, b, a } (0 to 255, a 0 to 1) of `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb(...)` or `rgba(...)`; null for anything else. */
export function parseColor(s) {
  if (typeof s !== 'string' || s.length > 40) return null;
  const v = s.trim();
  const h = HEX.exec(v);
  if (h) {
    let x = h[1];
    if (x.length <= 4) x = [...x].map((c) => c + c).join('');
    const n = (i) => parseInt(x.slice(i, i + 2), 16);
    return { r: n(0), g: n(2), b: n(4), a: x.length === 8 ? n(6) / 255 : 1 };
  }
  const f = FUNC.exec(v);
  if (!f) return null;
  const ch = (num, pct) => { const n = Number(num); return Number.isFinite(n) ? clamp(pct ? n * 2.55 : n, 0, 255) : NaN; };
  const [r, g, b] = [ch(f[1], f[2]), ch(f[3], f[4]), ch(f[5], f[6])];
  const a = f[7] === undefined ? 1 : clamp(Number(f[7]) / (f[8] ? 100 : 1), 0, 1);
  return [r, g, b, a].some(Number.isNaN) ? null : { r, g, b, a };
}

const hex2 = (n) => Math.round(clamp(n, 0, 255)).toString(16).padStart(2, '0');

/** The CSS text of a colour: `#rrggbb` when opaque, else `rgba(r, g, b, a)`. Always built from the numbers. */
export function toCss(c) {
  return c.a >= 1 ? `#${hex2(c.r)}${hex2(c.g)}${hex2(c.b)}` : `rgba(${Math.round(c.r)}, ${Math.round(c.g)}, ${Math.round(c.b)}, ${Math.round(c.a * 1000) / 1000})`;
}

const mix = (c, to, t) => ({ r: c.r + (to.r - c.r) * t, g: c.g + (to.g - c.g) * t, b: c.b + (to.b - c.b) * t, a: c.a });
const WHITE = { r: 255, g: 255, b: 255, a: 1 };
const BLACK = { r: 0, g: 0, b: 0, a: 1 };
const lighten = (c, t) => mix(c, WHITE, t);
const darken = (c, t) => mix(c, BLACK, t);
const alpha = (c, a) => ({ ...c, a });

function luminance(c) {
  const lin = (v) => { const s = v / 255; return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

/** The WCAG contrast ratio of two opaque colours, 1 to 21. */
export function contrast(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

/** The text colour that reads on `accent`: a very dark shade of it, or white, whichever has more contrast. */
export function onAccent(accent) {
  const dark = darken(accent, 0.85);
  return contrast(dark, accent) >= contrast(WHITE, accent) ? dark : WHITE;
}

/** Every token a theme's `colors` give, as { 'color-accent': '#ffc23d', ... } (no `--`). `accent` and `base` are shorthands: the
 *  accent colour and the dark colour of the window; the tokens that follow from them (hover, text on the accent, glass, scrims) are made
 *  here. A token named in `colors` wins over what a shorthand made. */
export function deriveTokens(colors = {}) {
  const t = {};
  const base = parseColor(colors.base);
  const accent = parseColor(colors.accent);
  if (base) {
    const b = { ...base, a: 1 };
    const deep = darken(b, 0.35);
    const rgba = (c, a) => toCss(alpha(c, a));
    Object.assign(t, {
      'color-bg': toCss(b), 'color-menu': toCss(lighten(b, 0.08)), 'color-solid': toCss(lighten(b, 0.05)), 'color-solid-raised': toCss(lighten(b, 0.12)),
      'surface-sunken': rgba(deep, 0.4), 'glass-tint': rgba(deep, 0.5), 'glass-tint-strong': rgba(deep, 0.68),
      'backdrop': rgba(deep, 0.54), 'rail-bg': rgba(deep, 0.8),
      // left fade, a spot behind the Home title (--hero-x, theme.css), bottom fade: the same three layers the launcher's own themes use
      'scrim-hero': `linear-gradient(90deg, ${rgba(deep, 0.66)} 0%, ${rgba(deep, 0.36)} 32%, ${rgba(deep, 0)} 54%), radial-gradient(ellipse 285px 270px at var(--hero-x) 57%, ${rgba(deep, 0.66)} 0%, ${rgba(deep, 0.52)} 50%, ${rgba(deep, 0)} 100%), linear-gradient(0deg, ${rgba(deep, 0.78)} 0%, ${rgba(deep, 0.2)} 30%, ${rgba(deep, 0)} 44%)`,
      'shadow-text': `0 2px 40px ${rgba(deep, 0.6)}, 0 1px 3px ${rgba(deep, 0.35)}`,
    });
  }
  if (accent) {
    const a = { ...accent, a: 1 };
    Object.assign(t, {
      'color-accent': toCss(a), 'color-accent-hover': toCss(lighten(a, 0.18)), 'color-accent-press': toCss(darken(a, 0.1)),
      'color-on-accent': toCss(onAccent(a)), 'color-selection': toCss(alpha(a, 0.3)), 'tint-accent': toCss(alpha(a, 0.15)),
      'color-focus': toCss(lighten(a, 0.85)),
    });
  }
  for (const [k, v] of Object.entries(colors)) {
    const c = TOKENS.has(k) ? parseColor(v) : null;
    if (c) t[k] = toCss(c);
  }
  return t;
}

const URL_SHAPE = /^(?:theme-data:\/\/localhost\/|http:\/\/theme-data\.localhost\/)[A-Za-z0-9._~%/+-]+(?:\?v=\d{1,20})?$/;
const DATA_SHAPE = /^data:image\/(?:jpeg|png);base64,[A-Za-z0-9+/=]+$/;
const POSITION = /^\d{1,3}(?:\.\d{1,3})?% ?(?:\d{1,3}(?:\.\d{1,3})?%)?$/;

/** The picture address, when it is one the protocol serves (themes.js builds it) or a generated data picture; else null. */
export const safeUrl = (u) => (typeof u === 'string' && (URL_SHAPE.test(u) || DATA_SHAPE.test(u)) ? u : null);

/** The style rule of a theme. `art`: { sharp, soft } picture addresses (null: the launcher's own stays). `colors`: the theme's. Returns
 *  the rule text for `:root[data-theme="user"]`. */
export function themeCss({ colors = {}, art = {}, position = null } = {}) {
  const lines = [];
  const sharp = safeUrl(art.sharp);
  const soft = safeUrl(art.soft) ?? sharp;
  if (sharp) lines.push(`--art-url: url("${sharp}");`, `--art-soft-url: url("${soft}");`);
  if (sharp && position && POSITION.test(position)) lines.push(`--art-position: ${position};`);
  for (const [k, v] of Object.entries(deriveTokens(colors))) lines.push(`--${k}: ${v};`);
  return `:root[data-theme="user"] {\n  ${lines.join('\n  ')}\n}`;
}

/** What themes.js does at start with the saved theme `key`: 'keep' (it is painted from the copy this PC kept, and the pack is unchanged),
 *  'paint' (paint it from the pack), 'drop' (nothing is saved or its pack is gone: the launcher's own look; the saved key stays) or 'none'.
 *  `version`: the pack's version, null when the pack or the theme is not there. `cached`: { key, version } of the kept copy, or null. */
export function syncAction(key, version, cached, painted) {
  if (key && version !== null) return cached && cached.key === key && cached.version === version ? 'keep' : 'paint';
  return painted || cached ? 'drop' : 'none';
}

/** The key the Colours button picks after `current` among the launcher's own looks, then the player's themes. */
export function nextTheme(builtins, userKeys, current) {
  const all = [...builtins, ...userKeys];
  return all[(all.indexOf(current) + 1) % all.length];
}

/** The pictures a theme gives the launcher's own pages, by the names branding.js uses; a picture it lacks is not listed. Co-op keeps its
 *  own wordmark (it carries a Co-op badge); Zonith keeps everything. */
export function brandOf(theme, url) {
  const out = {};
  const put = (kind, file, games) => { const u = file ? url(file) : null; if (u) for (const g of games) (out[g] ??= {})[kind] = u; };
  put('logo', theme.logo, ['recompia']);
  put('mark', theme.mark, ['recompia', 'coop']);
  return out;
}
