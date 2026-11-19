// The Online account dialog, opened from Home's account chip. What it says is cloud-signin-model.js; the work is cloud_signin.rs (a
// sign-in runs there on a thread of its own, and this dialog reads its state about once a second while it is open). Closing the
// dialog while it waits ("Not now", Esc) stops the sign-in: nothing is written.
import './cloud-signin.css';
import { api } from './api.js';
import { errText, icon, toast } from './ui.js';
import { t } from './i18n.js';
import { brandImg, brandUrl } from './branding.js';
import { countdown, view } from './cloud-signin-model.js';

/** The sign-in state, or null when the launcher cannot say (the chip is left out). */
export const readAccount = () => api.cloudSigninState().catch((e) => { console.warn('cloud_signin_state', e); return null; });

/** Opens the dialog; `onChange(state)` hears every new state (Home redraws its chip). */
export function openSignIn(onChange) {
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
      const v = view(st, icon, note, brandUrl('mark'));
      dlg.innerHTML = `<div class="modal-body">${brandImg('logo', 'class="signin-logo" alt="Recompia"')}<h2 id="modal-title">${t('Online account')}</h2>${v.body}</div><div class="modal-actions">${v.actions}</div>`;
      drawn = key;
      deadline = Date.now() + (st?.phase?.expires_in ?? 0) * 1000;
    }
    const left = dlg.querySelector('#signin-left');
    if (left) left.textContent = countdown((deadline - Date.now()) / 1000);
  };
  const take = (next) => {
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
      return run(() => api.cloudSignOut());
    }
    if (act === 'account') return api.cloudAccountPage().catch((err) => toast(errText(err), 'error'));
    if (act === 'close') dlg.close();
  });
  dlg.addEventListener('close', () => {
    open = false;
    clearTimeout(timer);
    const stop = st?.state === 'waiting' ? api.cloudSigninCancel() : api.cloudSigninState();
    stop.then((s) => onChange?.(s)).catch(() => {});
    dlg.remove();
  });
  dlg.showModal();
  poll();
}
