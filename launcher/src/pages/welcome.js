// The sign-in screen: a full-window screen (main.js: the page tabs go, the rail of games stays), Recompia's logo on the floating island
// and a card that signs in right here (cloud-signin-model.js welcome()). The one place for the online account: the launcher starts on it before the first Home
// of a launcher start while an account portal is set, this device is signed out of it (or its sign-in expired) and the player has not
// pressed Skip (cloud-signin.js welcomeOwed), and Home's account chip opens it (ctx.from === 'chip') at any time, signed in or not.
// At start the way out is Skip (or Esc): on to Home at once, remembered (launcher.json signin_skipped); signed in, the card says so
// for a moment and goes on by itself. From the chip the way out is Back (or Esc): Home again, the skip flag untouched; signed in, the
// card shows the account, Sign out and the account page. Leaving while a sign-in waits stops it, by the way out or any other (the rail,
// the pad's LT and RT): another game or page is "not now", not remembered. Play is never behind it: the way out is on every step, and a
// controller's B presses it (data-pad-back), as Esc does.
import './welcome.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { errText, esc, icon, toast } from '../ui.js';
import { brandUrl } from '../branding.js';
import { follow, hero, rememberSkip } from '../cloud-signin.js';
import { countdown, welcome } from '../cloud-signin-model.js';

const DONE_MS = 1800;   // how long "Signed in as …" stays before Home (at start)

/** ctx.next, ctx.nextOpts: the page to go on to (the game's Home); ctx.release: called once the screen is gone (main.js, at start);
 *  ctx.from: 'chip' when Home's account chip opened it. */
export function render(host, ctx) {
  const next = ctx.next || ctx.game.home;
  const chip = ctx.from === 'chip';
  host.innerHTML = `<div class="welcome-screen"><div class="welcome-layout">
      <div class="welcome-hero">${hero('welcome')}</div>
      <section class="welcome-card signin glass strong" aria-labelledby="welcome-title" aria-live="polite">
        <h1 id="welcome-title">${esc(t('Online account'))}</h1>
        <div class="welcome-main"></div>
        <div class="welcome-actions"></div>
      </section>
    </div></div>`;
  const root = host.firstElementChild;
  const main = root.querySelector('.welcome-main');
  const actions = root.querySelector('.welcome-actions');
  let st = null;
  let note = '';
  let drawn = '';
  let deadline = 0;
  let done = 0;
  let gone = false;

  /** skip: Skip at start (remembered); otherwise Back from the chip, or on by itself (signed in at start, no portal). */
  const leave = (skip) => {
    if (gone) return;
    gone = true;
    watch.stop();   // stops a sign-in that waits
    if (skip) rememberSkip(true);
    ctx.go(next, ctx.nextOpts);
  };
  const draw = () => {
    // the countdown ticks here; everything else is drawn again only when it changed (the QR code stays put)
    const key = JSON.stringify({ ...st, phase: st?.phase && { ...st.phase, expires_in: 0 }, note });
    if (key !== drawn) {
      const v = welcome(st, icon, brandUrl('mark'), { chip, note });
      if (v.step === 'off') return leave(false);
      root.dataset.step = v.step;
      main.innerHTML = v.body;
      actions.innerHTML = v.actions;
      drawn = key;
      deadline = Date.now() + (st?.phase?.expires_in ?? 0) * 1000;
      if (v.step === 'done' && !done) done = setTimeout(() => leave(false), DONE_MS);
      // Enter signs in the usual way; signed in, Enter goes Back
      if (v.step === 'choose' || v.step === 'account') root.querySelector('.welcome-card .btn.primary, .welcome-card [data-signin="leave"]')?.focus({ preventScroll: true });
    }
    const left = root.querySelector('#signin-left');
    if (left) left.textContent = countdown((deadline - Date.now()) / 1000);
  };
  const watch = follow((next) => {
    if (next?.state === 'signed_in' && st?.state !== 'signed_in') rememberSkip(false);
    st = next;
    draw();
  });

  root.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-signin]');
    if (!b || b.disabled || gone) return;
    const act = b.dataset.signin;
    if (act === 'browser' || act === 'phone') {
      note = '';
      return watch.run(() => api.cloudSigninBegin({ method: act }));
    }
    if (act === 'back') return watch.run(() => api.cloudSigninCancel());
    if (act === 'sign-out') {
      note = 'signed-out';
      return watch.run(() => api.cloudSignOut().then((s) => { rememberSkip(false); return s; }));
    }
    if (act === 'account') return api.cloudAccountPage().catch((err) => toast(errText(err), 'error'));
    if (act === 'skip' || act === 'leave') leave(act === 'skip');
  });
  // Esc: the way out, as its button (not while a dialog is open, nor in the moment "Signed in as …" shows at start)
  const onKey = (e) => {
    if (e.key !== 'Escape' || document.querySelector('dialog[open]')) return;
    if (chip) leave(false);
    else if (st?.state !== 'signed_in') leave(true);
  };
  document.addEventListener('keydown', onKey);
  watch.poll();

  return () => {   // gone by the rail or the pad too: the same stop
    gone = true;
    watch.stop();
    clearTimeout(done);
    document.removeEventListener('keydown', onKey);
    ctx.release?.();
  };
}
