// Puts the section on screen. A redraw must never pull a control from under the player: draw() waits while a text field has focus or
// the pointer is down, and otherwise replaces only the rows whose markup changed (and keeps focus on the same control).
import { sectionHtml } from './rows.js';
import { segmented } from '../../ui.js';
import { sectionsFor } from '../../settings-schema.js';
import { channelOf, env, rememberSection } from './state.js';

const rowsIn = (node) => [...node.querySelectorAll('[data-row]')];
const shape = (node) => [...rowsIn(node).map((r) => r.dataset.row), node.querySelector('[data-banner]')?.dataset.banner].join('|');
const rowSig = (node) => node.outerHTML.replace(/row-\d+/g, '');

export function createDrawer(el, st) {
  const sig = new Map();   // row id -> the markup it was drawn from
  let pointerDown = false;
  let redraw = false;

  const body = () => el.querySelector('#set-body');

  function focusKey() {
    const a = document.activeElement;
    if (!a || !el.contains(a) || a.closest('.set-pills')) return null;
    if (a.id) return `#${CSS.escape(a.id)}`;
    if (a.name) return `input[name="${CSS.escape(a.name)}"][value="${CSS.escape(a.value)}"]`;
    if (a.dataset.act) return `[data-act="${a.dataset.act}"]${a.dataset.arg ? `[data-arg="${CSS.escape(a.dataset.arg)}"]` : ''}`;
    return null;
  }

  const typing = () => document.activeElement?.matches?.('input.input') && el.contains(document.activeElement);

  /** Joining or leaving Testing adds or removes the Testing tab; a player standing on it when it goes lands on Display. */
  function syncPills() {
    const ids = sectionsFor(channelOf(st)).map((x) => x.id).join();
    if (!ids.split(',').includes(st.section)) { st.section = 'display'; rememberSection('display'); }
    const pills = el.querySelector('.set-pills');
    if (pills.dataset.ids === ids) return;   // only a changed list is redrawn: a redraw would take the focus off the pill being used
    pills.dataset.ids = ids;
    pills.innerHTML = segmented('section', sectionsFor(channelOf(st)).map((x) => ({ value: x.id, label: x.label })), st.section, { label: 'Settings section' });
  }

  /** Brings the section up to date. force: everything, now, even under the cursor (a failed write, a new section). */
  function draw(force = false) {
    if (!force && (pointerDown || typing())) {
      redraw = true;
      return;
    }
    redraw = false;
    syncPills();
    const host = body();
    const next = document.createElement('div');
    next.innerHTML = sectionHtml(st, env(st));
    const key = focusKey();
    if (force || shape(host) !== shape(next)) {
      host.replaceChildren(...next.childNodes);
      sig.clear();
      rowsIn(host).forEach((r) => sig.set(r.dataset.row, rowSig(r)));
    } else {
      const fresh = rowsIn(next);
      rowsIn(host).forEach((old, i) => {
        const was = sig.get(old.dataset.row);
        const now = rowSig(fresh[i]);
        if (was === now) return;
        sig.set(old.dataset.row, now);
        old.replaceWith(fresh[i]);
      });
    }
    if (key && !el.contains(document.activeElement)) host.querySelector(key)?.focus();
  }

  // a pointer press on a row must end on that same row: replacing a button under it loses the click
  const release = () => {
    pointerDown = false;
    setTimeout(() => redraw && draw());
  };

  el.addEventListener('pointerdown', () => { pointerDown = true; });
  window.addEventListener('pointerup', release);
  window.addEventListener('pointercancel', release);
  el.addEventListener('focusout', () => setTimeout(() => redraw && draw()));

  return {
    draw,
    /** Removes the window listeners (the page's own go with its element). */
    dispose() {
      window.removeEventListener('pointerup', release);
      window.removeEventListener('pointercancel', release);
    },
  };
}
