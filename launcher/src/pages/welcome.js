// The welcome screen: the whole window, before the first Home of a launcher start, while an account portal is set, this device is
// signed out of it (or its sign-in expired) and the player has not pressed Skip (cloud-signin.js welcomeOwed; main.js shows it and
// hides the sidebar). Recompia's logo on the floating island, and a card that signs in right here (cloud-signin-model.js welcome()).
// Skip (or Esc) goes on to Home at once and is remembered (launcher.json signin_skipped); signed in, the card says so for a moment and
// goes on by itself. Play is never behind it: Skip is on every step.
import './welcome.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { esc, icon } from '../ui.js';
import { brandUrl } from '../branding.js';
import { follow, hero, rememberSkip } from '../cloud-signin.js';
import { countdown, welcome } from '../cloud-signin-model.js';

const DONE_MS = 1800;   // how long "Signed in as …" stays before Home

/** ctx.next, ctx.nextOpts: the page to go on to (the game's Home); ctx.release: called once the screen is gone (main.js). */
export function render(host, ctx) {
  const next = ctx.next || ctx.game.home;
  host.innerHTML = `<div class="welcome-screen">
      <div class="welcome-hero">${hero('welcome')}</div>
      <section class="welcome-card signin glass strong" aria-labelledby="welcome-title" aria-live="polite">
        <h1 id="welcome-title">${esc(t('Online account'))}</h1>
        <div class="welcome-main"></div>
        <div class="welcome-actions"></div>
      </section>
    </div>`;
  const root = host.firstElementChild;
  const main = root.querySelector('.welcome-main');
  const actions = root.querySelector('.welcome-actions');
  let st = null;
  let drawn = '';
  let deadline = 0;
  let done = 0;
  let gone = false;

  const leave = (skip) => {
    if (gone) return;
    gone = true;
    watch.stop();
    if (skip) {
      rememberSkip(true);
      if (st?.state === 'waiting') api.cloudSigninCancel().catch(() => {});
    }
    ctx.go(next, ctx.nextOpts);
  };
  const draw = () => {
    // the countdown ticks here; everything else is drawn again only when it changed (the QR code stays put)
    const key = JSON.stringify({ ...st, phase: st?.phase && { ...st.phase, expires_in: 0 } });
    if (key !== drawn) {
      const v = welcome(st, icon, brandUrl('mark'));
      if (v.step === 'off') return leave(false);
      root.dataset.step = v.step;
      main.innerHTML = v.body;
      actions.innerHTML = v.actions;
      drawn = key;
      deadline = Date.now() + (st?.phase?.expires_in ?? 0) * 1000;
      if (v.step === 'done' && !done) done = setTimeout(() => leave(false), DONE_MS);
      if (v.step === 'choose') root.querySelector('.welcome-card .btn.primary')?.focus({ preventScroll: true });   // Enter signs in the usual way
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
    if (act === 'browser' || act === 'phone') return watch.run(() => api.cloudSigninBegin({ method: act }));
    if (act === 'back') return watch.run(() => api.cloudSigninCancel());
    if (act === 'skip') leave(true);
  });
  const onKey = (e) => {
    if (e.key === 'Escape' && !document.querySelector('dialog[open]') && st?.state !== 'signed_in') leave(true);
  };
  document.addEventListener('keydown', onKey);
  watch.poll();

  return () => {
    gone = true;
    watch.stop();
    clearTimeout(done);
    document.removeEventListener('keydown', onKey);
    ctx.release?.();
  };
}
