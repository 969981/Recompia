// Every row of the Settings page, and the pure mapping between the files and the controls. No DOM, no Tauri: `node --test` runs it.
// A new setting is one entry in SECTIONS; pages/settings.js draws and saves it without being touched.
// Keys and defaults: runtime/src/gfx/play/play_config.cpp, play_types.h, runtime/docs/CONFIG.md. A missing key reads as the default in
// play_types.h, which is what the game does with it.
//
// A source is { pkp, controls, config, controls_exists }: read_settings' `section.key` maps plus get_config.
// An edit is { file, section, key, value }. file 'pkp' and 'controls' go to write_settings, 'config' to save_config.
//
// Field: { id, label, type, at, def, options, min, max, step, suffix, ... }
//   type     segmented | select | choice | size | toggle | slider | path
//   at       where it lives: controls(), pkp() or config(); an array when it has two keys
//   read     (src) => the control's value, when the plain mapping of `type` does not fit
//   write    (value, src) => edits, likewise
//   when     (values, env) => shown or not
//   enabled  (src) => false disables the row; the default disables what lives in a controls.ini that does not exist yet
//   lock     (env) => true disables the row for now
//   replaces true: writes other fields too, so the page closes any Custom number field it holds open
//   quality  'value' | 'frame': a value quality presets set, so changing it by hand makes the preset Custom (see markCustom)
// A choice is a list of presets plus Custom with a number field (custom: { min, max, suffix, start, label }), a size likewise with two.
// Rows that are not fields: info (label, value, actions), keycap (label, key), note (a line of text), and { expand } for rows made from env.

const controls = (section, key) => ({ file: 'controls', section, key });
const pkp = (section, key) => ({ file: 'pkp', section, key });
const config = (key) => ({ file: 'config', section: '', key });

const keyOf = (at) => (at.file === 'config' ? at.key : `${at.section}.${at.key}`);
const rawOf = (src, at) => src[at.file]?.[keyOf(at)];
const has = (src, at) => {
  const raw = rawOf(src, at);
  return raw != null && String(raw).trim() !== '';
};
const edit = (at, value) => ({ ...at, value });
const atsOf = (f) => [f.at ?? []].flat();

const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const fwd = (s) => String(s).trim().replace(/\\/g, '/');
/** The runtime's name_key(): case and `-` are forgiven in the words of the file. */
const word = (v) => String(v).trim().toLowerCase().replace(/-/g, '_');

// ---- quality presets --------------------------------------------------------------------------------------------------------------

// Ids, labels and which ones set the frame values: kQualityPresets in runtime/src/gfx/play/play_types.cpp (labels without its bracket).
// The values: kPresets in runtime/games/pkp/pkp_quality.cpp, which the F1 page applies. The tests parse both files.
// render_scale 0 and fps_limit -1 are written as auto.
export const QUALITY_PRESETS = [
  { id: 'switch2', label: 'Switch 2', view_distance: 1, lod_bias: 0, anisotropy: 0 },
  { id: 'high', label: 'High', view_distance: 1.5, lod_bias: -0.5, anisotropy: 8 },
  { id: 'ultra', label: 'Ultra', view_distance: 2, lod_bias: -1, anisotropy: 16 },
  { id: 'steamdeck', label: 'Steam Deck', view_distance: 1, lod_bias: 0, anisotropy: 4, render_scale: 75, fps_limit: 40 },
  { id: 'legion_go_s', label: 'Legion Go S', view_distance: 1.25, lod_bias: 0, anisotropy: 8, render_scale: 80, fps_limit: 60 },
  { id: 'laptop', label: 'Laptop', view_distance: 1, lod_bias: 0, anisotropy: 4, render_scale: 75, fps_limit: 60 },
  { id: 'desktop', label: 'Desktop PC', view_distance: 2, lod_bias: -0.5, anisotropy: 16, render_scale: 0, fps_limit: -1 },
];

const AT_PRESET = controls('display', 'quality_preset');
const AT_RENDER_SCALE = controls('display', 'render_scale');
const AT_FPS_LIMIT = controls('display', 'fps_limit');

function presetEdits(id) {
  const p = QUALITY_PRESETS.find((q) => q.id === id);
  if (!p) return [];   // Custom is what a preset becomes, never something picked: the F1 list does not offer it either
  const edits = [
    edit(AT_PRESET, id),
    edit(controls('display', 'view_distance'), String(p.view_distance)),
    edit(controls('display', 'lod_bias'), String(p.lod_bias)),
    edit(controls('display', 'anisotropy'), String(p.anisotropy)),
  ];
  if (p.render_scale !== undefined) edits.push(edit(AT_RENDER_SCALE, p.render_scale > 0 ? String(p.render_scale) : 'auto'));
  if (p.fps_limit !== undefined) edits.push(edit(AT_FPS_LIMIT, p.fps_limit >= 0 ? String(p.fps_limit) : 'auto'));
  return edits;
}

/** quality_preset_edited() of play_types.cpp: the render scale and the frame limit only belong to the device presets. */
function markCustom(kind, src) {
  const cur = word(has(src, AT_PRESET) ? rawOf(src, AT_PRESET) : 'switch2');
  if (cur === 'custom') return [];
  const setsFrame = QUALITY_PRESETS.find((q) => q.id === cur)?.render_scale !== undefined;
  return kind === 'frame' && !setsFrame ? [] : [edit(AT_PRESET, 'custom')];
}

// ---- volume and mouse ---------------------------------------------------------------------------------------------------------------

const AT_VOLUME = controls('audio', 'volume');
const AT_VOLUME_PKP = pkp('audio', 'volume');

/** [audio] volume in pkp.ini beats controls.ini and undoes the F1 slider, so what it says is what plays. Shown as 0 to 100. */
function readVolume(src) {
  const at = [AT_VOLUME_PKP, AT_VOLUME].find((a) => has(src, a));
  const v = at ? parseFloat(rawOf(src, at)) : 1;
  return Math.round(clamp(Number.isFinite(v) ? v : 1, 0, 1) * 100);
}

function writeVolume(value, src) {
  const text = String(value / 100);
  return has(src, AT_VOLUME_PKP) ? [edit(AT_VOLUME, text), edit(AT_VOLUME_PKP, text)] : [edit(AT_VOLUME, text)];
}

export const SOURCE_RAD = (0.022 * Math.PI) / 180;   // kSourceRadPerCount: radians per mouse count at sensitivity 1
const AT_YAW = controls('mouse', 'yaw');
const AT_PITCH = controls('mouse', 'pitch');
const SENS_MIN = 0.1;
const SENS_MAX = 20;
const SENS_DEFAULT = 2;   // 0.000767945 rad in play_types.h

function readSensitivity(src) {
  const rad = parseFloat(rawOf(src, AT_YAW));
  return Math.round(clamp(Number.isFinite(rad) ? rad / SOURCE_RAD : SENS_DEFAULT, SENS_MIN, SENS_MAX) * 10) / 10;
}

function writeSensitivity(value) {
  const rad = (clamp(value, SENS_MIN, SENS_MAX) * SOURCE_RAD).toPrecision(9);
  return [edit(AT_YAW, rad), edit(AT_PITCH, rad)];
}

// ---- the plain mapping of each type -------------------------------------------------------------------------------------------------

const wordOf = (f, src) => {
  const v = word(has(src, f.at) ? rawOf(src, f.at) : f.def);
  return f.aliases?.[v] ?? v;
};

const READ = {
  segmented: wordOf,
  select: wordOf,
  choice: wordOf,
  size: (f, src) => atsOf(f).map((at, i) => (has(src, at) ? String(rawOf(src, at)).trim() : f.def.split('x')[i])).join('x'),
  toggle: (f, src) => truthy(has(src, f.at) ? rawOf(src, f.at) : f.def),
  slider: (f, src) => {
    const v = parseFloat(has(src, f.at) ? rawOf(src, f.at) : f.def);
    const near = Math.round(clamp(Number.isFinite(v) ? v : f.def, f.min, f.max) / f.step) * f.step;
    return +near.toFixed(2);
  },
  path: (f, src) => String(rawOf(src, f.at) ?? ''),
};

const WRITE = {
  segmented: (f, v) => [edit(f.at, String(v))],
  select: (f, v) => [edit(f.at, String(v))],
  choice: (f, v) => [edit(f.at, String(v))],
  size: (f, v) => String(v).split('x').map((part, i) => edit(atsOf(f)[i], part)),
  toggle: (f, v) => [edit(f.at, f.at.file === 'config' ? !!v : v ? '1' : '0')],
  slider: (f, v) => [edit(f.at, String(v))],
  path: (f, v) => [edit(f.at, fwd(v))],
};

// ---- the page ---------------------------------------------------------------------------------------------------------------------

const REPO_URL = 'https://github.com/featureJosh/pokopia-recomp';
const open = (url, label = 'Open') => ({ act: 'open-url', arg: url, label, icon: 'external-link' });
const hasControls = (src) => src.controls_exists !== false;

export const DLSS_DOWNLOAD = {
  title: 'Download DLSS?',
  html: `<p>NVIDIA licenses this file separately, so it does not come with the launcher. It comes from NVIDIA's DLSS repository on GitHub, and its version and hash are checked.</p>
    <p>By downloading it you accept the NVIDIA RTX SDKs licence: <code class="selectable">licenses/LICENSE.NVIDIA-RTX-SDKs.txt</code></p>`,
  ok: 'Download',
};

export const IMPORT_FACTS = 'Your current save moves to a backup folder first, and the source is only read. A copied save cannot join its original in Link Play.';

export const SECTIONS = [
  {
    id: 'display',
    label: 'Display',
    groups: [
      {
        title: 'Picture',
        rows: [
          {
            id: 'window_mode', label: 'Window', type: 'segmented', at: controls('display', 'window_mode'), def: 'windowed',
            options: [{ value: 'windowed', label: 'Windowed' }, { value: 'borderless', label: 'Borderless' }, { value: 'exclusive', label: 'Fullscreen' }],
          },
          {
            id: 'window_size', label: 'Window size', type: 'size', at: [controls('display', 'width'), controls('display', 'height')], def: '1280x720',
            min: 320, max: 16384, when: (v) => v.window_mode === 'windowed',
            options: [
              { value: '1280x720', label: '1280 x 720' }, { value: '1600x900', label: '1600 x 900' }, { value: '1920x1080', label: '1920 x 1080' },
              { value: '2560x1440', label: '2560 x 1440' }, { value: '3840x2160', label: '3840 x 2160' },
            ],
          },
          {
            // 100 is 1920 x 1080: the percentages are the game's own size, see render_scale in play_types.h
            id: 'render_scale', label: 'Render resolution', type: 'choice', at: AT_RENDER_SCALE, def: '100', quality: 'frame',
            custom: { min: 25, max: 400, suffix: '%', start: 100, label: 'Custom render resolution in percent' },
            options: [
              { value: 'auto', label: 'Match window' }, { value: '67', label: '720p' }, { value: '100', label: '1080p' },
              { value: '133', label: '1440p' }, { value: '200', label: '2160p' },
            ],
          },
          { id: 'keep_aspect', label: 'Keep 16:9 picture', type: 'toggle', at: controls('display', 'keep_aspect'), def: '1' },
        ],
      },
      {
        title: 'Frame rate',
        rows: [
          {
            id: 'fps_limit', label: 'Frame rate limit', type: 'choice', at: AT_FPS_LIMIT, def: '60', quality: 'frame',
            custom: { min: 0, max: 1000, suffix: 'fps', start: 60, label: 'Custom frame rate limit' },
            options: [
              { value: 'auto', label: 'Match display' }, { value: '30', label: '30' }, { value: '60', label: '60' },
              { value: '120', label: '120' }, { value: '144', label: '144' }, { value: '0', label: 'Unlimited' },
            ],
          },
          { id: 'vsync', label: 'VSync', type: 'toggle', at: controls('display', 'vsync'), def: '0' },
          { id: 'fps_show', label: 'Frame rate counter', type: 'toggle', at: controls('fps', 'show'), def: '0' },
        ],
      },
    ],
  },
  {
    id: 'graphics',
    label: 'Graphics',
    groups: [
      {
        title: 'Quality',
        rows: [
          {
            id: 'quality_preset', label: 'Quality preset', type: 'select', at: AT_PRESET, def: 'switch2',
            options: QUALITY_PRESETS.map((q) => ({ value: q.id, label: q.label })),
            write: (value) => presetEdits(value), replaces: true,
          },
          { id: 'view_distance', label: 'View distance', type: 'slider', at: controls('display', 'view_distance'), def: 1, min: 1, max: 4, step: 0.05, suffix: 'x', quality: 'value' },
        ],
      },
      {
        title: 'Upscaling',
        rows: [
          {
            // the runtime keeps xess but runs it as off; spatial is its other word for fsr1
            id: 'upscale_mode', label: 'Upscaler', type: 'select', at: controls('upscale', 'mode'), def: 'off',
            aliases: { spatial: 'fsr1', xess: 'off' }, guard: 'dlss', lock: (env) => env.dlssBusy,
            options: [
              { value: 'off', label: 'Off' }, { value: 'fsr1', label: 'FSR 1' }, { value: 'fsr3', label: 'FSR 3' }, { value: 'nis', label: 'NIS' },
              { value: 'dlss', label: 'DLSS', offer: (env) => env.dlssOffered }, { value: 'dlaa', label: 'DLAA', offer: (env) => env.dlssOffered },
            ],
          },
          {
            id: 'upscale_quality', label: 'Upscaling quality', type: 'select', at: controls('upscale', 'quality'), def: 'quality',
            when: (v) => ['fsr3', 'dlss'].includes(v.upscale_mode),
            options: [
              { value: 'native', label: 'Native' }, { value: 'quality', label: 'Quality' }, { value: 'balanced', label: 'Balanced' },
              { value: 'performance', label: 'Performance' }, { value: 'ultra_performance', label: 'Ultra performance' },
            ],
          },
          {
            id: 'dlss_file', label: 'DLSS file', type: 'info', value: (env) => (env.dlssPresent ? 'Downloaded' : 'Missing'),
            when: (v, env) => ['dlss', 'dlaa'].includes(v.upscale_mode) && env.dlssCanFetch,
            actions: (env) => [{ act: 'dlss-get', label: env.dlssPresent ? 'Download again' : 'Download', icon: 'download', disabled: env.dlssBusy }],
          },
        ],
      },
      { title: '', rows: [{ id: 'f1_graphics', label: 'More options in game', type: 'keycap', key: 'F1' }] },
    ],
  },
  {
    id: 'audio',
    label: 'Audio',
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'volume', label: 'Volume', type: 'slider', at: AT_VOLUME, min: 0, max: 100, step: 1, suffix: '%',
            read: readVolume, write: writeVolume, enabled: (src) => hasControls(src) || has(src, AT_VOLUME_PKP),
          },
          { id: 'mute_unfocused', label: 'Mute when not focused', type: 'toggle', at: controls('audio', 'mute_unfocused'), def: '0' },
        ],
      },
    ],
  },
  {
    id: 'controls',
    label: 'Controls',
    groups: [
      {
        title: 'Mouse',
        rows: [
          { id: 'sensitivity', label: 'Mouse sensitivity', type: 'slider', at: AT_YAW, min: SENS_MIN, max: SENS_MAX, step: 0.1, read: readSensitivity, write: writeSensitivity },
          { id: 'invert_x', label: 'Invert horizontal', type: 'toggle', at: controls('mouse', 'invert_x'), def: '0' },
          { id: 'invert_y', label: 'Invert vertical', type: 'toggle', at: controls('mouse', 'invert_y'), def: '0' },
        ],
      },
      {
        title: 'Buttons',
        rows: [
          {
            id: 'prompt_style', label: 'Button icons', type: 'select', at: controls('prompts', 'style'), def: 'auto',
            aliases: { ps: 'playstation', ps4: 'playstation', ps5: 'playstation', dualsense: 'playstation', kbm: 'keyboard', keyboard_mouse: 'keyboard', mouse: 'keyboard' },
            options: [
              { value: 'auto', label: 'Automatic' }, { value: 'switch', label: 'Switch' }, { value: 'xbox', label: 'Xbox' },
              { value: 'playstation', label: 'PlayStation' }, { value: 'keyboard', label: 'Keyboard and mouse' },
            ],
          },
          { id: 'f1_bindings', label: 'Change bindings in game', type: 'keycap', key: 'F1' },
        ],
      },
    ],
  },
  {
    id: 'saves',
    label: 'Saves',
    groups: [
      {
        title: '',
        rows: [{
          id: 'save_dir', label: 'Save folder', type: 'path', at: pkp('fs', 'save_dir'), pick: 'Choose the save folder', open: 'save',
          placeholder: (env) => `${env.gameDir}/save`,
        }],
      },
      {
        title: 'Import a save',
        rows: [
          {
            expand: (env) => env.saveSources.map((s, i) => ({
              id: `source_${i}`, label: s.label, type: 'info', value: s.path, mono: true,
              actions: [{ act: 'import', arg: s.path, name: s.label, label: 'Import', icon: 'upload', disabled: env.gameBusy || env.importing }],
            })),
          },
          {
            id: 'import_folder', label: 'From a folder', type: 'info',
            actions: (env) => [{ act: 'import-folder', label: 'Choose a folder', icon: 'folder-open', disabled: env.gameBusy || env.importing }],
          },
          { id: 'import_result', type: 'note', when: (v, env) => !!env.importResult, note: (env) => env.importResult },
        ],
      },
    ],
  },
  {
    id: 'game',
    label: 'Game',
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'game_files', label: 'Game files', type: 'info', value: (env) => env.gameFiles, mono: true,
            actions: [{ act: 'change-files', label: 'Change', icon: 'folder-open' }],
          },
          {
            id: 'setup_again', label: 'Set up again', type: 'info',
            actions: (env) => [{ act: 'setup-again', label: 'Set up', icon: 'wrench', disabled: !env.canSetUp }],
          },
          { id: 'setup_log', label: 'Setup log', type: 'info', actions: [{ act: 'setup-log', label: 'Open', icon: 'file-text' }] },
        ],
      },
      {
        title: '',
        rows: [
          {
            // saved on leaving the field, not while typing: half a path would make the whole launcher think the game is gone
            id: 'game_dir', label: 'Install folder', type: 'path', at: config('game_dir'), pick: 'Choose the install folder', open: 'game',
            placeholder: (env) => env.defaultGameDir, commit: 'blur', reloads: true,
          },
          {
            id: 'romfs_mod', label: 'Mods', type: 'path', at: pkp('fs', 'romfs_mod'), pick: 'Choose the mods folder',
            placeholder: () => '',
          },
          {
            id: 'romfs_mod_note', type: 'note',
            note: () => ({ text: 'Choose a romfs-shaped folder of text or texture replacements; its files win over the update and base game.' }),
          },
          { id: 'minimize_on_play', label: 'Minimise while playing', type: 'toggle', at: config('minimize_on_play'), def: false },
        ],
      },
    ],
  },
  {
    id: 'about',
    label: 'About',
    groups: [
      {
        title: '',
        rows: [
          { id: 'version', label: 'Version', type: 'info', value: (env) => env.version, selectable: true },
          {
            id: 'updates', label: 'Updates', type: 'info', value: (env) => env.updateText,
            actions: (env) => [
              ...(env.canUpdate ? [{ act: 'update', label: 'Update', icon: 'download', primary: true }] : []),
              { act: 'check-updates', label: 'Check for updates', icon: 'refresh-cw', disabled: env.checking },
            ],
          },
          { id: 'licence', label: 'Licence', type: 'info', value: 'GPL-3.0-or-later' },
          { id: 'built_with', label: 'Built with', type: 'info', value: 'Tauri 2, Lucide (ISC), Inter (OFL)' },
          { id: 'repo', label: 'Source code', type: 'info', actions: [open(REPO_URL, 'GitHub')] },
          { id: 'bug', label: 'Report a bug', type: 'info', actions: [open(`${REPO_URL}/issues/new/choose`)] },
        ],
      },
      {
        title: '',
        rows: [{
          id: 'legal', type: 'note',
          note: () => ({ text: 'No game files are included. You need your own copy of the game, from a console you own. This project is not affiliated with or endorsed by Nintendo, The Pokemon Company or Koei Tecmo.' }),
        }],
      },
    ],
  },
];

export const FIELDS = Object.fromEntries(
  SECTIONS.flatMap((s) => s.groups.flatMap((g) => g.rows)).filter((r) => r.at).map((f) => [f.id, f]),
);

// ---- reading and writing ------------------------------------------------------------------------------------------------------------

export const makeSource = (dump, cfg) => ({ pkp: dump.pkp, controls: dump.controls, config: cfg, controls_exists: dump.controls_exists });

export const readField = (f, src) => (f.read ? f.read(src) : READ[f.type](f, src));

export const readAll = (src) => Object.fromEntries(Object.values(FIELDS).map((f) => [f.id, readField(f, src)]));

/**
 * What it takes to give field `f` this value: nothing when that is what it already reads as, and never a key that already holds the
 * text or a file that does not exist. Reading and writing back is therefore no edit at all, so a value the page cannot show survives.
 */
export function writeField(f, value, src) {
  if (String(readField(f, src)) === String(value)) return [];
  const edits = f.write ? f.write(value, src) : WRITE[f.type](f, value);
  if (f.quality) edits.push(...markCustom(f.quality, src));
  return edits.filter((e) => !(e.file === 'controls' && src.controls_exists === false) && String(rawOf(src, e)) !== String(e.value));
}

/** The source as it reads after the edits. */
export function applyEdits(src, edits) {
  const next = { ...src, pkp: { ...src.pkp }, controls: { ...src.controls }, config: { ...src.config } };
  for (const e of edits) next[e.file][keyOf(e)] = e.value;
  return next;
}

/** { ini: edits for write_settings, config: the keys for save_config, or null } */
export function splitEdits(edits) {
  const own = edits.filter((e) => e.file === 'config');
  return { ini: edits.filter((e) => e.file !== 'config'), config: own.length ? Object.fromEntries(own.map((e) => [e.key, e.value])) : null };
}

// ---- what a control shows ---------------------------------------------------------------------------------------------------------

/** The option a value selects, 'custom' when it is none of them. Never changes the value itself. */
export function shownChoice(f, value) {
  return f.options.some((o) => o.value === String(value)) ? String(value) : 'custom';
}

/** The options to offer. Custom is a real choice on a number control, and on a plain list only the name of a value it cannot show. */
export function optionsFor(f, value, env = {}) {
  const list = f.options.filter((o) => !o.offer || o.offer(env) || o.value === value).map(({ value: v, label }) => ({ value: v, label }));
  if (f.type === 'choice' || f.type === 'size') return [...list, { value: 'custom', label: 'Custom' }];
  return list.some((o) => o.value === value) ? list : [...list, { value: 'custom', label: 'Custom', disabled: true }];
}

/** What a number field starts from when Custom is picked: the value when it is a number, else the control's starting point. */
export function customStart(f, value) {
  return /^\d+$/.test(String(value)) ? String(value) : String(f.custom.start);
}

/** A whole number from a number field, kept in range; null when the text is not a number. */
export function fitNumber(text, min, max) {
  const n = Math.round(Number(String(text).trim() || NaN));
  return Number.isFinite(n) ? clamp(n, min, max) : null;
}

export const isVisible = (row, values, env) => (row.when ? row.when(values, env) : true);

/** The file is there. A field in controls.ini waits for the game to write it: the launcher never creates that file (settings.rs apply). */
export function isAvailable(f, src) {
  if (f.enabled) return f.enabled(src);
  return !atsOf(f).some((at) => at.file === 'controls') || src.controls_exists !== false;
}

export const isEnabled = (f, src, env) => isAvailable(f, src) && !f.lock?.(env);

/** The rows of a group with `expand` entries unfolded and hidden rows gone. */
export const rowsOf = (group, values, env) => group.rows.flatMap((r) => (r.expand ? r.expand(env) : [r])).filter((r) => isVisible(r, values, env));
