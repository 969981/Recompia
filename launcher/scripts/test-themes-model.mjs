// Tests for the player's themes (src/themes-model.js). Run: node scripts/test-themes-model.mjs   (exit 1 on a failure)
import { TOKENS, brandOf, syncAction, contrast, deriveTokens, nextTheme, onAccent, parseColor, safeUrl, themeCss, toCss } from '../src/themes-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };
const same = (a, b, what) => check(JSON.stringify(a) === JSON.stringify(b), `${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);

// colours: numbers only
same(parseColor('#ffc23d'), { r: 255, g: 194, b: 61, a: 1 }, 'hex');
same(parseColor('#fc3'), { r: 255, g: 204, b: 51, a: 1 }, 'short hex');
check(Math.abs(parseColor('#ffffff80').a - 128 / 255) < 1e-9, 'hex with alpha');
same(parseColor('rgba(36, 18, 6, 0.44)'), { r: 36, g: 18, b: 6, a: 0.44 }, 'rgba');
same(parseColor('rgb(10 20 30)'), { r: 10, g: 20, b: 30, a: 1 }, 'rgb with spaces');
same(parseColor('rgba(10, 20, 30, 50%)'), { r: 10, g: 20, b: 30, a: 0.5 }, 'alpha in percent');
for (const bad1 of ['url(http://x)', 'red', '#ggg', '#12345', 'rgb(1,2,3);}', 'rgba(var(--a))', 'rgb(1,2,3) x', 'expression(1)', '', 'rgb()', null, 5, 'a'.repeat(50)]) {
  check(parseColor(bad1) === null, `refuses ${JSON.stringify(bad1)}`);
}
check(toCss(parseColor('#ABCDEF')) === '#abcdef', 'opaque back to hex');
check(toCss({ r: 1, g: 2, b: 3, a: 0.5 }) === 'rgba(1, 2, 3, 0.5)', 'translucent to rgba');

// derived tokens are complete, and text on the accent reads (4.5:1) for light, dark and mid accents
const full = deriveTokens({ accent: '#ffc23d', base: '#1c1006' });
for (const k of ['color-bg', 'color-menu', 'color-solid', 'color-solid-raised', 'color-accent', 'color-accent-hover', 'color-accent-press', 'color-on-accent', 'color-selection', 'tint-accent', 'color-focus', 'surface-sunken', 'glass-tint', 'glass-tint-strong', 'backdrop', 'rail-bg', 'scrim-hero', 'shadow-text']) {
  check(typeof full[k] === 'string' && full[k].length > 0, `derived ${k}`);
}
for (const accent of ['#ffffff', '#000000', '#7ee8c1', '#ff0000', '#3366ff', '#808080', '#ffc23d', '#2a1700']) {
  const a = parseColor(accent);
  check(contrast(onAccent(a), a) >= 4.5, `on-accent contrast for ${accent}: ${contrast(onAccent(a), a).toFixed(2)}`);
}
check(Object.keys(deriveTokens({})).length === 0, 'no colours, no tokens');
check(deriveTokens({ accent: 'not a colour' })['color-accent'] === undefined, 'a bad shorthand makes nothing');

check(full['scrim-hero'].includes('radial-gradient(ellipse 285px 270px at var(--hero-x) 57%'), 'the scrim has the spot behind the title');

// explicit tokens win, unknown or bad ones are dropped
const over = deriveTokens({ accent: '#ffc23d', 'color-accent': '#112233', 'color-warn': 'url(x)', 'font-ui': '#fff', 'color-info': '#b3c9ff' });
check(over['color-accent'] === '#112233', 'a named token wins over the shorthand');
check(!('color-warn' in over) && !('font-ui' in over), 'a bad colour and a token outside the list are dropped');
check(over['color-info'] === '#b3c9ff' && TOKENS.has('color-info'), 'an allowed token passes');

// the style rule: what is written is safe whatever the pack says
const URL = 'theme-data://localhost/camp%2Fbg.webp?v=123';
const css = themeCss({ colors: { accent: '#ffc23d', 'color-warn': 'rgba(1,2,3,0.5)' }, art: { sharp: URL, soft: null }, position: '50% 56%' });
check(css.startsWith(':root[data-theme="user"] {') && css.endsWith('}'), 'rule shape');
check(css.includes(`--art-url: url("${URL}");`) && css.includes(`--art-soft-url: url("${URL}");`), 'soft falls back to the sharp picture');
check(css.includes('--art-position: 50% 56%;') && css.includes('--color-accent: #ffc23d;'), 'position and colour written');
const hostile = themeCss({ colors: { accent: '#fff;} body{display:none', 'color-text': 'red; background: url(//evil)' }, art: { sharp: 'https://evil.example/x.png", x: url("y', soft: 'javascript:alert(1)' }, position: '50%;}' });
check(!/evil|javascript|display|url\(\/\//.test(hostile) && !hostile.includes('--art-url'), `hostile input writes nothing: ${hostile}`);
check(hostile.split('{').length === 2 && hostile.split('}').length === 2, 'one rule only');
check(safeUrl(URL) === URL && safeUrl('http://theme-data.localhost/camp%2Fbg.png') !== null, 'protocol addresses pass');
for (const u of ['https://x.example/a.png', 'theme-data://localhost/a"b', 'file:///etc/passwd', 'data:text/html;base64,AAAA', 'theme-data://localhost/a?v=x', null]) check(safeUrl(u) === null, `refuses address ${u}`);
check(safeUrl('data:image/jpeg;base64,/9j/4AAQ+=') !== null, 'a generated picture passes');

// the Colours button walks the launcher's looks, then the player's
same(nextTheme(['a', 'b'], ['p/x'], 'a'), 'b', 'next builtin');
same(nextTheme(['a', 'b'], ['p/x'], 'b'), 'p/x', 'into the player\'s themes');
same(nextTheme(['a', 'b'], ['p/x'], 'p/x'), 'a', 'wraps');
same(nextTheme(['a', 'b'], [], 'gone/y'), 'a', 'an unknown current starts over');

// branding: Co-op keeps its wordmark, Zonith never appears
same(brandOf({ logo: 'l', mark: 'm' }, (f) => `u:${f}`), { recompia: { logo: 'u:l', mark: 'u:m' }, coop: { mark: 'u:m' } }, 'brandOf');
check(!('zonith' in brandOf({ logo: 'l', mark: 'm' }, (f) => f)), 'zonith keeps its own');

// at start: a saved theme with an unchanged kept copy stays (the bug that opened in Meadow), a changed or missing copy is painted, a gone pack drops
const kept = { key: 'p/t', version: 7 };
same(syncAction('p/t', 7, kept, false), 'keep', 'saved theme, copy kept: keep');
same(syncAction('p/t', 7, kept, true), 'keep', 'saved theme, painted from the copy already: keep');
same(syncAction('p/t', 8, kept, true), 'paint', 'pack changed: paint again');
same(syncAction('p/t', 7, null, false), 'paint', 'no copy yet: paint');
same(syncAction('p/t', 7, { key: 'p/other', version: 7 }, true), 'paint', 'another theme was kept: paint');
same(syncAction('p/t', null, kept, true), 'drop', 'pack gone: drop');
same(syncAction('', null, kept, true), 'drop', 'nothing saved but a copy was painted: drop');
same(syncAction('', null, null, false), 'none', 'nothing saved, nothing painted: nothing');

// negative control: the comparison helper really fails on a wrong answer (that one failure is not counted)
{
  const kept = bad, log = console.log;
  console.log = () => {};
  same(1, 2, 'control');
  console.log = log;
  check(bad === kept + 1, 'the helper reports a wrong answer');
  if (bad === kept + 1) bad = kept;
}

console.log(bad ? `TEST FAILED (${bad})` : 'themes model tests passed');
process.exit(bad ? 1 : 0);
