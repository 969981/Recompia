// Tests for what the online account sign-in says (src/cloud-signin-model.js). Run: node scripts/test-cloud-signin-model.mjs   (exit 1 on a failure)
import { autoOpen, chip, countdown, safeQr, shown, view } from '../src/cloud-signin-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };
const icon = (name, cls = '') => `<i data-icon="${name}" class="${cls}"></i>`;
const base = { default_method: 'browser', device_name: 'Windows PC' };
const qr = '<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="M4,4h1v1h-1zM5,4h1v1h-1z" fill="#000"/><rect x="16" y="16" width="5" height="5" fill="#fff"/></svg>';
const plainQr = '<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="M4,4h1v1h-1z" fill="#000"/></svg>';

// hidden without a portal; no chip, no dialog content to speak of
check(!shown(null) && !shown({ ...base, state: 'off' }), 'off is hidden');
check(chip({ ...base, state: 'off' }, icon) === '', 'no chip when off');
check(shown({ ...base, state: 'signed_out' }), 'signed out is shown');

// the chip says where the device stands
check(chip({ ...base, state: 'signed_out' }, icon).includes('Sign in<'), 'chip: Sign in');
check(chip({ ...base, state: 'signed_in', account_name: 'Ana' }, icon).includes('Signed in: Ana'), 'chip: signed in names the account');
check(chip({ ...base, state: 'signed_in', account_name: '<b>' }, icon).includes('&lt;b&gt;'), 'chip escapes the name');
check(chip({ ...base, state: 'expired' }, icon).includes('class="chip on-art warn"'), 'chip: expired warns');

// signed out: both ways, the platform's default is the primary one
const pc = view({ ...base, state: 'signed_out' }, icon);
check(/data-signin="phone">.*data-signin="browser"/s.test(pc.actions) && pc.actions.includes('btn primary" data-signin="browser"'), 'Windows: Use this PC is primary');
const deck = view({ ...base, default_method: 'phone', device_name: 'Handheld', state: 'signed_out' }, icon);
check(deck.actions.includes('btn primary" data-signin="phone"'), 'handheld: Use my phone is primary');
check(pc.actions.includes('data-signin="close"') && pc.actions.includes('Not now'), 'Not now is offered');
check(view({ ...base, state: 'signed_out', error: 'declined' }, icon).body.includes('form-error'), 'an error is shown');

// waiting on a phone: QR, the short code, the device name, a countdown, the other way
const phone = view({ ...base, state: 'waiting', method: 'phone', phase: { phase: 'phone', url: 'http://localhost:8790/link?ticket=x', user_code: 'ABCD-EFGH', qr_svg: qr, fingerprint: '0123 4567 89AB CDEF', expires_in: 299 } }, icon);
check(phone.body.includes(qr), 'the QR code is drawn');
check(phone.body.includes('ABCD-EFGH') && phone.body.includes('“Windows PC”') && phone.body.includes('4:59'), 'code, device name, countdown');
check(phone.actions.includes('data-signin="browser"'), 'can switch to this PC');
check(safeQr('<svg onload="x()"></svg>') === '' && safeQr(qr) === qr && safeQr(plainQr) === '', 'only signin.rs-shaped SVG goes in as markup');
check(safeQr(qr.replace(' width="148" height="148"', '')) === '' && safeQr(qr.replace('width="148"', 'width="100%"')) === '', 'the QR code must bring its own pixel size');
const marked = view({ ...base, state: 'waiting', method: 'phone', phase: { phase: 'phone', user_code: 'A', qr_svg: qr } }, icon, '', 'art/mark.png?"x');
check(marked.body.includes('<image href="art/mark.png?&quot;x" x="16.5" y="16.5" width="4" height="4"/></svg>'), 'the mark sits in the light middle square, half a module in');
check(safeQr('<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="" fill="#000"/><rect x="16" y="16" width="5" height="6" fill="#fff"/></svg>', 'm.png') === '', 'a middle square that is not square is left out');
check(!view({ ...base, state: 'waiting', phase: { phase: 'phone', user_code: 'A', qr_svg: '<svg><script>x</script></svg>' } }, icon).body.includes('<script>'), 'a strange QR is left out');

// waiting in the browser, finishing
check(view({ ...base, state: 'waiting', phase: { phase: 'browser', fingerprint: 'AAAA' } }, icon).body.includes('Finish in your browser'), 'browser wait');
check(view({ ...base, state: 'waiting', phase: { phase: 'finishing' } }, icon).body.includes('certificate'), 'finishing');

// signed in / signed out note / expired
const inn = view({ ...base, state: 'signed_in', account_name: 'Ana' }, icon);
check(inn.body.includes('Signed in as Ana') && inn.actions.includes('data-signin="sign-out"') && inn.actions.includes('data-signin="account"'), 'signed in: name, Sign out, account page');
const out = view({ ...base, state: 'signed_out' }, icon, 'signed-out');
check(out.body.includes('remove it on your account page') && out.actions.includes('data-signin="account"'), 'after sign out: how to revoke');
check(view({ ...base, state: 'expired' }, icon).body.includes('expired'), 'expired says so');

// Home opens it by itself at start: signed out or expired with a portal, until Skip; never with no portal, signed in or waiting
check(autoOpen({ ...base, state: 'signed_out' }, false) && autoOpen({ ...base, state: 'expired' }, false), 'opens by itself: signed out, expired');
check(!autoOpen({ ...base, state: 'signed_out' }, true) && !autoOpen({ ...base, state: 'expired' }, true), 'not after Skip');
check(!autoOpen(null, false) && !autoOpen({ ...base, state: 'off' }, false), 'not without a portal');
check(!autoOpen({ ...base, state: 'signed_in', account_name: 'Ana' }, false) && !autoOpen({ ...base, state: 'waiting' }, false), 'not signed in or waiting');
const startup = view({ ...base, state: 'signed_out' }, icon, '', '', true);
check(startup.actions.includes('data-signin="close">Skip<') && !startup.actions.includes('Not now'), 'at start the way out is Skip');
check(view({ ...base, state: 'expired' }, icon, '', '', true).actions.includes('>Skip<'), 'expired at start: Skip');
check(view({ ...base, state: 'waiting', phase: { phase: 'phone', user_code: 'A', qr_svg: qr } }, icon, '', '', true).actions.includes('>Skip<'), 'waiting at start: Skip stops it');
check(view({ ...base, state: 'signed_out' }, icon, 'signed-out', '', true).actions.includes('Not now'), 'right after Sign out: Not now');
check(!view({ ...base, state: 'signed_in', account_name: 'Ana' }, icon, '', '', true).actions.includes('Skip'), 'signed in: no Skip');

check(countdown(0) === '0:00' && countdown(61.4) === '1:01' && countdown(-5) === '0:00', 'countdown');

if (bad) { console.log(`${bad} failed`); process.exit(1); }
console.log('cloud-signin-model: all passed');
