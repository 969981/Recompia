// Cross-checks the page side against the Rust side, which have no compiler between them:
//   - every command src/api.js invokes is listed in generate_handler! in src-tauri/src/main.rs, and every command listed there is in
//     src/api.js (test_report is Rust-only: the external test harness reports through it, no page calls it);
//   - each api.js key is the camelCase of its command name;
//   - every `api.<key>` the pages use exists in src/api.js, and every key in api.js is used by a page;
//   - the dev mock (src/dev/mock.js) answers the same commands (printed as a note, not a failure).
// Run: node scripts/check-api.mjs   (exit 1 on a mismatch)
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const RUST_ONLY = new Set(['test_report']);

const walk = (dir) =>
  readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
const camel = (n) => n.replace(/_(\w)/g, (_, c) => c.toUpperCase());
const diff = (a, b) => [...a].filter((x) => !b.has(x)).sort();

const main = readFileSync(join(root, 'src-tauri/src/main.rs'), 'utf8');
const list = main.match(/generate_handler!\[([\s\S]*?)\]/)?.[1] ?? '';
const handler = new Set([...list.matchAll(/(?:\w+::)?(\w+)\s*,/g)].map((m) => m[1]));

const apiSrc = readFileSync(join(root, 'src/api.js'), 'utf8');
const entries = [...apiSrc.matchAll(/^\s+(\w+): \([^)]*\) => invoke\('(\w+)'/gm)].map((m) => ({ key: m[1], cmd: m[2] }));
const apiCmds = new Set(entries.map((e) => e.cmd));

const used = new Set();
for (const f of walk(join(root, 'src')).filter((p) => p.endsWith('.js') && !p.endsWith('api.js') && !p.includes(`${join('src', 'dev')}`))) {
  for (const m of readFileSync(f, 'utf8').matchAll(/(?<![\w/.])api\.(\w+)/g)) used.add(m[1]);
}

const mock = readFileSync(join(root, 'src/dev/mock.js'), 'utf8');
const mocked = new Set([...mock.matchAll(/case '(\w+)':/g)].map((m) => m[1]).filter((n) => handler.has(n)));

const problems = [];
const add = (what, items) => items.length && problems.push(`${what}: ${items.join(', ')}`);
add('in api.js but not in generate_handler!', diff(apiCmds, handler));
add('in generate_handler! but not in api.js', diff(handler, new Set([...apiCmds, ...RUST_ONLY])));
add('api.js key is not the camelCase of its command', entries.filter((e) => camel(e.cmd) !== e.key).map((e) => `${e.key} -> ${e.cmd}`));
add('used by a page but not in api.js', diff(used, new Set(entries.map((e) => e.key))));
add('in api.js but used by no page', diff(new Set(entries.map((e) => e.key)), used));
const note = diff(apiCmds, mocked);

console.log(`generate_handler!: ${handler.size} commands (${[...handler].filter((n) => RUST_ONLY.has(n)).join(', ')} Rust-only)`);
console.log(`api.js: ${apiCmds.size} commands, ${used.size} used by pages`);
if (note.length) console.log(`note: the dev mock does not answer: ${note.join(', ')}`);
if (problems.length) {
  console.error(problems.join('\n'));
  process.exit(1);
}
console.log('api cross-check passed');
