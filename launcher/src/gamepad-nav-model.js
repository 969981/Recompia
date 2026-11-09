// Controller navigation, the pure part: which way a pad is pushed, which buttons just went down, and which control is next in a direction.
// No DOM here, so it runs under plain node (scripts/test-gamepad-nav.mjs). The glue is gamepad-nav.js.

export const STICK_ON = 0.55;     // a stick this far over counts as a push (and a d-pad)
export const FIRST_REPEAT_MS = 420;
export const REPEAT_MS = 130;

// Standard-mapping button numbers (the browser's "standard" gamepad layout, which Steam Input and the XInput pads use).
export const BTN = { A: 0, B: 1, X: 2, Y: 3, LB: 4, RB: 5, BACK: 8, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };

/** What a pad is doing now: { dir: 'up'|'down'|'left'|'right'|null, down: Set of button numbers, scroll: -1..1 (right stick, + = down) }. */
export function readPad(pad) {
  const out = { dir: null, down: new Set(), scroll: 0 };
  if (!pad) return out;
  (pad.buttons || []).forEach((b, i) => { if (b && (b.pressed || b.value > 0.5)) out.down.add(i); });
  const ax = (i) => (pad.axes && Math.abs(pad.axes[i]) > 0.01 ? pad.axes[i] : 0);
  const lx = ax(0), ly = ax(1);
  if (out.down.has(BTN.UP)) out.dir = 'up';
  else if (out.down.has(BTN.DOWN)) out.dir = 'down';
  else if (out.down.has(BTN.LEFT)) out.dir = 'left';
  else if (out.down.has(BTN.RIGHT)) out.dir = 'right';
  else if (Math.max(Math.abs(lx), Math.abs(ly)) >= STICK_ON) out.dir = Math.abs(lx) > Math.abs(ly) ? (lx < 0 ? 'left' : 'right') : (ly < 0 ? 'up' : 'down');
  const ry = ax(3);
  out.scroll = Math.abs(ry) >= 0.2 ? ry : 0;
  return out;
}

/** The buttons that are down in `cur` and were not in `prev` (both from readPad). */
export function newlyDown(prev, cur) {
  const out = [];
  for (const b of cur.down) if (!prev || !prev.down.has(b)) out.push(b);
  return out;
}

/** Turns a held direction into steps: one at once, then after FIRST_REPEAT_MS every REPEAT_MS. state is kept by the caller: { dir, since, last }. */
export function repeatStep(state, dir, now) {
  if (!dir) { state.dir = null; return false; }
  if (state.dir !== dir) { state.dir = dir; state.since = now; state.last = now; return true; }
  if (now - state.since >= FIRST_REPEAT_MS && now - state.last >= REPEAT_MS) { state.last = now; return true; }
  return false;
}

const centre = (r) => ({ x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 });

/**
 * The control to go to from `from` in a direction. Rects are { left, top, right, bottom }. A candidate has to lie in that direction
 * (its centre past the edge of `from` on that side, or past its centre when they overlap); the nearest wins, with a sideways miss counting
 * double so a row of buttons is walked along before jumping to another row. Returns an index into `rects` or -1.
 */
export function pickNext(from, rects, dir) {
  const f = centre(from);
  let best = -1, bestScore = Infinity;
  rects.forEach((r, i) => {
    if (r === from) return;
    const c = centre(r);
    let along, across;
    if (dir === 'right') { along = r.left >= from.right - 1 ? r.left - from.right : c.x - f.x; across = Math.abs(c.y - f.y); if (c.x <= f.x) return; }
    else if (dir === 'left') { along = r.right <= from.left + 1 ? from.left - r.right : f.x - c.x; across = Math.abs(c.y - f.y); if (c.x >= f.x) return; }
    else if (dir === 'down') { along = r.top >= from.bottom - 1 ? r.top - from.bottom : c.y - f.y; across = Math.abs(c.x - f.x); if (c.y <= f.y) return; }
    else if (dir === 'up') { along = r.bottom <= from.top + 1 ? from.top - r.bottom : f.y - c.y; across = Math.abs(c.x - f.x); if (c.y >= f.y) return; }
    else return;
    const score = Math.max(0, along) + 2 * across;
    if (score < bestScore) { bestScore = score; best = i; }
  });
  return best;
}

/** The next index in a list of n, wrapping: step is +1 or -1. */
export function cycle(i, n, step) {
  if (n <= 0) return -1;
  return ((i < 0 ? (step > 0 ? -1 : 0) : i) + step + n) % n;
}

// ---- arrow keys (a handheld whose d-pad reaches the launcher as the arrow keys, Steam's default layout for a non-Steam shortcut) ----

const ARROWS = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };

/** The direction of an arrow key press, or null: other keys, and any arrow with Ctrl, Alt, Meta or Shift held, are not navigation. */
export function keyDir(e) {
  if (e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return null;
  return ARROWS[e.key] || null;
}

/**
 * Who handles an arrow key: 'nav' (move the focus) or 'native' (leave it to the browser). `kind` is what has the focus: 'none' (no control),
 * 'text' (a field with a caret), 'select', 'range', 'radio' or 'other'. A text field keeps all four arrows; a drop-down keeps up and down; a slider
 * and a radio group keep left and right. With nothing focused, up and down scroll the page when it can scroll, left and right take the first control.
 */
export function arrowRoute(kind, dir, canScroll = false) {
  const horizontal = dir === 'left' || dir === 'right';
  switch (kind) {
    case 'text': return 'native';
    case 'select': return horizontal ? 'nav' : 'native';
    case 'range':
    case 'radio': return horizontal ? 'native' : 'nav';
    case 'none': return !horizontal && canScroll ? 'native' : 'nav';
    default: return 'nav';
  }
}
