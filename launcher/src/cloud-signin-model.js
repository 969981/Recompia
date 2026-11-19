// What the online account sign-in says: Home's account chip and the sign-in screen it opens, the one the launcher also starts on
// (pages/welcome.js), from the state cloud_signin.rs sends: { state: off | signed_out | waiting | signed_in | expired, default_method, device_name, account_name?,
// method?, phase?: { phase: starting | browser | phone | finishing, url?, user_code?, qr_svg?, fingerprint?, expires_in? }, error? }.
// Pure: no DOM and no Tauri (icons come in as a function, pictures as URLs), so node scripts/test-cloud-signin-model.mjs runs it.
import { t } from './i18n.js';

const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

/** Whether the launcher shows the sign-in at all (an account portal is set in Settings > Game > Online). */
export const shown = (st) => !!st && !!st.state && st.state !== 'off';

/**
 * Whether the launcher starts on the welcome screen (pages/welcome.js) before Home: a portal is set, this device is signed out or
 * its sign-in expired, and the player has not pressed Skip since the last sign-in or sign-out (`skipped`: launcher.json
 * signin_skipped).
 */
export const welcomeAtStart = (st, skipped) => shown(st) && ['signed_out', 'expired'].includes(st.state) && !skipped;

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

const busy = (icon, text) => `<p class="signin-busy">${icon('loader-circle', 'spin')}<span>${esc(text)}</span></p>`;
const keyLine = (fp) => (fp ? `<p class="signin-key">${esc(t('Device key {fingerprint}', { fingerprint: fp }))}</p>` : '');
const error = (e) => (e ? `<p class="form-error" role="alert">${esc(e)}</p>` : '');

/** Waiting on a phone: the QR code beside the steps, the short code, the countdown (#signin-left) and the device key; the link for a
 *  phone without a camera under them. */
function phoneBlock(ph, mark, device) {
  const steps = [
    t("Scan the code with your phone's camera."),
    t('Sign in with your passkey.'),
    t('Check that the device is “{device}”, then type the code below.', { device }),
    t('Approve.'),
  ];
  return `<div class="signin-phone">
      <div class="signin-qr" aria-label="${esc(t('QR code for the sign-in page'))}" role="img">${safeQr(ph.qr_svg, mark)}</div>
      <div class="signin-steps"><ol>${steps.map((s) => `<li>${esc(s)}</li>`).join('')}</ol>
        <p class="signin-code" aria-label="${esc(t('Code'))}">${esc(ph.user_code)}</p>
        <p class="signin-left">${esc(t('Expires in'))} <span id="signin-left">${countdown(ph.expires_in ?? 0)}</span></p>${keyLine(ph.fingerprint)}</div>
    </div>
    <p class="signin-url">${esc(t('No camera? Open {url} on any device.', { url: ph.url ?? '' }))}</p>`;
}

/** Waiting in this PC's browser. */
const browserBlock = (icon, ph, device) =>
  busy(icon, t('Finish in your browser: sign in with your passkey, check that the device is “{device}”, then approve.', { device })) + keyLine(ph.fingerprint)
  + `<p class="signin-url">${esc(t('No browser opened? Use your phone instead.'))}</p>`;

/**
 * The sign-in screen's card for a state (pages/welcome.js): { step, body, actions } as HTML. The launcher starts on it (its way out
 * is Skip, remembered) and Home's account chip opens it (`chip`: its way out is Back, nothing remembered). step: choose (the two ways
 * in, the platform's default first and primary; it says so when the sign-in expired, or right after Sign out with `note`
 * 'signed-out'), phone, browser, busy (starting, finishing), done (signed in at start: the screen goes on to Home by itself),
 * account (signed in, from the chip: who, Sign out, the account page), off (no portal: straight on to Home). The way out is on every
 * step but done. `mark`: the picture for the middle of the QR code (Recompia's mark). Buttons carry data-signin: browser | phone
 * (start that way), back (stop waiting, choose again), skip (on to Home, remembered), leave (Back to Home), sign-out, account (the
 * account page).
 */
export function welcome(st, icon, mark = '', { chip = false, note = '' } = {}) {
  const device = st?.device_name || t('this device');
  const out = chip ? btn('leave', t('Back'), 'welcome-skip') : btn('skip', t('Skip'), 'welcome-skip');
  // at start Back steps back to the choice and Skip leaves; from the chip the way out is Back itself (leaving stops the wait), so
  // one Back, not two
  const back = chip ? '' : btn('back', t('Back'), 'quiet welcome-back', icon('arrow-left'));
  const aside = (act, label, iconHtml = '') => btn(act, label, 'quiet welcome-aside', iconHtml);
  if (!shown(st)) return { step: 'off', body: '', actions: out };
  if (st.state === 'signed_in') {
    const who = (name) => `<p class="signin-who">${icon(name)}<span>${esc(t('Signed in as {name}', { name: st.account_name }))}</span></p>`;
    if (!chip) return { step: 'done', body: `${who('circle-check')}<p>${esc(t('Taking you to the launcher…'))}</p>`, actions: '' };
    return {
      step: 'account',
      body: `${who('user-check')}<p>${esc(t('This {device} joins Cloud Islands with your account.', { device }))}</p>`,
      actions: aside('sign-out', t('Sign out')) + btn('account', t('Account page'), '', icon('external-link')) + out,
    };
  }
  if (st.state === 'waiting') {
    const ph = st.phase ?? {};
    if (ph.phase === 'phone') return { step: 'phone', body: phoneBlock(ph, mark, device), actions: back + btn('browser', t('Use this PC instead')) + out };
    if (ph.phase === 'browser') return { step: 'browser', body: browserBlock(icon, ph, device), actions: back + btn('phone', t('Use my phone instead')) + out };
    const text = ph.phase === 'finishing' ? t("Approved. Getting this device's certificate…") : t('Starting…');
    return { step: 'busy', body: busy(icon, text), actions: back + out };
  }
  const phoneFirst = st.default_method === 'phone';
  const pc = btn('browser', t('Sign in with this PC'), phoneFirst ? '' : 'primary', icon('monitor'));
  const phone = btn('phone', t('Sign in with my phone'), phoneFirst ? 'primary' : '', icon('smartphone'));
  const optional = `<p class="signin-how">${esc(t('Optional: everything plays without it.'))}</p>`;
  const intro = st.state === 'expired'
    ? `<p>${esc(t("This device's sign-in has expired. Sign in again to keep joining islands with your account."))}</p>${optional}`
    : note === 'signed-out'
      ? `<p class="signin-who">${icon('circle-check')}<span>${esc(t('Signed out on this {device}.', { device }))}</span></p>
        <p class="signin-how">${esc(t('To revoke this device for good, remove it on your account page.'))}</p>`
      : `<p>${esc(t('One account for your islands and everything online.'))}</p>${optional}`;
  const extra = st.state === 'expired'
    ? aside('sign-out', t('Sign out'))
    : note === 'signed-out' ? aside('account', t('Account page'), icon('external-link')) : '';
  return {
    step: 'choose',
    body: `${intro}${error(st.error)}
      <div class="welcome-methods">${phoneFirst ? phone + pc : pc + phone}</div>`,
    actions: extra + out,
  };
}
