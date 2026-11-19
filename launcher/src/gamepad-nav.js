// Controller navigation for the launcher (Steam Deck, ROG Ally and other handhelds, or any pad): the d-pad or left stick moves between
// controls, A presses the focused one, B goes back (closes a question, or returns Home), LB and RB switch pages, LT and RT switch games,
// Start presses the shown Home's Play, the right stick scrolls. While a pad is in use a bar along the bottom shows these (main.js).
// The pad is read from the webview's Gamepad API; where it has none (WebKitGTK on Linux and the Steam Deck), from the launcher's own reader
// (src-tauri/src/pad.rs), whose events store.js passes to feedNativePad.
// A slider moves with left and right, a drop-down with up and down. It works beside touch and the mouse: the focus ring only shows while a
// pad is being used. The logic that needs no page is in gamepad-nav-model.js.
//
// A frame of the launcher's own (same origin) marked data-pad-frame, such as the Save Editor's, is navigated like the page: moving onto it
// goes to its first control, the pad then moves among its controls, past its edge on to the launcher's, and B steps out of it to the
// page's entry in the sidebar (B never leaves a page from inside a frame, which may hold unsaved work).
import { BTN, readPad, newlyDown, repeatStep, pickNext, cycle, nativePad } from './gamepad-nav-model.js';

const FOCUSABLE = 'button, a[href], input, select, textarea, [tabindex], iframe[data-pad-frame]';
let started = false;
let native = null;   // the last pad-state from pad.rs, as a Gamepad-like object (null: none)
let hooks = { switchGame: () => {}, play: () => {} };
let padActive = false;   // a pad has been used since the mouse or a finger last was
const watched = new WeakSet();   // the frame documents whose mouse and touch turn the pad's focus ring off

const isPadFrame = (el) => !!el && el.tagName === 'IFRAME' && el.hasAttribute('data-pad-frame');

/** A pad frame's document, when it is loaded and of the launcher's own origin. */
function frameDoc(frame) {
  try {
    const doc = frame.contentDocument;
    return doc?.body ? doc : null;
  } catch {
    return null;
  }
}

/** Where the pad works now: inside a pad frame while the focus is in it, else the launcher's page. */
function root() {
  const frame = isPadFrame(document.activeElement) ? document.activeElement : null;
  const doc = frame && frameDoc(frame);
  return doc ? { doc, frame } : { doc: document, frame: null };
}

function usable(el, scope) {
  if (!scope.contains(el) || el.disabled || el.getAttribute('tabindex') === '-1' || el.type === 'hidden') return false;
  if (el.closest('[hidden], [inert]') || el.closest('#winbtns')) return false;   // the window buttons are for the mouse and touch
  const s = el.ownerDocument.defaultView.getComputedStyle(el);
  if (s.visibility === 'hidden' || s.display === 'none') return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0;
}

function candidates(doc = document) {
  const scope = doc.querySelector('dialog[open]') || doc.body;   // a question owns the pad while it is open
  return { scope, list: [...scope.querySelectorAll(FOCUSABLE)].filter((el) => usable(el, scope)) };
}

function rectOf(el) {
  const r = el.getBoundingClientRect();
  return { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
}

/** `from` and `dir`: where the pad came from (a launcher control's rect) and which way it moved, for a pad frame to enter near it. */
function focusEl(el, from = null, dir = null) {
  if (isPadFrame(el)) return enter(el, from, dir);
  el.focus({ preventScroll: true });
  el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

/**
 * Into a pad frame (the focus moves into it with its control): coming from a launcher control, the frame's nearest control that way
 * (its rects measured in this window); otherwise its first control.
 */
function enter(frame, from = null, dir = null) {
  const doc = frameDoc(frame);
  if (!doc) return;
  watch(doc);
  setPadClass(doc, padActive);
  const { list } = candidates(doc);
  if (!list.length) return;
  const at = frame.getBoundingClientRect();
  const outer = (el) => {
    const r = rectOf(el);
    return { left: r.left + at.left, top: r.top + at.top, right: r.right + at.left, bottom: r.bottom + at.top };
  };
  const i = from && dir ? pickNext(from, list.map(outer), dir) : -1;
  frame.focus({ preventScroll: true });
  frame.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  focusEl(list[i >= 0 ? i : 0]);
}

/** Out of a pad frame towards `dir`: the launcher's nearest control that way, or the current page's entry in the sidebar. */
function leave(frame, dir) {
  const { list } = candidates(document);
  const others = list.filter((el) => el !== frame);
  const i = dir ? pickNext(rectOf(frame), others.map(rectOf), dir) : -1;
  const to = i >= 0 ? others[i] : document.querySelector('#nav [aria-current]');
  if (to) focusEl(to);
}

function move(dir) {
  const { doc, frame } = root();
  const { list } = candidates(doc);
  const cur = doc.activeElement;
  if (!cur || !list.includes(cur)) {   // nothing focused yet: the page's main action or its pad frame, else the first control
    if (frame) return enter(frame);
    if (!list.length) return;
    focusEl(document.querySelector('dialog[open] .btn.primary, #view .btn.primary, #view iframe[data-pad-frame]') || list[0]);
    return;
  }
  // a slider uses left and right, a drop-down up and down, for their value
  if (cur.matches('input[type="range"]') && (dir === 'left' || dir === 'right')) return nudgeRange(cur, dir === 'right' ? 1 : -1);
  if (cur.matches('select') && (dir === 'up' || dir === 'down')) return nudgeSelect(cur, dir === 'down' ? 1 : -1);
  const i = pickNext(rectOf(cur), list.map(rectOf), dir);
  if (i >= 0) focusEl(list[i], frame ? null : rectOf(cur), dir);
  else if (frame) leave(frame, dir);   // past the frame's edge: on to the launcher's controls that way
}

function fire(el, type) {
  el.dispatchEvent(new Event(type, { bubbles: true }));
}

function nudgeRange(el, step) {
  const lo = Number(el.min || 0), hi = Number(el.max || 100);
  const unit = Number(el.step) > 0 ? Number(el.step) : Math.max(1, (hi - lo) / 20);
  el.value = String(Math.min(hi, Math.max(lo, Number(el.value) + step * unit)));
  fire(el, 'input');
  fire(el, 'change');
}

function nudgeSelect(el, step) {
  const n = Math.min(el.options.length - 1, Math.max(0, el.selectedIndex + step));
  if (n === el.selectedIndex) return;
  el.selectedIndex = n;
  fire(el, 'change');
}

function press() {
  const { doc } = root();
  const el = doc.activeElement;
  if (!el || !candidates(doc).list.includes(el)) return move(null);   // no control focused (the page area itself): as a first move
  if (el.matches('select')) { if (el.showPicker) { try { el.showPicker(); return; } catch { /* not allowed now: fall through */ } } }
  el.click();
}

function back() {
  const { doc, frame } = root();
  const dlg = doc.querySelector('dialog[open]');
  if (dlg) {
    const cancel = dlg.querySelector('[data-answer=""], [value="cancel"], .btn.quiet');
    if (cancel) cancel.click();
    else dlg.close('');
    return;
  }
  if (frame) return leave(frame, null);   // out of the frame first; B again then goes Home
  const home = document.querySelector('#nav [data-home]');   // the shown game's Home (games.js `home`)
  if (home && !home.hasAttribute('aria-current')) home.click();   // from any other page, B goes Home
}

function switchPage(step) {
  if (root().doc.querySelector('dialog[open]')) return;
  const items = [...document.querySelectorAll('#nav [data-page]')];
  const at = items.findIndex((b) => b.hasAttribute('aria-current'));
  const to = cycle(at, items.length, step);
  if (to >= 0 && to !== at) items[to].click();
}

/** The nearest box around `el` that scrolls up and down. */
function scroller(el) {
  for (let e = el; e && e.nodeType === 1; e = e.parentElement) {
    const s = e.ownerDocument.defaultView.getComputedStyle(e);
    if (/(auto|scroll)/.test(s.overflowY) && e.scrollHeight > e.clientHeight) return e;
  }
  return null;
}

function scroll(amount) {
  const { doc, frame } = root();
  const target = doc.querySelector('dialog[open]')   // a long question scrolls itself
    || (frame ? scroller(doc.activeElement) || doc.scrollingElement : document.getElementById('view'));
  target?.scrollBy({ top: amount * 22 });
}

function setPadClass(doc, on) {
  doc.body?.classList.toggle('pad-nav', on);
}

function setPadActive(on) {
  if (padActive === on) return;
  padActive = on;
  setPadClass(document, on);
  for (const frame of document.querySelectorAll('iframe[data-pad-frame]')) {
    const doc = frameDoc(frame);
    if (doc) setPadClass(doc, on);
  }
}

/** The mouse and touch inside a frame turn the pad's focus ring off there too (their events do not reach this window). */
function watch(doc) {
  if (watched.has(doc)) return;
  watched.add(doc);
  doc.addEventListener('pointerdown', () => setPadActive(false), true);
  doc.addEventListener('mousemove', (e) => { if (e.movementX || e.movementY) setPadActive(false); }, true);
}

/** A pad-state event from the launcher's own pad reader (pad.rs). */
export function feedNativePad(state) {
  native = nativePad(state);
}

/** The pad to read this frame: the webview's first connected pad, else the launcher's own reader's. */
function currentPad() {
  const pads = navigator.getGamepads ? [...navigator.getGamepads()] : [];
  return pads.find((p) => p && p.connected) || native;
}

const GLYPH = { A: 'A', B: 'B', X: 'X', Y: 'Y', LB: 'LB', RB: 'RB', LT: 'LT', RT: 'RT', Start: '≡' };

/** A pad button's glyph, as the hints and the tab row show it. `cls`: 'round a' and the like (components.css .pad-glyph). */
export const padGlyph = (button, cls = '') => `<span class="pad-glyph ${cls}" aria-hidden="true">${GLYPH[button] ?? button}</span>`;

/** hooks: { switchGame(step), play() }: what LT/RT and Start do (main.js). */
export function startGamepadNav(opts = {}) {
  hooks = { ...hooks, ...opts };
  if (started) return;
  started = true;
  let prev = null;
  const rep = { dir: null, since: 0, last: 0 };

  watch(document);

  const tick = (now) => {
    requestAnimationFrame(tick);
    if (!document.hasFocus()) { prev = null; return; }   // true too while the focus is inside a frame of this window
    const pad = currentPad();
    if (!pad) { prev = null; return; }
    const cur = readPad(pad);
    const fresh = prev ? newlyDown(prev, cur) : [];   // the first frame only records what is held (a button held while the launcher opened)
    prev = cur;
    const used = cur.dir || cur.scroll || fresh.length;
    if (used) setPadActive(true);
    if (!padActive) return;
    if (repeatStep(rep, cur.dir, now)) move(cur.dir);
    for (const b of fresh) {
      if (b === BTN.A) press();
      else if (b === BTN.B) back();
      else if (b === BTN.LB) switchPage(-1);
      else if (b === BTN.RB) switchPage(1);
      else if (b === BTN.LT && !root().doc.querySelector('dialog[open]')) hooks.switchGame(-1);
      else if (b === BTN.RT && !root().doc.querySelector('dialog[open]')) hooks.switchGame(1);
      else if (b === BTN.START && !root().doc.querySelector('dialog[open]')) hooks.play();
    }
    if (cur.scroll) scroll(cur.scroll);
  };
  requestAnimationFrame(tick);
}
