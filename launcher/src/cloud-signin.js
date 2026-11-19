// The online account: what Home's account chip and the sign-in screen (pages/welcome.js) share: the floating island with the logo,
// the skip flag, whether the launcher starts on that screen, and following a sign-in. What they say is cloud-signin-model.js; the work
// is cloud_signin.rs (a sign-in runs there on a thread of its own, and the screen reads its state about once a second while it waits).
// Leaving the screen while it waits stops the sign-in: nothing is written.
import './cloud-signin.css';
import { api } from './api.js';
import { errText, toast } from './ui.js';
import { brandImg } from './branding.js';
import { welcomeAtStart } from './cloud-signin-model.js';

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
 * Recompia's logo standing on its floating island, as on the account portal's sign-in page: the big island with its waterfall and two
 * small islets below, each picture floating on its own (cloud-signin.css, art/floating-*.png). Recompia's logo in Co-op too: the
 * account is the same. `size`: a class for the place (the sign-in screen: welcome).
 */
export const hero = (size = '') =>
  `<div class="float-hero ${size}">${brandImg('logo', 'class="float-logo" alt="Recompia"', 'recompia')}` +
  '<div class="float-isles" aria-hidden="true"><span class="float-isle"><span class="float-falls"></span></span>' +
  '<span class="float-islet float-islet-a"></span><span class="float-islet float-islet-b"></span></div></div>';

/**
 * Follows a sign-in for the sign-in screen: hands every state to `take`, reading it again about once a second while it
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
