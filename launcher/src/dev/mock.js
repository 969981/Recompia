// Dev only: stands in for Rust in a plain browser, so every page runs with `npm run dev`. main.js loads it behind
// import.meta.env.DEV, so it never reaches the production bundle (checks/static.mjs nomock).
//   ?s=<scenario>    one of SCENARIOS below; the corner switcher flips them. &clean hides the switcher for screenshots.
import { mockIPC, mockWindows } from '@tauri-apps/api/mocks';
import { emit } from '@tauri-apps/api/event';

const params = new URLSearchParams(location.search);
const scenario = params.get('s') || 'ready';

const GAME_DIR = 'C:/Games/PokopiaRecomp-0.7.0';
const DUMP = { path: 'D:/Dumps/Pokopia/updates/2.0.1a', kind: 'update', version: '2.0.1a' };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const RELEASE_BODIES = {
  'v0.8.1-beta.2': `### Regressions and known problems
- The new Repair and reset dialog has only been tried on the dev mock and in unit tests so far
- The launcher lag fix was measured on NVIDIA with Linux only

### What to test
- **Repair and reset:** try each choice, then Undo. Report: which choice, what the list showed, any error words
- **Launcher speed:** change pages for a minute. Report: smooth or laggy, your graphics card

### Changes in this build
- #291 Launcher: Repair and reset (settings, caches, old copies) with a preview and Undo

### Launcher
- Added **Settings > About > Repair and reset**`,
  'v0.8.1-beta.1': `### Changes in this build
- #289 Launcher: fix the lag on Linux with NVIDIA (Testing build)

### Launcher
- Fixed the launcher feeling slow on Linux with an NVIDIA card`,
  'v0.7.0': `Extract it over your 0.6.x install, then run **Set up again** once: the game code and the shader translator changed. The launcher asks for it. Link Play builds from before this one cannot join this one, so update together.

### Highlights
- Link Play now speaks pokopia-native's protocol, so you can play with pokopia-native players on game 2.0.1.
- Text on 21:9 and 32:9 screens no longer runs together.
- House and area transitions take the same time at any frame rate, with no 60 fps hold.
- Fewer GPU freezes on NVIDIA, GPU counters that count once, and particle effects that were missing.
- Shader setup writes about 90 files instead of 361,000, which could take hours on a hard drive.
- More graphics memory for 4K, and a smaller default picture size on GPUs with 4 GB or less.

### Known issues
- Link Play cross-play passes pokopia-native's protocol tests but has not been confirmed in a full two-player session yet, and Story co-op has not been tried. Please report what you find.
- The larger graphics pools for 4K have not been confirmed in game yet ([#164](https://github.com/featureJosh/pokopia-recomp/issues/164)).`,
  'v0.6.2': `### Highlights
- The 3D world is drawn in the right place again below 100% scene size on Steam Deck, Legion Go S, 1440p and ultrawide screens.
- Frame limits above 60 and unlimited work again.
- Lamp lights no longer show a ghost copy at 16:10 or 1440p.
- The in-game clock shows your real time.
- New one-click presets: Steam Deck, Legion Go S, Laptop and Desktop PC.
- Steam Deck: smoother play, plus a batch of anti-stutter changes.`,
  'v0.6.1': `### Highlights
- Fixes for saves imported from an emulator.
- Fewer stutters the first time a new area loads.

### Fixes
- The launcher no longer loses the game folder after a move.`,
};

const release = (tag, name, date) => ({ tag, name, date, body: RELEASE_BODIES[tag], url: `https://github.com/featureJosh/pokopia-recomp/releases/tag/${tag}` });

const RELEASES = [
  release('v0.7.0', 'Pokopia Recomp 0.7.0 (build K: Kangaroo)', '2026-10-30T18:12:00Z'),
  release('v0.6.2', 'Pokopia Recomp 0.6.2 (build J: Jackrabbit)', '2026-10-11T09:40:00Z'),
  release('v0.6.1', 'Pokopia Recomp 0.6.1 (build J: Jackal)', '2026-09-27T20:05:00Z'),
];

const NEXT = {
  tag: 'v0.7.1',
  name: 'Pokopia Recomp 0.7.1 (build K: Koala)',
  date: '2026-11-05T17:30:00Z',
  url: 'https://github.com/featureJosh/pokopia-recomp/releases/tag/v0.7.1',
  body: `Install it from the launcher, or extract it over your 0.7.0 install.

### Highlights
- Steam Deck and Proton find hosts and are found on the LAN, with nothing to type.
- Pad combos take Switch names, and a bad value in the controls file no longer resets it.

### Fixes
- Link Play: a host under Proton is no longer invisible to Windows players.`,
};

const idle = { state: 'idle', code: null, crashed: false, files: [], message: '' };
const progress = (state = 'idle', extra = {}) => ({ state, phase: '', done: 0, total: 0, message: '', ...extra });
const sstatus = (extra = {}) => ({ progress: progress(), complete: true, started: true, rebuild: false, ...extra });

const ready = {
  readiness: { kind: 'ready', detail: '' }, dump: DUMP, exe_exists: true, setup: sstatus(), game: idle,
};

// what each scenario changes from `ready`. A readiness detail is the sentence Rust gives for that state.
const SCENARIOS = {
  'first-run': { readiness: { kind: 'no_files', detail: 'Choose your game files first.' }, dump: null, exe_exists: false, setup: sstatus({ complete: false, started: false }) },
  'bad-files': { readiness: { kind: 'bad_files', detail: 'The game files at D:/Dumps/Pokopia are not usable: This folder has no base/ or updates/ subfolder, and no exefs/ and romfs/ either. Pick the folder that contains them (for example Pokopia_v2.0.1a).' }, dump: null, setup: sstatus({ complete: false, started: true }) },
  ready: {},
  update: { update: NEXT },
  updating: { update: NEXT, updating: true },
  'setup-running': { readiness: { kind: 'resume', detail: 'Setup has not finished. Press Continue setup: it picks up where it stopped.' }, setup: sstatus({ complete: false, started: true }), autoSetup: true },
  rebuild: { readiness: { kind: 'rebuild', detail: 'This update changes the game code, so the game has to be built again (about 7 minutes). Your shaders and saves are kept.' }, setup: sstatus({ complete: false, rebuild: true }) },
  'mixed-release': { readiness: { kind: 'mixed_release', detail: 'pkp_rt.dll next to the launcher is from another release than the build tools in the tools folder (API 2 and an API before 2), so Set up would build a pkp.exe that does not run with it. Only part of an update is in place: extract the whole release zip over this folder, every file, then press Set up again.' }, setup: sstatus({ complete: false }) },
  running: { game: { ...idle, state: 'running', message: 'pkp.exe started (process 18244)' } },
  crash: { game: { state: 'exited', code: -1073741819, crashed: true, message: 'pkp.exe exited with code -1073741819 (0xC0000005).', files: [{ name: 'pkp_crash.txt', fresh: true }, { name: 'pkp.log', fresh: true }] } },
  'no-controls': { noControls: true },
  // not in the contract, for defects
  'after-update': { readiness: { kind: 'rebuild', detail: 'Finish the update.' }, setup: sstatus({ complete: false, rebuild: true }), updated: true },
  'preflight-warn': { readiness: { kind: 'setup', detail: 'Press Set up to build the game and prepare its shaders.' }, setup: sstatus({ complete: false, started: false }), preflight: 'warn' },
  'preflight-block': { readiness: { kind: 'setup', detail: 'Press Set up to build the game and prepare its shaders.' }, setup: sstatus({ complete: false, started: false }), preflight: 'block' },
  retranslate: { readiness: { kind: 'retranslate', detail: 'This version fixes how some shaders are translated. Press Finish update: it translates only those shaders again.' }, setup: sstatus({ complete: false }) },
  rebase: { readiness: { kind: 'rebase', detail: 'Your update holds only the files it changes, so the launcher now adds the missing ones from the base game. Press Finish update so their shaders are prepared too.' }, setup: sstatus({ complete: false }) },
  'setup-error': { readiness: { kind: 'resume', detail: 'Setup has not finished. Press Continue setup: it picks up where it stopped.' }, setup: sstatus({ complete: false, progress: progress('error', { message: 'The shader translator stopped after 41,200 of 99,012 shaders.\nD:/Games/Pokopia/shader_cache/corpus/unique.tsv could not be read: The disk is full. (os error 112)' }) }) },
  'setup-paused': { readiness: { kind: 'resume', detail: 'Setup has not finished. Press Continue setup: it picks up where it stopped.' }, setup: sstatus({ complete: false, progress: progress('cancelled', { message: 'Setup paused. Set up again continues where it stopped.' }) }) },
  'no-exe': { readiness: { kind: 'no_exe', detail: `pkp.exe was not found in ${GAME_DIR}. Put the launcher next to pkp.exe.` }, exe_exists: false },
  offline: { releases: [] },
  packed: { storage: 'packed' },
  'long-strings': {
    readiness: { kind: 'resume', detail: `Setup has not finished, and this is a very long sentence about ${'C:/Users/somebody/Games/With a really long folder name/'.repeat(3)}` },
    setup: sstatus({ complete: false, started: true }),
    releases: [{ ...RELEASES[0], name: 'Pokopia Recomp 0.7.0 (build K: Kangaroo with an absurdly long release name that goes on and on and on and on)', body: `### Highlights\n- ${'A very long highlight line that will not fit on the card. '.repeat(8)}` }, { ...RELEASES[1], date: 'not a date', body: '' }],
    version: '0.7.0-dev+g1234567890abcdef1234567890',
  },
  'api-down': { down: true },
};

const sc = { ...ready, ...SCENARIOS[scenario] };

let game = { ...idle, ...sc.game };
let setup = structuredClone(sc.setup);
let readiness = sc.readiness;
let dumpDir = sc.dump ? 'D:/Dumps/Pokopia' : '';
let relay = null;
let mockSteam = { found: true, running: false, accounts: 1, game_added: 0, launcher_added: 0 };
let mockChannel = 'stable';
let mockLook = { logo: '', icon: '' };
let mockOnline = { online_asked: false, online_name: '', online_salt: 'mock-salt' };   // the switch itself is pkp.online_presence
let mockRepair = null;
// the builds the launcher offers, as list_builds returns them, for a launcher that is 0.8.1-beta.6
const BACK = ['The game code may have to be built again for the older version, before it is installed or with Set up again after it: from a few minutes to an hour, depending on your PC.', 'Files that only the newer version has stay in the folder; they are not removed.'];
const MOCK_BUILDS = [
  { tag: 'v0.8.1-beta.7', name: 'Pokopia Recomp 0.8.1-beta.7 (Testing)', date: '2026-11-09T12:00:00Z', prerelease: true, installable: true, relation: 'newer', risk: 'none', reasons: [], url: 'https://github.com/featureJosh/Recompia/releases/tag/v0.8.1-beta.7',
    notes: '### Regressions and known problems\n- Going back to an older build has only been tried in tests so far\n\n### What to test\n- Open Choose a version and read a note. Report: what the list showed' },
  { tag: 'v0.8.1-beta.6', name: 'Pokopia Recomp 0.8.1-beta.6 (Testing)', date: '2026-11-08T17:00:00Z', prerelease: true, installable: true, relation: 'installed', risk: 'none', reasons: [], url: '', notes: '### Changes in this build\n- #298 The version in pkp.log' },
  { tag: 'v0.8.1-beta.5', name: 'Pokopia Recomp 0.8.1-beta.5 (Testing)', date: '2026-11-08T15:11:00Z', prerelease: true, installable: true, relation: 'older', risk: 'low', reasons: BACK, url: '', notes: '### What to test\n- Repair and reset' },
  { tag: 'v0.8.0', name: 'Pokopia Recomp 0.8.0 (build L: Lykoi)', date: '2026-11-08T11:32:00Z', prerelease: false, installable: true, relation: 'older', risk: 'low', reasons: BACK, url: '', notes: '### Highlights\n- Profiles\n- Update channels' },
  { tag: 'v0.7.9', name: 'Pokopia Recomp 0.7.9 (build K: Kiwi)', date: '2026-11-08T02:12:00Z', prerelease: false, installable: true, relation: 'older', risk: 'high',
    reasons: ['Profiles (a separate save and name for each player) arrived in 0.8.0. An older version ignores profiles.json and uses one save, so a second player\'s save may not be found.', ...BACK], url: '', notes: '### Highlights\n- Frame pacing fixes' },
  { tag: 'v0.7.0', name: 'Pokopia Recomp 0.7.0 (build K: Kangaroo)', date: '2026-10-30T18:12:00Z', prerelease: false, installable: false, relation: 'older', risk: 'high', reasons: BACK, url: '', notes: 'Old notes' },
];
const mockBackups = [
  { version: 1, name: '20261108-143000-auto', created: '2026-11-08T19:30:00Z', kind: 'auto', key: 'k', profile: 'Player', files: 6, bytes: 17825792, hash: 'a' },
  { version: 1, name: '20261108-142000-auto', created: '2026-11-08T19:20:00Z', kind: 'auto', key: 'k', profile: 'Player', files: 6, bytes: 17825000, hash: 'b' },
];
const mockProfiles = { active: 'default', profiles: [{ id: 'default', name: 'Player', uid: '', nickname: 'Player', net_uid: '' }] };
const mockPlayTimes = { default: 13104 };   // 3 h 38 min; a profile added in the mock has none yet

const settings = {
  game_dir: GAME_DIR,
  pkp_exists: true,
  controls_exists: !sc.noControls,
  pkp: {
    'fs.romfs': 'D:/Dumps/Pokopia/updates/2.0.1a/romfs', 'fs.save_dir': '', 'audio.volume': '0.8',
    'net.protocol': 'native', 'net.mode': 'auto', 'net.internet': 'false', 'net.relay': '', 'net.server': 'auto',
    'pkp.link_interact_everywhere': '0',
  },
  controls: sc.noControls ? {} : {
    'display.window_mode': 'borderless', 'display.width': '1920', 'display.height': '1080', 'display.render_scale': 'auto', 'display.fps_limit': 'auto', 'display.vsync': '1',
    'display.quality_preset': 'high', 'display.lod_bias': '-0.5', 'display.anisotropy': '8', 'display.keep_aspect': '1',
    'upscale.mode': 'fsr1', 'upscale.quality': 'quality', 'fps.show': '0', 'audio.volume': '0.8', 'audio.mute_unfocused': '1',
    'mouse.yaw': '0.000767945', 'mouse.pitch': '0.000767945', 'mouse.invert_x': '0', 'mouse.invert_y': '0', 'prompts.style': 'auto',
  },
};

const lanReport = {
  lan: [
    { ip: '192.0.2.42', adapter: 'Ethernet', default_route: true, public_profile: false },
    { ip: '198.51.100.1', adapter: 'VirtualBox Host-Only', default_route: false, public_profile: false },
  ],
  overlay: [{ ip: '203.0.113.7', adapter: 'Tailscale', label: 'internet via Tailscale' }],
  skipped: [{ ip: '172.26.112.1', adapter: 'vEthernet (WSL)', reason: 'virtual adapter' }],
  warning: null,
  firewall_hint: null,
};
// Link Play states for review: &ini=net.mode=join:192.0.2.7 (repeatable) sets a pkp.ini key, &lan=public|none the network, &gamecode=<code> room-code.txt
for (const kv of params.getAll('ini')) settings.pkp[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
if (params.get('lan') === 'public') Object.assign(lanReport, { warning: 'Windows treats this network as Public, so other PCs may not reach you. Set it to Private in Settings > Network.', firewall_hint: 'If Windows Firewall asks about this program, allow it on Private networks. If it was answered wrongly once, remove the Block rules for the program in Windows Defender Firewall > Allow an app.' });
if (params.get('lan') === 'none') Object.assign(lanReport, { lan: [], overlay: [], firewall_hint: 'If Windows Firewall asks about this program, allow it on Private networks.' });

const dumpCheck = (label) => ({ label, ok: true });
const candidate = {
  kind: 'update', version: '2.0.1a', path: DUMP.path, exefs: `${DUMP.path}/exefs`, romfs: `${DUMP.path}/romfs`, romfs_base: 'D:/Dumps/Pokopia/base/romfs',
  romfs_files: 48211, romfs_bytes: 13_204_881_920, ok: true,
  checks: ['main', 'sdk', 'subsdk0', 'rtld', 'main.npdm', 'romfs'].map(dumpCheck),
};
const dumpReport = (path) => (/bad/i.test(path) || scenario === 'bad-files'
  ? { path, exists: true, errors: ['No exefs folder was found. Expected base/ or updates/<version>/, each with exefs/ and romfs/.'], candidates: [], chosen: null, aoc_dir: '', aoc: [] }
  : { path, exists: true, errors: [], candidates: [candidate, { ...candidate, kind: 'base', version: '', path: 'D:/Dumps/Pokopia/base', romfs_base: '' }], chosen: candidate, aoc_dir: 'D:/Dumps/Pokopia/aoc', aoc: ['0100F4300BF2D001'] });

const check = (id, level, title, detail) => ({ id, level, title, detail });
const PREFLIGHT = {
  ok: { checks: [check('disk', 'ok', 'Disk space', '112 GB free')], blocked: false, warnings: 0, need_bytes: 38e9, suggest_gentle: false },
  warn: {
    checks: [
      check('disk', 'ok', 'Disk space', '112 GB free'),
      check('cpu', 'warn', 'This CPU runs hot under a long build', 'Use fewer cores keeps it cooler. Setup takes longer.'),
      check('av', 'warn', 'Windows Security is scanning the game folder', 'Setup can take much longer. You can add the folder as an exclusion.'),
    ],
    blocked: false, warnings: 2, need_bytes: 38e9, suggest_gentle: true,
  },
  block: {
    checks: [check('disk', 'fail', 'Not enough disk space', 'Setup needs 36 GB free on drive C and 11 GB are free.')],
    blocked: true, warnings: 0, need_bytes: 38e9, suggest_gentle: false,
  },
};

const emitGame = (s) => { game = s; emit('game-status', s); };

async function runPlay() {
  if (game.state === 'running') throw 'The game is already running.';
  emitGame({ ...idle, state: 'running', message: 'Checking the shaders the game builds while it runs...' });
  sleep(2500).then(async () => {
    emitGame({ ...idle, state: 'running', message: 'pkp.exe started (process 18244)' });
    await sleep(6000);
    emitGame({ ...idle, state: 'running', message: 'Translating the shaders the game built while it ran...' });
    await sleep(2000);
    emitGame({ state: 'exited', code: 0, crashed: false, files: [{ name: 'pkp.log', fresh: true }], message: 'pkp.exe exited normally.' });
  });
}

let setupTimer = null;

// The real sequence, as the build script and the shader job report it (pkp_build.py's `@progress`, shaders.rs): every
// phase has its own count, and the lift's message names the module and the step it is in. Compressed in time.
const MODULES = [['main', 230], ['sdk', 45], ['subsdk0', 4], ['rtld', 1]];
const LIFT_TOTAL = MODULES.reduce((n, m) => n + m[1], 0) * 1000;
const DUMP_FILES = ['main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'];

/** pkp_build.py lift(): a module's shards are its share, and inside it the passes are 2 % discovery, 60 % flags, 38 % text. */
function liftAt(f) {
  const done = f * LIFT_TOTAL;
  let k = 0;
  let at = 0;
  while (k < MODULES.length - 1 && done >= at + MODULES[k][1] * 1000) {
    at += MODULES[k][1] * 1000;
    k += 1;
  }
  const inner = Math.min(1, (done - at) / (MODULES[k][1] * 1000));
  const step = inner < 0.02 ? 'reading the game code' : inner < 0.62 ? 'working out the flags' : 'writing the code';
  return { done: Math.round(done), total: LIFT_TOTAL, message: `Translating the game code: ${MODULES[k][0]} (${k + 1} of ${MODULES.length}), ${step}` };
}

const PLAN = [
  { phase: 'verify', ms: 500, ticks: 3, at: (f) => ({ done: Math.round(f * 5), total: 5, message: f >= 1 ? 'Game files match 2.0.1a' : `Checking your game files: ${DUMP_FILES[Math.min(4, Math.round(f * 5))]}` }) },
  { phase: 'lift', ms: 7000, ticks: 140, at: liftAt },
  { phase: 'compile', ms: 5000, ticks: 50, at: (f) => ({ done: Math.round(f * 284), total: 284, message: `Compiling: ${Math.round(f * 284)} of 284` }) },
  { phase: 'link', ms: 700, ticks: 2, at: (f) => ({ done: f >= 1 ? 1 : 0, total: 1, message: f >= 1 ? 'pkp.exe built' : 'Linking pkp.exe' }) },
  { phase: 'finish', ms: 700, ticks: 2, at: (f) => ({ done: f >= 1 ? 1 : 0, total: 1, message: f >= 1 ? 'pkp.exe is ready (68 s)' : 'Removing the build files' }) },
  { phase: 'extract', ms: 1200, ticks: 6, at: (f) => ({ done: Math.round(f * 4), total: 4, message: f > 0 ? `Scanning game files: ${Math.round(f * 4)} of 4` : "Finding the game's shaders in your game files..." }) },
  { phase: 'recheck', ms: 600, ticks: 3, at: (f) => ({ done: Math.round(f * 99012), total: 99012, message: 'Checking the shaders an earlier version translated...' }) },
  { phase: 'translate', ms: 5000, ticks: 50, at: (f) => ({ done: Math.round(f * 99012), total: 99012, message: `Translating shaders: ${Math.round(f * 99012)} of 99,012` }) },
];

function runSetup() {
  if (setup.progress.state === 'running') throw 'Setup is already running.';
  let i = 0;
  let n = 0;
  const next = () => {
    if (i >= PLAN.length) {
      setup = sstatus();
      readiness = { kind: 'ready', detail: '' };
      emit('setup-status', progress('done', { message: 'Setup finished.' }));
      return;
    }
    const p = PLAN[i];
    n += 1;
    const f = Math.min(1, n / p.ticks);
    setup.progress = progress('running', { phase: p.phase, ...p.at(f) });
    emit('setup-status', setup.progress);
    if (f >= 1) {
      i += 1;
      n = 0;
    }
    setupTimer = setTimeout(next, p.ms / p.ticks);
  };
  setup.progress = progress('running', { message: 'Checking the setup tools...' });
  emit('setup-status', setup.progress);
  setupTimer = setTimeout(next, 400);
}

function pauseSetup() {
  clearTimeout(setupTimer);
  setup.progress = { ...setup.progress, state: 'cancelled', message: 'Paused. Finished work is kept.' };
  emit('setup-status', setup.progress);
}

let updateStop = null;
let unpackStop = false;

function runUpdate() {
  return new Promise((resolve, reject) => {
    const total = 148_000_000;
    let done = 0;
    const timer = setInterval(() => {
      done = Math.min(total, done + 2_100_000);
      emit('update-progress', { phase: 'download', done, total });
      if (done < total) return;
      clearInterval(timer);
      (async () => {
        for (const phase of ['verify', 'extract']) {
          emit('update-progress', { phase, step: '', done: 0, total: 0 });
          await sleep(1200);
        }
        // update.rs prepares the game code with the update's own build (pkp_build.py --prepare) while Play still works
        for (let n = 0; n <= 284; n += 4) {
          emit('update-progress', { phase: 'prepare', step: 'compile', done: n, total: 284 });
          await sleep(60);
        }
        for (const phase of ['apply', 'adopt', 'restart']) {
          emit('update-progress', { phase, step: '', done: 0, total: 0 });
          await sleep(1200);
        }
        location.search = '?s=ready';   // what a restart looks like from here
        resolve();
      })();
    }, 250);
    updateStop = () => {
      clearInterval(timer);
      reject({ code: 'update_cancelled', message: 'Update cancelled.' });
    };
  });
}

// shader storage: ?s=ready has loose files (the launcher offers to pack them), ?s=packed is packed already
const LOOSE = { mode: 'loose', loose_files: 99_012, loose_bytes: 26.3 * 2 ** 30, archive_shaders: 0, archive_bytes: 0, archive_raw_bytes: 0, free_estimate: 19.4 * 2 ** 30, complete: true, running: false };
const PACKED = { mode: 'compact', loose_files: 3, loose_bytes: 0.001 * 2 ** 30, archive_shaders: 99_012, archive_bytes: 5.2 * 2 ** 30, archive_raw_bytes: 26.3 * 2 ** 30, free_estimate: 0, complete: true, running: false };
let storage = structuredClone(sc.storage === 'packed' ? PACKED : sc.storage === 'none' ? { ...LOOSE, mode: 'none', loose_files: 0, loose_bytes: 0, complete: false } : LOOSE);
let repackStop = false;
let modsAutoUpdate = false;   // config mods_auto_update
let looseShaders = false;   // config loose_shaders
async function runRepack(kind) {
  repackStop = false;
  const total = kind === 'compact' ? storage.loose_files : storage.archive_shaders;
  for (let i = 0; i <= 40; i++) {
    if (repackStop) return { finished: false, shaders: 0, raw_bytes: 0, disk_bytes: 0, seconds: 1 };
    await emit('shader-storage-progress', { kind, done: Math.floor((total * i) / 40), total });
    await sleep(150);
  }
  storage = structuredClone(kind === 'compact' ? PACKED : LOOSE);
  return { finished: true, shaders: total, raw_bytes: 26.3 * 2 ** 30, disk_bytes: 5.2 * 2 ** 30, seconds: 6 };
}

async function runTicks(event, make, count = 40) {
  for (let i = 1; i <= count; i++) {
    await sleep(150);
    emit(event, make(i, count));
  }
}

// ?page=mods: three installed mods (one with code not allowed yet, one the launcher locks) and a short hub list
const mockMods = {
  dir: `${GAME_DIR}/mods`, gameplay: [], points: 2,
  mods: [
    { id: 'field_of_view', name: 'Field of View', version: '1.1.0', authors: 'The Modtopia contributors', license: 'MIT', description: 'A wider or narrower view.', source: 'field-of-view', gameplay: false, guessed: false, code: true, needs_yes: false, network: '', enabled: true, state: 'ok', message: '', hook_points: 2,
      settings: [
        { key: 'on', label: 'Change the field of view', description: '', kind: 'bool', restart: false, min: 0, max: 0, step: 0, choices: [], text: 'On', number: 1 },
        { key: 'scale', label: 'Field of view', description: '', kind: 'number', restart: false, min: 0.5, max: 1.6, step: 0.05, choices: [], text: '1.25', number: 1.25 },
      ] },
    { id: 'bigger_stacks', name: 'Bigger Stacks', version: '1.0.0', authors: 'The Modtopia contributors', license: 'MIT', description: '', source: 'bigger_stacks-1.0.0.pkmod', gameplay: true, guessed: false, code: true, needs_yes: true, network: '', enabled: false, state: 'off', message: 'switched off', hook_points: 1, settings: [] },
    { id: 'community_achievements', name: 'Community Achievements', version: '0.3.0', authors: 'The Modtopia contributors', license: 'MIT', description: 'Achievements for what you do in Pokopia.', source: 'community_achievements-0.3.0.pkmod', gameplay: false, guessed: false, code: true, needs_yes: false, network: 'achievements.pokopia-native.workers.dev', enabled: true, state: 'ok', message: '', hook_points: 0,
      settings: [
        { key: 'messages', label: 'Show a message when I earn an achievement', description: '', kind: 'bool', restart: false, min: 0, max: 0, step: 0, choices: [], text: 'On', number: 1 },
        { key: 'share', label: 'Share my achievements with other players (no name) and show how many have each', description: '', kind: 'bool', restart: false, min: 0, max: 0, step: 0, choices: [], text: 'On', number: 1 },
      ] },
  ],
};
// the launcher's locks (config achievements_unlocked / achievements_sharing_unlocked, inverted), marked on every list as mods_lock.rs does
const mockLocks = { kept: true, sharing: true };
const mockModOverrides = new Map();
function modOverrides(profile, id, create = false) {
  if (!profile) return null;
  if (!mockProfiles.profiles.some((p) => p.id === profile)) throw 'That profile does not exist.';
  if (create && !mockModOverrides.has(profile)) mockModOverrides.set(profile, new Map());
  const mods = mockModOverrides.get(profile);
  if (create && !mods.has(id)) mods.set(id, { settings: {} });
  return mods?.get(id);
}
function markedMods(profile = null) {
  const v = structuredClone(mockMods);
  v.profile = profile;
  v.locks = { ...mockLocks };
  for (const m of v.mods) {
    const overrides = modOverrides(profile, m.id);
    m.global_enabled = m.enabled;
    m.enabled_override = overrides?.enabled ?? null;
    m.enabled = (m.enabled_override ?? m.enabled) && !m.needs_yes;
    m.lockable = m.id === 'community_achievements';
    m.locked = m.lockable && mockLocks.kept;
    for (const s of m.settings) {
      s.overridden = !!overrides && Object.hasOwn(overrides.settings, s.key);
      if (s.overridden) Object.assign(s, overrides.settings[s.key]);
      s.lockable = m.lockable && s.key === 'share';
      s.locked = s.lockable && mockLocks.sharing;
    }
  }
  return v;
}
const mockCatalog = {
  offline: false, preview: false, game: '2.0.1', hub: 'https://pokopia-native.vercel.app/mods', submit: 'https://pokopia-native.vercel.app/mods/submit',
  groups: [{ key: 'qol', label: 'Quality of life' }, { key: 'gameplay', label: 'Gameplay' }, { key: 'community', label: 'Community' }],
  mods: [
    { id: 'auto_run', name: 'Auto-Run', version: '1.0.0', summary: 'Tap once to keep running.', description: 'Tap once to keep running: your character goes on forward (you still steer) until you tap again, pull the stick back or open a menu.\n\nWorks with the controller and the keyboard.',
      settings: ['Controller button that starts and stops it (the game no longer sees that button)', 'Keyboard key that starts and stops it', 'Run (hold B) instead of walking', 'Say so on the screen'], tested: ['2.0.1', '1.0.5'], group: 'qol', source: 'https://github.com/example/auto-run', size: 77330,
      authors: ['The Modtopia contributors'], license: 'MIT', gameplay: false, network: false, page: 'https://pokopia-native.vercel.app/mods/auto-run', available: true, installed: '', newer: false },
    { id: 'bigger_stacks', name: 'Bigger Stacks', version: '1.1.0', summary: 'More of an item in one slot.', description: 'More of an item in one slot. The game still counts what you hold, only the limit moves.', settings: ['Stack limit'], tested: [], group: 'gameplay', source: '', size: 4096,
      authors: ['The Modtopia contributors'], license: 'MIT', gameplay: true, network: false, page: '', available: true, installed: '1.0.0', newer: true },
    { id: 'community_achievements', name: 'Community Achievements', version: '1.0.0', summary: 'Achievements counted with the community.', description: '', settings: [], tested: ['2.0.1'], group: 'community', source: '', size: 0,
      authors: ['The Modtopia contributors'], license: 'MIT', gameplay: false, network: true, page: '', available: false, installed: '', newer: false },
  ],
};

// ?page=achievements: a short list with counts, two earned; the mod is installed and locked (Unlock, remove it on ?page=mods, Lock again)
const mockAch = (n, name, cat, goal, players) => ({ id: n, name, desc: `Have ${goal} of the ${cat}.`, cat, goal, picture: '', players });
const mockAchievements = {
  available: true, offline: false, mod_id: 'community_achievements',
  stats: { players: 240, kept: false },
  earned: { dex_5: 1762900000, habitat_5: 1762990000 },
  list: [
    mockAch('dex_5', 'New Neighbors', 'pokedex', 5, 200), mockAch('dex_25', 'Busy Street', 'pokedex', 25, 90), mockAch('dex_200', 'Living Encyclopedia', 'pokedex', 200, 2),
    mockAch('habitat_5', 'Home Sweet Habitat', 'habitats', 5, 150), mockAch('streak_30', 'Dedicated', 'streak', 30, 0), mockAch('relic_1', 'Dig It', 'relics', 1, 12),
  ],
};

function handle(cmd, args) {
  if (cmd.startsWith('plugin:window|') || cmd.startsWith('plugin:')) return null;
  if (sc.down && cmd !== 'get_releases') throw 'The launcher backend did not answer.';
  switch (cmd) {
    case 'launcher_state':
      return { readiness, game_dir: GAME_DIR, default_game_dir: GAME_DIR, exe_exists: sc.exe_exists, builds_exe: true, dump: sc.dump, version: sc.version || '0.7.0', updated: !!sc.updated };
    case 'setup_status': return structuredClone(setup);
    case 'game_status': return game;
    case 'get_releases': return { releases: sc.releases ?? (mockChannel === 'testing' ? [{ ...release('v0.8.1-beta.2', 'Pokopia Recomp 0.8.1-beta.2 (Testing)', '2026-11-12T12:00:00Z'), prerelease: true }, { ...release('v0.8.1-beta.1', 'Pokopia Recomp 0.8.1-beta.1 (Testing)', '2026-11-10T12:00:00Z'), prerelease: true }, ...(sc.update ? [NEXT, ...RELEASES] : RELEASES)] : (sc.update ? [NEXT, ...RELEASES] : RELEASES)), update: sc.update && mockChannel !== 'testing' ? { tag: NEXT.tag, name: NEXT.name, url: NEXT.url, size: 148_000_000 } : null };   // Testing offers no update (update.rs Channel::offers_updates)
    case 'install_update': return runUpdate();
    case 'cancel_update': return updateStop?.();
    case 'play': return runPlay();
    case 'preflight': return sleep(500).then(() => ({ ...PREFLIGHT[sc.preflight || 'ok'], need_bytes: (looseShaders ? 31 : 14) * 2 ** 30 }));
    case 'start_setup': return runSetup();
    case 'cancel_setup': return pauseSetup();
    case 'open_setup_log': case 'open_game_file': case 'open_game_dir': case 'open_save_dir': case 'open_link':
      console.info('[mock] open', cmd, args);
      return null;
    case 'get_config': return { dump_dir: dumpDir, game_dir: '', minimize_on_play: false, discord_presence: false, gentle_build: false, loose_shaders: looseShaders, channel: mockChannel, mods_auto_update: modsAutoUpdate, achievements_unlocked: !mockLocks.kept, achievements_sharing_unlocked: !mockLocks.sharing, ...mockLook, ...mockOnline };
    case 'save_config': modsAutoUpdate = !!args.config.mods_auto_update; looseShaders = !!args.config.loose_shaders; mockChannel = args.config.channel || 'stable'; mockLook = { logo: args.config.logo ?? '', icon: args.config.icon ?? '' };
      mockOnline = { online_asked: !!args.config.online_asked, online_name: args.config.online_name ?? '', online_salt: args.config.online_salt ?? '' };
      return null;
    case 'apply_branding': return null;
    case 'get_overview': return { game_dir: GAME_DIR, default_game_dir: GAME_DIR, exe_exists: sc.exe_exists, builds_exe: true };
    case 'pick_folder': return 'D:/Dumps/Pokopia';
    case 'pick_package': return 'D:/Downloads/pokopia-2.0.1a.nup';
    case 'pick_nsp': return 'D:/Downloads/Pokopia DLC [0400A3A01EE4D001].nsp';
    case 'import_dlc': return ['0400A3A01EE4D001'];
    case 'validate_dump': return dumpReport(args.path);
    case 'save_dump':
      dumpDir = args.path;
      readiness = { kind: 'setup', detail: 'Press Set up to build the game and prepare its shaders.' };
      return null;
    case 'import_package':
      unpackStop = false;
      return runTicks('unpack-progress', (i, n) => {
        if (unpackStop) throw { code: 'unpack_cancelled', message: 'Unpacking was cancelled.' };
        return { done: i, total: n, file: `romfs/data/pack_${i}.bin` };
      }).then(() => `${GAME_DIR}/nup_dump`);
    case 'cancel_unpack': unpackStop = true; return null;
    case 'read_settings': return structuredClone(settings);
    case 'prompt_icon_pack': throw 'assets/prompts/prompt_icons.pak not found in the game folder';   // the preview row says so
    case 'write_settings':
      for (const e of args.edits) settings[e.file][`${e.section}.${e.key}`] = e.value;
      return null;
    case 'shader_storage': return structuredClone(storage);
    case 'compact_shaders': return runRepack('compact');
    case 'expand_shaders': return runRepack('expand').then((r) => r.finished);
    case 'cancel_shader_storage': repackStop = true; return null;
    case 'dlss_status': return { available: true, present: false, dir: `${GAME_DIR}/upscalers/streamline` };
    case 'fetch_dlss': return runTicks('dlss-progress', (i, n) => ({ done: i, total: n, message: 'Downloading nvngx_dlss.dll' })).then(() => null);
    case 'repair_plan': return { changes: [
      ...(args.actions.includes('game_settings') ? [{ action: 'game_settings', file: 'pkp.ini', what: 'reset', bytes: 812, backup: true, groups: args.groups ?? [] }] : []),
      ...(args.actions.includes('controls') ? [{ action: 'controls', file: 'controls.ini', what: 'remove', bytes: 2210, backup: true }] : []),
      ...(args.actions.includes('launcher_prefs') ? [{ action: 'launcher_prefs', file: 'launcher.json', what: 'reset', bytes: 296, backup: true }] : []),
      ...(args.actions.includes('caches') ? [{ action: 'caches', file: 'pipeline_cache.bin', what: 'remove', bytes: 31457280, backup: false }] : []),
      ...(args.actions.includes('leftovers') ? [{ action: 'leftovers', file: 'pkp_rt.dll.0.7.6', what: 'remove', bytes: 18874368, backup: false }] : []),
    ] };
    case 'repair_apply': mockRepair = { name: '20261108-120000', files: ['pkp.ini'] }; return { done: ['Game settings (pkp.ini) are back to their defaults'], backup: '20261108-120000' };
    case 'repair_undo': mockRepair = null; return { done: ['pkp.ini restored'], backup: null };
    case 'repair_last': return mockRepair;
    case 'list_builds': return structuredClone(MOCK_BUILDS.filter((b) => args.channel === 'testing' || !b.prerelease));
    case 'install_version': {
      const b = MOCK_BUILDS.find((x) => x.tag === args.tag);
      if (!b) throw 'That build cannot be installed on this system.';
      if (b.risk !== 'none' && !args.accepted) throw 'Going back to an older build needs your confirmation first. Nothing was changed.';
      console.info('[mock] install_version', args.tag, 'accepted', args.accepted);
      return runUpdate();
    }
    case 'steam_status': return { ...mockSteam, game_name: mockChannel === 'testing' ? 'Recompia (Testing)' : 'Recompia', launcher_name: mockChannel === 'testing' ? 'Recompia Launcher (Testing)' : 'Recompia Launcher' };
    case 'steam_add': mockSteam[`${args.target}_added`] = 1; return `Added "${args.target === 'game' ? 'Recompia' : 'Recompia Launcher'}" to Steam for 1 account. Start Steam again to see it, with its artwork.`;
    case 'steam_remove': mockSteam[`${args.target}_added`] = 0; return 'Removed it from Steam. Start Steam again.';
    case 'backup_status': return { settings: { minutes: 0, keep: 10, on_exit: true }, count: mockBackups.length, bytes: mockBackups.reduce((n, b) => n + b.bytes, 0), last: mockBackups[0] ?? null };
    case 'backup_list': return structuredClone(mockBackups);
    case 'save_edit_read': return [{ path: 'G00/CMNDAT.BIN', data: '' }, { path: 'S00/SYDAT.BIN', data: '' }];
    case 'save_edit_write': return { files: args.files.length, safety: null };
    case 'save_edit_data_ready': return false;
    case 'save_edit_prepare': return runTicks('save-edit-data-progress', (i, n) => ({ done: i * 217, total: n * 217 })).then(() => null);
    case 'backup_now': mockBackups.unshift({ version: 1, name: `20261108-15${String(mockBackups.length).padStart(2, '0')}00-manual`, created: '2026-11-08T20:00:00Z', kind: 'manual', key: 'k', profile: 'Player', files: 6, bytes: 17825792, hash: String(mockBackups.length) }); return { result: 'made', entry: mockBackups[0] };
    case 'backup_restore': return { restored: mockBackups.find((b) => b.name === args.name), safety: null };
    case 'open_backup_dir': console.info('[mock] open backups'); return null;
    case 'profiles_list': return structuredClone(mockProfiles);
    case 'profiles_play_time': return { ...mockPlayTimes };
    case 'profile_add': mockProfiles.profiles.push({ id: `p${mockProfiles.profiles.length}`, name: args.name, uid: '', nickname: args.name, net_uid: '' }); return structuredClone(mockProfiles);
    case 'profile_rename': mockProfiles.profiles.find((p) => p.id === args.id).name = args.name; return structuredClone(mockProfiles);
    case 'profile_remove': mockProfiles.profiles = mockProfiles.profiles.filter((p) => p.id !== args.id); mockProfiles.active = 'default'; return structuredClone(mockProfiles);
    case 'profile_select': mockProfiles.active = args.id; return structuredClone(mockProfiles);
    case 'save_sources': return [{ label: 'Eden', path: 'C:/Users/player/AppData/Roaming/eden/nand/user/save/0000000000000000/1/0100F4300BF2C000' }];
    case 'import_save': return { source: 'Eden', files: 12, backup: `${GAME_DIR}/save.backup-20261106`, target: `${GAME_DIR}/save` };
    case 'relay_start':
      relay = { addr: `0.0.0.0:${args.port || 47610}`, udp: true, clients: 0, stats: {} };
      return relay;
    case 'relay_stop': relay = null; return null;
    case 'relay_status':
      if (relay) relay = { ...relay, clients: Math.floor(Date.now() / 8000) % 4 };
      return relay;
    case 'lan_report': return lanReport;
    case 'public_ip': return params.get('public') === 'none' ? null : '198.51.100.23';

    case 'mods_list': return markedMods(args.active ? mockProfiles.active : args.profile ?? null);
    case 'mods_set_enabled': {
      const m = mockMods.mods.find((m) => m.id === args.id);
      const on = args.inherit ? m.enabled : args.on;
      if (on && m.needs_yes && !args.allow) throw 'This mod\'s code is not allowed yet.';
      if (args.allow) m.needs_yes = false;
      if (args.profile) { const o = modOverrides(args.profile, m.id, true); if (args.inherit) delete o.enabled; else o.enabled = on; }
      else m.enabled = on;
      return markedMods(args.profile ?? null);
    }
    case 'mods_move': {
      const i = mockMods.mods.findIndex((m) => m.id === args.id), j = i + args.delta;
      if (j >= 0 && j < mockMods.mods.length) [mockMods.mods[i], mockMods.mods[j]] = [mockMods.mods[j], mockMods.mods[i]];
      return markedMods();
    }
    case 'mods_set_setting': {
      const s = mockMods.mods.find((m) => m.id === args.id).settings.find((x) => x.key === args.key);
      const value = args.number !== undefined
        ? { number: args.number, text: s.kind === 'choice' ? s.choices[args.number] : s.kind === 'bool' ? args.number ? 'On' : 'Off' : String(args.number) }
        : { text: args.text };
      if (args.profile) { const o = modOverrides(args.profile, args.id, true); if (args.inherit) delete o.settings[s.key]; else o.settings[s.key] = value; }
      else Object.assign(s, value);
      return markedMods(args.profile ?? null);
    }
    case 'mods_remove': mockMods.mods = mockMods.mods.filter((m) => m.id !== args.id); return markedMods();
    case 'mods_catalog': return structuredClone(mockCatalog);
    case 'mods_install': return markedMods();
    case 'mods_auto_update': return [];
    case 'mods_keep_locked': return [];
    case 'mods_set_lock': {
      mockLocks[args.what === 'mod' ? 'kept' : 'sharing'] = args.locked;
      const ca = mockMods.mods.find((m) => m.id === 'community_achievements');
      if (args.locked && ca && args.what === 'mod') ca.enabled = true;
      if (args.locked && ca && args.what === 'sharing') ca.settings.find((s) => s.key === 'share').number = 1;
      if (args.locked) for (const mods of mockModOverrides.values()) {
        const o = mods.get('community_achievements');
        if (o) { if (args.what === 'mod') delete o.enabled; else delete o.settings.share; }
      }
      return markedMods();
    }
    case 'open_mods_dir': return null;
    case 'achievements_view': return structuredClone(mockAchievements);
    default:
      console.warn('[mock] unknown command', cmd, args);
      throw `The mock has no command ${cmd}`;
  }
}

mockWindows('main');
mockIPC(handle, { shouldMockEvents: true });

if (sc.autoSetup) sleep(50).then(runSetup);
if (sc.updating) sleep(300).then(() => runUpdate().catch(() => {}));

function switcher() {
  if (params.has('clean')) return;
  const box = document.createElement('details');
  box.style.cssText = 'position:fixed;left:50%;bottom:4px;transform:translateX(-50%);z-index:30;font-size:11px;color:var(--color-text-2);background:var(--glass-tint-strong);border:1px solid var(--line);border-radius:10px;padding:2px 8px;max-width:640px;';
  const links = Object.keys(SCENARIOS).map((s) => `<a href="?s=${s}" style="color:${s === scenario ? 'var(--color-accent)' : 'inherit'};margin-right:10px;white-space:nowrap;display:inline-block">${s}</a>`);
  box.innerHTML = `<summary style="cursor:pointer">mock: ${scenario}</summary><div style="padding:6px 0">${links.join('')}</div>`;
  document.body.appendChild(box);
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', switcher);
else switcher();
