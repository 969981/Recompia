// The markup of the Settings rows: one function per row type, from the schema's rows and the current values. Pure: it reads st and the
// env it is given and returns strings; draw.js decides what reaches the DOM.
import { esc, field, group, icon, row, segmented, select, slider, toggle } from '../../ui.js';
import { store } from '../../store.js';
import { AUTO, LANGUAGES, savedLanguage, t } from '../../i18n.js';
import { fromBackend } from '../../backend-text.js';
import { SECTIONS, customStart, isAvailable, isEnabled, optionsFor, readAll, rowsOf, shownChoice } from '../../settings-schema.js';

const numberField = (id, value, min, max, label, disabled) =>
  field(id, value, '', { type: 'number', label, disabled }).replace('class="input', `min="${min}" max="${max}" step="1" class="input set-num`);

const clockInput = (minutes) => {
  const picked = new Date(Date.now() + minutes * 60000);
  return new Date(picked.getTime() - picked.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
};

const clockReadout = (minutes) => {
  if (!minutes) return t('Matches this PC');
  const left = Math.abs(minutes);
  const days = Math.floor(left / 1440), hours = Math.floor(left % 1440 / 60), mins = left % 60;
  const parts = [...(days ? [`${days} d`] : []), ...(hours ? [`${hours} h`] : []), ...(mins ? [`${mins} min`] : [])];
  return t(minutes > 0 ? '{time} ahead of this PC' : '{time} behind this PC', { time: parts.join(' ') });
};

const CONTROL = {
  segmented: (st, f, value, e, off) => segmented(f.id, optionsFor(f, value, e), value, { disabled: off }),
  select: (st, f, value, e, off) => select(f.id, optionsFor(f, value, e), shownChoice(f, value), { disabled: off }),
  toggle: (st, f, value, e, off) => toggle(f.id, value, { disabled: off }),
  slider: (st, f, value, e, off) => slider(f.id, f.min, f.max, value, { step: f.step, suffix: f.suffix ?? '', disabled: off }),
  choice: (st, f, value, e, off) => {
    const open = st.customOpen.has(f.id) || shownChoice(f, value) === 'custom';
    const c = f.custom;
    return `${select(f.id, optionsFor(f, value, e), open ? 'custom' : value, { disabled: off })}
      ${open ? `${numberField(`${f.id}--custom`, customStart(f, value), c.min, c.max, c.label, off)}<span class="set-unit">${esc(c.suffix)}</span>` : ''}`;
  },
  size: (st, f, value, e, off) => {
    const open = st.customOpen.has(f.id) || shownChoice(f, value) === 'custom';
    const [w, h] = String(value).split('x');
    return `${select(f.id, optionsFor(f, value, e), open ? 'custom' : value, { disabled: off })}
      ${open ? `${numberField(`${f.id}--w`, w, f.min, f.max, t('Window width'), off)}<span class="set-unit">x</span>${numberField(`${f.id}--h`, h, f.min, f.max, t('Window height'), off)}` : ''}`;
  },
  path: (st, f, value, e, off) => {
    const holder = f.placeholder(e);
    return `${field(f.id, value, holder, { mono: true, disabled: off }).replace('class="input', `title="${esc(value || holder)}" class="input set-path`)}
      <button class="btn" data-act="browse" data-arg="${f.id}">${esc(t('Browse'))}</button>
      ${f.open ? `<button class="btn" data-act="open" data-arg="${f.open}">${icon('folder-open')}${esc(t('Open'))}</button>` : ''}`;
  },
  text: (st, f, value, e, off) => field(f.id, value, f.placeholder?.(e) ?? '', { disabled: off }).replace('class="input', 'maxlength="32" class="input'),
  clock: (st, f, value, e, off) => `${field(f.id, clockInput(value), '', { type: 'datetime-local', disabled: off }).replace('class="input', 'step="60" class="input')}
    <button class="btn" data-act="clock-real" data-arg="${f.id}" ${off ? 'disabled' : ''}>${icon('rotate-ccw')}${esc(t('Real time'))}</button>
    <span class="set-value">${esc(clockReadout(value))}</span>`,
};

const actionsOf = (r, e) => (typeof r.actions === 'function' ? r.actions(e) : r.actions ?? []);

const actionButton = (a) => `<button class="btn ${a.primary ? 'primary' : ''}" data-act="${a.act}" ${a.arg ? `data-arg="${esc(a.arg)}"` : ''} ${a.name ? `data-name="${esc(a.name)}"` : ''} ${a.disabled ? 'disabled' : ''}>${icon(a.icon)}${esc(a.label)}</button>`;

function infoControl(r, e) {
  const value = typeof r.value === 'function' ? r.value(e) : r.value;
  const text = value ? `<span class="set-value clip ${r.mono ? 'path' : ''} ${r.selectable ? 'selectable' : ''}" title="${esc(value)}">${esc(value)}</span>` : '';
  return text + actionsOf(r, e).map(actionButton).join('');
}

const rowHtml = (r, cls, label, control) => row(label, control).replace('class="row-item"', `class="row-item ${cls}" data-row="${r.id}"`);

function noteHtml(r, e) {
  const n = r.note(e);
  return `<div class="row-item set-note ${n.kind ?? ''}" data-row="${r.id}">
    ${n.kind ? icon(n.kind === 'danger' ? 'circle-x' : 'circle-check') : ''}
    <p>${esc(n.text)}${n.path ? ` <span class="path selectable">${esc(n.path)}</span>` : ''}</p>
  </div>`;
}

/** Automatic first, then every language by its own name (so a player who cannot read the current one still finds theirs). */
const languageSelect = () => select('language', [[AUTO, t('Automatic (system language)')], ...LANGUAGES.map((l) => [l.code, l.name])], savedLanguage(), { label: t('Language') });

/** The Button icons preview: each Switch button with its icon; a dashed box where the game keeps its own icon. */
function glyphsControl(r, values, e) {
  const g = r.glyphs(values);
  if (g.text) return `<span class="set-value">${esc(g.text)}</span>`;
  if (!e.promptIcons) return `<span class="set-value">${esc(t('No icon pack in the game folder'))}</span>`;
  return `<div class="set-glyphs">${g.items.map(({ button, icon }) => {
    const url = icon ? e.promptIcons(icon) : '';
    const tip = url ? t('Switch {button} shows this icon', { button }) : t("Switch {button} keeps the game's own icon", { button });
    return `<figure class="set-glyph" title="${esc(tip)}">${url ? `<img src="${url}" alt="">` : '<span class="set-glyph-none"></span>'}<figcaption>${esc(button)}</figcaption></figure>`;
  }).join('')}</div>`;
}

function rowOf(st, r, values, e) {
  if (r.type === 'note') return noteHtml(r, e);
  if (r.type === 'glyphs') return rowHtml(r, 'set-glyphrow', r.label, glyphsControl(r, values, e));
  if (r.type === 'language') return rowHtml(r, '', r.label, languageSelect());
  if (r.type === 'keycap') return rowHtml(r, '', r.label, `<kbd class="set-key">${esc(r.key)}</kbd>`);
  if (r.type === 'info') return rowHtml(r, 'set-info', r.label, infoControl(r, e));
  const off = !isEnabled(r, st.src, e);
  return rowHtml(r, r.type === 'path' ? 'set-pathrow' : '', r.label, CONTROL[r.type](st, r, values[r.id], e, off));
}

function bannerHtml(st, sec, values, e) {
  const waiting = sec.groups.some((g) => rowsOf(g, values, e).some((r) => r.at && !isAvailable(r, st.src)));
  const noExe = sec.id === 'game' && ['no_exe', 'no_runtime'].includes(store.state.app?.readiness?.kind);
  const text = waiting ? t('Available after the first start of the game')
    : noExe ? fromBackend(store.state.app.readiness.detail)
    : e.gameBusy && sec.id !== 'about' ? t('Changes apply next time the game starts')
    : '';
  return text ? `<div class="banner ${noExe && !waiting ? 'warn' : 'info'}" data-banner="${esc(text)}">${icon(noExe && !waiting ? 'triangle-alert' : 'info')}<div>${esc(text)}</div></div>` : '';
}

/** The whole current section: its banner and its groups of rows. */
export function sectionHtml(st, e) {
  const sec = SECTIONS.find((s) => s.id === st.section);
  const values = readAll(st.src);
  const groups = sec.groups.map((g) => {
    const rows = rowsOf(g, values, e);
    return rows.length ? group(g.title, rows.map((r) => rowOf(st, r, values, e))) : '';
  });
  return bannerHtml(st, sec, values, e) + groups.join('');
}
