// The Online account dialog, opened from Home's account chip, or by Home itself once at start (offerSignIn). What it says is
// cloud-signin-model.js; the work is cloud_signin.rs (a sign-in runs there on a thread of its own, and this dialog reads its state
// about once a second while it is open). Closing the dialog while it waits ("Not now", "Skip", Esc) stops the sign-in: nothing is written.
import './cloud-signin.css';
import { api } from './api.js';
import { errText, icon, toast } from './ui.js';
import { t } from './i18n.js';
import { brandImg, brandUrl } from './branding.js';
import { store } from './store.js';
import { autoOpen, countdown, view } from './cloud-signin-model.js';

/** The sign-in state, or null when the launcher cannot say (the chip is left out). */
export const readAccount = () => api.cloudSigninState().catch((e) => { console.warn('cloud_signin_state', e); return null; });

let offered = false;   // Home offers the sign-in by itself at most once per launcher start
let opened = false;    // the dialog was opened (from the chip too): the offer is not needed any more

/** Skip, remembered per install (launcher.json signin_skipped), and forgotten at the next sign-in or sign-out. */
async function rememberSkip(on) {
  try {
    const cfg = await api.getConfig();
    if (!!cfg.signin_skipped !== on) await api.saveConfig({ config: { ...cfg, signin_skipped: on } });
  } catch (e) {
    console.warn('signin_skipped', e);
  }
}

/**
 * Home's first draw, with the state it read: once per launcher start, opens the dialog by itself when autoOpen says so. It waits for
 * the questions the launcher asks by itself at start and for any other open dialog, and gives up when Home is no longer shown
 * (`here()` false) or the player opened the dialog meanwhile. Play never waits for it.
 */
export async function offerSignIn(st, onChange, here) {
  if (offered || !st) return;
  offered = true;
  let skipped;
  try {
    skipped = !!(await api.getConfig()).signin_skipped;
  } catch (e) {
    console.warn('get_config', e);
    return;
  }
  if (st.state === 'signed_in' && skipped) return rememberSkip(false);   // signed in some other way (the gateway's own signin)
  if (!autoOpen(st, skipped)) return;
  await store.startupQuestions();
  while (document.querySelector('dialog[open]')) await new Promise((r) => setTimeout(r, 300));
  if (here() && !opened) openSignIn(onChange, { startup: true });
}

/** Opens the dialog; `onChange(state)` hears every new state (Home redraws its chip). `startup`: Home opened it by itself, so the
 *  way out reads Skip and closing it while signed out is remembered. */
export function openSignIn(onChange, { startup = false } = {}) {
  opened = true;
  const dlg = document.createElement('dialog');
  dlg.className = 'modal glass strong signin';
  dlg.setAttribute('aria-labelledby', 'modal-title');
  document.body.append(dlg);
  let st = null;
  let note = '';
  let drawn = '';
  let deadline = 0;
  let timer = 0;
  let open = true;

  const draw = () => {
    // the countdown ticks here; everything else is drawn again only when it changed (the QR code stays put)
    const key = JSON.stringify({ ...st, phase: st?.phase && { ...st.phase, expires_in: 0 }, note });
    if (key !== drawn) {
      const v = view(st, icon, note, brandUrl('mark'), startup);
      dlg.innerHTML = `<div class="modal-body">${brandImg('logo', 'class="signin-logo" alt="Recompia"')}<h2 id="modal-title">${t('Online account')}</h2>${v.body}</div><div class="modal-actions">${v.actions}</div>`;
      drawn = key;
      deadline = Date.now() + (st?.phase?.expires_in ?? 0) * 1000;
    }
    const left = dlg.querySelector('#signin-left');
    if (left) left.textContent = countdown((deadline - Date.now()) / 1000);
  };
  const take = (next) => {
    if (next?.state === 'signed_in' && st?.state !== 'signed_in') rememberSkip(false);
    st = next;
    draw();
    onChange?.(st);
  };
  const poll = async () => {
    clearTimeout(timer);
    try {
      take(await api.cloudSigninState());
    } catch (e) {
      console.warn('cloud_signin_state', e);
    }
    if (open) timer = setTimeout(poll, st?.state === 'waiting' ? 1000 : 5000);
  };
  const run = async (what) => {
    try {
      take(await what());
    } catch (e) {
      toast(errText(e), 'error');
    }
    if (open) poll();
  };

  dlg.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-signin]');
    if (!b || b.disabled) return;
    const act = b.dataset.signin;
    if (act === 'browser' || act === 'phone') {
      note = '';
      return run(() => api.cloudSigninBegin({ method: act }));
    }
    if (act === 'sign-out') {
      note = 'signed-out';
      return run(() => api.cloudSignOut().then((s) => { rememberSkip(false); return s; }));
    }
    if (act === 'account') return api.cloudAccountPage().catch((err) => toast(errText(err), 'error'));
    if (act === 'close') dlg.close();
  });
  dlg.addEventListener('close', () => {
    open = false;
    clearTimeout(timer);
    if (startup && st?.state !== 'signed_in' && note !== 'signed-out') rememberSkip(true);   // Skip (or Esc): not offered again
    const stop = st?.state === 'waiting' ? api.cloudSigninCancel() : api.cloudSigninState();
    stop.then((s) => onChange?.(s)).catch(() => {});
    dlg.remove();
  });
  dlg.showModal();
  poll();
}
