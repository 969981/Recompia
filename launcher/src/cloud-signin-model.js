// What the online account sign-in says: Home's account chip and the Online account dialog (cloud-signin.js), from the state
// cloud_signin.rs sends: { state: off | signed_out | waiting | signed_in | expired, default_method, device_name, account_name?,
// method?, phase?: { phase: starting | browser | phone | finishing, url?, user_code?, qr_svg?, fingerprint?, expires_in? }, error? }.
// Pure: no DOM and no Tauri (icons come in as a function, pictures as URLs), so node scripts/test-cloud-signin-model.mjs runs it.
import { t } from './i18n.js';

const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/** Whether the launcher shows the sign-in at all (an account portal is set in Settings > Game > Online). */
export const shown = (st) => !!st && !!st.state && st.state !== 'off';

/**
 * Whether Home opens the dialog by itself at start: a portal is set, this device is signed out or its sign-in expired, and the
 * player has not pressed Skip since the last sign-in or sign-out (`skipped`: launcher.json signin_skipped).
 */
export const autoOpen = (st, skipped) => shown(st) && ['signed_out', 'expired'].includes(st.state) && !skipped;

/** m:ss */
export const countdown = (seconds) => {
  const s = Math.max(0, Math.round(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

const QR_SVG = /^<svg xmlns="http:\/\/www\.w3\.org\/2000\/svg" width="\d+" height="\d+" viewBox="0 0 \d+ \d+" shape-rendering="crispEdges"><rect width="\d+" height="\d+" fill="#fff"\/><path d="[Mhvz0-9,-]*" fill="#000"\/><rect x="(\d+)" y="(\d+)" width="(\d+)" height="\3" fill="#fff"\/><\/svg>$/;

/**
 * The QR code exactly as signin.rs draws it (its own size in whole pixels a module, squares in one path, then the light square in
 * the middle), else nothing: it goes into the page as markup. `mark`: a picture drawn in that square, half a module in from its
 * edges (the code is level H).
 */
export const safeQr = (svg, mark = '') => {
  const m = typeof svg === 'string' && QR_SVG.exec(svg);
  if (!m) return '';
  if (!mark) return svg;
  const [x, y, side] = m.slice(1).map(Number);
  return svg.replace(/<\/svg>$/, `<image href="${esc(mark)}" x="${x + 0.5}" y="${y + 0.5}" width="${side - 1}" height="${side - 1}"/></svg>`);
};

/** Home's chip: '' when the sign-in is hidden. */
export function chip(st, icon) {
  if (!shown(st)) return '';
  const [cls, name, label] = {
    signed_in: ['ok', 'user-check', t('Signed in: {name}', { name: st.account_name })],
    expired: ['warn', 'circle-alert', t('Sign in again')],
    waiting: ['', 'loader-circle', t('Signing in…')],
  }[st.state] ?? ['', 'log-in', t('Sign in')];
  return `<button class="chip on-art ${cls}" id="account-chip" title="${esc(t('Online account'))}">${icon(name, st.state === 'waiting' ? 'spin' : '')}${esc(label)}</button>`;
}

const btn = (act, label, cls = '', iconHtml = '') => `<button class="btn ${cls}" data-signin="${act}">${iconHtml}${esc(label)}</button>`;

/** The two ways to sign in, this platform's default first and primary. */
function methods(st, icon) {
  const pc = btn('browser', t('Use this PC'), st.default_method === 'browser' ? 'primary' : '', icon('monitor'));
  const phone = btn('phone', t('Use my phone'), st.default_method === 'phone' ? 'primary' : '', icon('smartphone'));
  return st.default_method === 'phone' ? pc + phone : phone + pc;   // the primary button sits last, next to the edge
}

const busy = (icon, text) => `<p class="signin-busy">${icon('loader-circle', 'spin')}<span>${esc(text)}</span></p>`;
const keyLine = (fp) => (fp ? `<p class="signin-key">${esc(t('Device key {fingerprint}', { fingerprint: fp }))}</p>` : '');
const error = (e) => (e ? `<p class="form-error" role="alert">${esc(e)}</p>` : '');

/**
 * The dialog for a state: { body, actions } as HTML. `note` is 'signed-out' right after Sign out. `mark`: the picture for the
 * middle of the QR code (Recompia's mark). `startup`: Home opened it by itself (autoOpen), so the way out reads Skip, except right
 * after Sign out. Buttons carry data-signin: browser | phone (start that way), close (Not now or Skip: also stops waiting), sign-out,
 * account (the account page).
 */
export function view(st, icon, note = '', mark = '', startup = false) {
  const device = st?.device_name || t('this device');
  const notNow = btn('close', startup && note !== 'signed-out' ? t('Skip') : t('Not now'), 'quiet');
  if (!shown(st)) {
    return { body: `<p>${esc(t('Online accounts are off. Set the account portal in Settings > Game > Online.'))}</p>`, actions: btn('close', t('Close'), 'primary') };
  }
  if (st.state === 'signed_in') {
    return {
      body: `<p class="signin-who">${icon('user-check')}<span>${esc(t('Signed in as {name}', { name: st.account_name }))}</span></p>
        <p>${esc(t('This {device} joins Cloud Islands with your account.', { device }))}</p>`,
      actions: btn('sign-out', t('Sign out'), 'quiet') + btn('account', t('Account page'), '', icon('external-link')) + btn('close', t('Done'), 'primary'),
    };
  }
  if (st.state === 'waiting') {
    const ph = st.phase ?? {};
    if (ph.phase === 'phone') {
      const steps = [
        t("Scan the code with your phone's camera."),
        t('Sign in with your passkey.'),
        t('Check that the device is “{device}”, then type the code below.', { device }),
        t('Approve.'),
      ];
      return {
        body: `<div class="signin-phone">
            <div class="signin-qr" aria-label="${esc(t('QR code for the sign-in page'))}" role="img">${safeQr(ph.qr_svg, mark)}</div>
            <div class="signin-steps"><ol>${steps.map((s) => `<li>${esc(s)}</li>`).join('')}</ol>
              <p class="signin-code" aria-label="${esc(t('Code'))}">${esc(ph.user_code)}</p>
              <p class="signin-left">${esc(t('Expires in'))} <span id="signin-left">${countdown(ph.expires_in ?? 0)}</span></p></div>
          </div>
          <p class="signin-url">${esc(t('No camera? Open {url} on any device.', { url: ph.url ?? '' }))}</p>${keyLine(ph.fingerprint)}`,
        actions: notNow + btn('browser', t('Use this PC instead')),
      };
    }
    if (ph.phase === 'browser') {
      return {
        body: busy(icon, t('Finish in your browser: sign in with your passkey, check that the device is “{device}”, then approve.', { device })) + keyLine(ph.fingerprint)
          + `<p class="signin-url">${esc(t('No browser opened? Use your phone instead.'))}</p>`,
        actions: notNow + btn('phone', t('Use my phone instead')),
      };
    }
    if (ph.phase === 'finishing') return { body: busy(icon, t("Approved. Getting this device's certificate…")), actions: notNow };
    return { body: busy(icon, t('Starting…')), actions: notNow };
  }
  const intro = st.state === 'expired'
    ? `<p>${esc(t("This device's sign-in has expired. Sign in again to keep joining islands with your account."))}</p>`
    : note === 'signed-out'
      ? `<p class="signin-who">${icon('circle-check')}<span>${esc(t('Signed out on this {device}.', { device }))}</span></p>
        <p>${esc(t('To revoke this device for good, remove it on your account page.'))}</p>`
      : `<p>${esc(t('Sign in to join Cloud Islands with your account. It is optional: signed out, everything plays as it does now.'))}</p>
        <p class="signin-how">${esc(t("Use this PC: approve in this PC's browser. Use my phone: scan a code and approve on your phone."))}</p>`;
  const extra = st.state === 'expired' ? btn('sign-out', t('Sign out'), 'quiet') : note === 'signed-out' ? btn('account', t('Account page'), 'quiet', icon('external-link')) : '';
  return { body: intro + error(st.error), actions: notNow + extra + methods(st, icon) };
}
