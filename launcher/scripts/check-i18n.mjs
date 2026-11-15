// Checks the launcher's translations (src/i18n/*.js) against the code: every text passed to t('...') in src/ has an entry in every
// catalog, an entry keeps the {placeholders} of its English text, and no catalog holds an entry the code no longer asks for.
//   node scripts/check-i18n.mjs          lists what is missing or stale, exit 1 when anything is
//   node scripts/check-i18n.mjs --keys   prints the English texts the code asks for, one JSON string per line
// Texts are found by reading t('...') / t("...") and msg('...') calls (msg marks a text translated later, see backend-text.js) with a string first argument, so a text built from pieces cannot be translated:
// write the whole sentence with {placeholders} instead.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const SRC = fileURLToPath(new URL('../src', import.meta.url));
const CODES = ['es', 'ja', 'fr', 'it', 'de', 'pt-BR', 'ko', 'zh-CN', 'zh-TW'];

function files(dir) {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return name === 'i18n' || name === 'dev' ? [] : files(path);
    return path.endsWith('.js') ? [path] : [];
  });
}

const CALL = /(?<![\w.$])(?:t|msg)\(\s*('(?:\\.|[^'\\\n])*'|"(?:\\.|[^"\\\n])*")/g;
const keys = new Map();   // English text -> first file asking for it
for (const file of files(SRC)) {
  if (file.endsWith('i18n.js')) continue;
  for (const m of readFileSync(file, 'utf8').matchAll(CALL)) {
    const text = new Function(`return ${m[1]}`)();
    if (!keys.has(text)) keys.set(text, relative(SRC, file));
  }
}

if (process.argv.includes('--keys')) {
  for (const k of keys.keys()) console.log(JSON.stringify(k));
  process.exit(0);
}

const holes = (s) => [...String(s).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
let bad = 0;
const fail = (what) => { console.log(`FAIL ${what}`); bad++; };

for (const code of CODES) {
  const catalog = (await import(pathToFileURL(join(SRC, 'i18n', `${code}.js`)).href)).default;
  for (const [text, file] of keys) {
    if (!(text in catalog)) fail(`${code}: no translation for ${JSON.stringify(text)} (${file})`);
    else if (!String(catalog[text]).trim()) fail(`${code}: empty translation for ${JSON.stringify(text)}`);
    else if (holes(catalog[text]) !== holes(text)) fail(`${code}: {placeholders} differ for ${JSON.stringify(text)}`);
  }
  for (const text of Object.keys(catalog)) if (!keys.has(text)) fail(`${code}: stale entry ${JSON.stringify(text)}`);
}

console.log(`${keys.size} texts, ${CODES.length} languages${bad ? `, ${bad} problems` : ', all translated'}`);
process.exit(bad ? 1 : 0);
