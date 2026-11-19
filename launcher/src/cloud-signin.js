// The online account: the Online account dialog (opened from Home's account chip), the floating island with the logo that the dialog
// and the welcome screen (pages/welcome.js) show, and what both share to follow a sign-in. What they say is cloud-signin-model.js; the
// work is cloud_signin.rs (a sign-in runs there on a thread of its own, and the dialog or the welcome screen reads its state about once
// a second while it waits). Closing the dialog while it waits ("Not now", Esc) stops the sign-in: nothing is written.
import './cloud-signin.css';
import { api } from './api.js';
import { errText, icon, toast } from './ui.js';
import { t } from './i18n.js';
import { brandImg, brandUrl } from './branding.js';
import { countdown, heroSize, view, welcomeAtStart } from './cloud-signin-model.js';

/** The sign-in state, or null when the launcher cannot say (the chip is left out). */
export const readAccount = () => api.cloudSigninState().catch((e) => { console.warn('cloud_signin_state', e); return null; });

/** Skip, remembered per install (launcher.json signin_skipped), and forgotten at the next sign-in or sign-out. */
export async function rememberSkip(on) {
  try {
    const cfg = await api.getConfig();
    if (!!cfg.signin_skipped !== on) await api.saveConfig({ config: { ...cfg, signin_skipped: on } });
  } catch (e) {
    console.warn('signin_skipped', e);
  }
}

/**
 * Whether the launcher starts on the welcome screen before Home (welcomeAtStart; main.js asks once per launcher start). A device
 * signed in some other way (the gateway's own signin) forgets an old Skip here.
 */
export async function welcomeOwed() {
  const st = await readAccount();
  if (!st) return false;
  let skipped;
  try {
    skipped = !!(await api.getConfig()).signin_skipped;
  } catch (e) {
    console.warn('get_config', e);
    return false;
  }
  if (st.state === 'signed_in' && skipped) rememberSkip(false);
  return welcomeAtStart(st, skipped);
}

/**
 * Recompia's logo standing on the floating island (art/floating-island.svg), the two drifting together (cloud-signin.css). Recompia's
 * logo in Co-op too: the account is the same. `size`: a class for how big (the dialog: '', small, large; the welcome screen: welcome).
 */
export const hero = (size = '') =>
  `<div class="float-hero ${size}"><div class="float-drift">${brandImg('logo', 'class="float-logo" alt="Recompia"', 'recompia')}<div class="float-island"></div></div></div>`;

/**
 * Follows a sign-in for the dialog or the welcome screen: hands every state to `take`, reading it again about once a second while it
 * waits and every 5 s otherwise. run(what): does `what` (start, stop, sign out), takes its answer, reads again. stop(): no more.
 */
export function follow(take) {
  let timer = 0;
  let on = true;
  let last = null;
  const got = (st) => {
    last = st;
    if (on) take(st);
  };
  const poll = async () => {
    clearTimeout(timer);
    try {
      got(await api.cloudSigninState());
    } catch (e) {
      console.warn('cloud_signin_state', e);
    }
    if (on) timer = setTimeout(poll, last?.state === 'waiting' ? 1000 : 5000);
  };
  return {
    poll,
    async run(what) {
      try {
        got(await what());
      } catch (e) {
        toast(errText(e), 'error');
      }
      if (on) poll();
    },
    stop() {
      on = false;
      clearTimeout(timer);
    },
    get state() {
      return last;
    },
  };
}

/** Opens the dialog; `onChange(state)` hears every new state (Home redraws its chip). */
export function openSignIn(onChange) {
  const dlg = document.createElement('dialog');
  dlg.className = 'modal glass strong signin';
  dlg.setAttribute('aria-labelledby', 'modal-title');
  // the island is drawn once, so its drift never jumps; the rest is drawn again when the state changes
  dlg.innerHTML = `<div class="modal-body"><div class="signin-head">${hero()}<h2 id="modal-title">${t('Online account')}</h2></div><div class="signin-main"></div></div><div class="modal-actions"></div>`;
  document.body.append(dlg);
  const head = dlg.querySelector('.float-hero');
  const main = dlg.querySelector('.signin-main');
  const actions = dlg.querySelector('.modal-actions');
  let st = null;
  let note = '';
  let drawn = '';
  let deadline = 0;

  const draw = () => {
    // the countdown ticks here; everything else is drawn again only when it changed (the QR code stays put)
    const key = JSON.stringify({ ...st, phase: st?.phase && { ...st.phase, expires_in: 0 }, note });
    if (key !== drawn) {
      const v = view(st, icon, note, brandUrl('mark'));
      head.className = `float-hero ${heroSize(st)}`;
      dlg.classList.toggle('phone', st?.state === 'waiting' && st.phase?.phase === 'phone');
      main.innerHTML = v.body;
      actions.innerHTML = v.actions;
      drawn = key;
      deadline = Date.now() + (st?.phase?.expires_in ?? 0) * 1000;
      if (document.activeElement === dlg || !dlg.contains(document.activeElement)) actions.querySelector('button')?.focus();   // as other dialogs
    }
    const left = dlg.querySelector('#signin-left');
    if (left) left.textContent = countdown((deadline - Date.now()) / 1000);
  };
  const watch = follow((next) => {
    if (next?.state === 'signed_in' && st?.state !== 'signed_in') rememberSkip(false);
    st = next;
    draw();
    onChange?.(st);
  });

  dlg.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-signin]');
    if (!b || b.disabled) return;
    const act = b.dataset.signin;
    if (act === 'browser' || act === 'phone') {
      note = '';
      return watch.run(() => api.cloudSigninBegin({ method: act }));
    }
    if (act === 'sign-out') {
      note = 'signed-out';
      return watch.run(() => api.cloudSignOut().then((s) => { rememberSkip(false); return s; }));
    }
    if (act === 'account') return api.cloudAccountPage().catch((err) => toast(errText(err), 'error'));
    if (act === 'close') dlg.close();
  });
  dlg.addEventListener('close', () => {
    watch.stop();
    const stop = st?.state === 'waiting' ? api.cloudSigninCancel() : api.cloudSigninState();
    stop.then((s) => onChange?.(s)).catch(() => {});
    dlg.remove();
  });
  dlg.showModal();
  watch.poll();
}
