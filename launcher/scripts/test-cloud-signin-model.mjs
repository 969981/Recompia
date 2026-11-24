// Tests for what the online account sign-in says (src/cloud-signin-model.js). Run: node scripts/test-cloud-signin-model.mjs   (exit 1 on a failure)
import { chip, countdown, needsPasskey, safeQr, shown, welcome, welcomeAtStart } from '../src/cloud-signin-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };
const icon = (name, cls = '') => `<i data-icon="${name}" class="${cls}"></i>`;
const base = { default_method: 'browser', device_name: 'Windows PC' };
const qr = '<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="M4,4h1v1h-1zM5,4h1v1h-1z" fill="#000"/><rect x="16" y="16" width="5" height="5" fill="#fff"/></svg>';
const plainQr = '<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="M4,4h1v1h-1z" fill="#000"/></svg>';

// hidden while Cloud Islands are off; no chip, and the screen goes straight on to Home
check(!shown(null) && !shown({ ...base, state: 'off' }), 'off is hidden');
check(chip({ ...base, state: 'off' }, icon) === '', 'no chip when off');
check(shown({ ...base, state: 'signed_out' }), 'signed out is shown');

// the chip says where the device stands
check(chip({ ...base, state: 'signed_out' }, icon).includes('Sign in<'), 'chip: Sign in');
check(chip({ ...base, state: 'signed_in', account_name: 'Ana' }, icon).includes('Signed in: Ana'), 'chip: signed in names the account');
check(chip({ ...base, state: 'signed_in', account_name: '<b>' }, icon).includes('&lt;b&gt;'), 'chip escapes the name');
check(chip({ ...base, state: 'expired' }, icon).includes('class="chip on-art warn"'), 'chip: expired warns');
check(['signed_out', 'signed_in', 'expired', 'waiting'].every((state) => chip({ ...base, state }, icon).includes('<span class="chip exp-tag">Experimental</span></button>')), 'chip: marked Experimental in every state');

// waiting on a phone: QR, the short code, the device name, a countdown, the other way
const phoneSt = { ...base, state: 'waiting', method: 'phone', phase: { phase: 'phone', url: 'http://localhost:8790/link?ticket=x', user_code: 'ABCD-EFGH', qr_svg: qr, fingerprint: '0123 4567 89AB CDEF', expires_in: 299 } };
const phone = welcome(phoneSt, icon);
check(phone.body.includes(qr), 'the QR code is drawn');
check(phone.body.includes('ABCD-EFGH') && phone.body.includes('“Windows PC”') && phone.body.includes('4:59'), 'code, device name, countdown');
check(phone.body.includes('0123 4567 89AB CDEF') && phone.body.includes('link?ticket=x'), 'the key line and the link for no camera');
check(safeQr('<svg onload="x()"></svg>') === '' && safeQr(qr) === qr && safeQr(plainQr) === '', 'only signin.rs-shaped SVG goes in as markup');
check(safeQr(qr.replace(' width="148" height="148"', '')) === '' && safeQr(qr.replace('width="148"', 'width="100%"')) === '', 'the QR code must bring its own pixel size');
const marked = welcome({ ...base, state: 'waiting', method: 'phone', phase: { phase: 'phone', user_code: 'A', qr_svg: qr } }, icon, 'art/mark.png?"x');
check(marked.body.includes('<image href="art/mark.png?&quot;x" x="16.5" y="16.5" width="4" height="4"/></svg>'), 'the mark sits in the light middle square, half a module in');
check(safeQr('<svg xmlns="http://www.w3.org/2000/svg" width="148" height="148" viewBox="0 0 37 37" shape-rendering="crispEdges"><rect width="37" height="37" fill="#fff"/><path d="" fill="#000"/><rect x="16" y="16" width="5" height="6" fill="#fff"/></svg>', 'm.png') === '', 'a middle square that is not square is left out');
check(!welcome({ ...base, state: 'waiting', phase: { phase: 'phone', user_code: 'A', qr_svg: '<svg><script>x</script></svg>' } }, icon).body.includes('<script>'), 'a strange QR is left out');

// the launcher starts on the welcome screen: signed out or expired with Cloud Islands on, until Skip; never with them off, signed in or waiting
check(welcomeAtStart({ ...base, state: 'signed_out' }, false) && welcomeAtStart({ ...base, state: 'expired' }, false), 'welcome: signed out, expired');
check(!welcomeAtStart({ ...base, state: 'signed_out' }, true) && !welcomeAtStart({ ...base, state: 'expired' }, true), 'welcome: not after Skip');
check(!welcomeAtStart(null, false) && !welcomeAtStart({ ...base, state: 'off' }, false), 'welcome: not while Cloud Islands are off');
check(!welcomeAtStart({ ...base, state: 'signed_in', account_name: 'Ana' }, false) && !welcomeAtStart({ ...base, state: 'waiting' }, false), 'welcome: not signed in or waiting');

// at start (the welcome): the platform's way in first and primary, the other one under it, Skip on every step but signed in
const wPc = welcome({ ...base, state: 'signed_out' }, icon);
check(wPc.step === 'choose' && /data-signin="browser">.*Sign in with this PC.*data-signin="phone">.*Sign in with my phone/s.test(wPc.body), 'welcome on Windows: this PC first');
check(wPc.body.includes('btn primary" data-signin="browser"') && !wPc.body.includes('btn primary" data-signin="phone"'), 'welcome on Windows: this PC is primary');
const wDeck = welcome({ ...base, default_method: 'phone', state: 'signed_out' }, icon);
check(/data-signin="phone">.*data-signin="browser">/s.test(wDeck.body) && wDeck.body.includes('btn primary" data-signin="phone"'), 'welcome elsewhere: my phone first and primary');
check(wPc.body.includes('One account for your islands and everything online.') && wPc.body.includes('Optional: everything plays without it.'), 'welcome: what it is for, optional');
check(wPc.actions.includes('data-signin="skip"') && wPc.actions.includes('>Skip<'), 'welcome: Skip');
check(/data-signin="skip" data-pad-back>/.test(wPc.actions) && (wPc.actions.match(/data-pad-back/g) || []).length === 1, "welcome: the pad's B is Skip, as Esc");
check(welcome({ ...base, state: 'expired' }, icon).body.includes('expired') && welcome({ ...base, state: 'signed_out', error: 'declined' }, icon).body.includes('form-error'), 'welcome: expired, an error');
const wPhone = welcome({ ...base, state: 'waiting', method: 'phone', phase: { phase: 'phone', url: 'http://localhost:8790/link?ticket=x', user_code: 'ABCD-EFGH', qr_svg: qr, fingerprint: '0123 4567 89AB CDEF', expires_in: 299 } }, icon);
check(wPhone.step === 'phone' && wPhone.body.includes(qr) && wPhone.body.includes('ABCD-EFGH') && wPhone.body.includes('0123 4567 89AB CDEF'), 'welcome phone: QR, code, key');
check(wPhone.actions.includes('data-signin="back"') && wPhone.actions.includes('data-signin="browser"') && wPhone.actions.includes('data-signin="skip"'), 'welcome phone: Back, this PC, Skip');
const wBrowser = welcome({ ...base, state: 'waiting', phase: { phase: 'browser', fingerprint: 'AAAA' } }, icon);
check(wBrowser.step === 'browser' && wBrowser.body.includes('Finish in your browser') && wBrowser.actions.includes('data-signin="back"') && wBrowser.actions.includes('data-signin="phone"') && wBrowser.actions.includes('data-signin="skip"'), 'welcome browser: wait, Back, phone, Skip');
const wBusy = welcome({ ...base, state: 'waiting', phase: { phase: 'finishing' } }, icon);
check(wBusy.step === 'busy' && wBusy.body.includes('certificate') && wBusy.actions.includes('data-signin="skip"'), 'welcome finishing: Skip');
const wIn = welcome({ ...base, state: 'signed_in', account_name: '<Ana>' }, icon);
check(wIn.step === 'done' && wIn.body.includes('Signed in as &lt;Ana&gt;') && wIn.actions === '', 'welcome signed in: names the account (escaped), no buttons');
check(welcome({ ...base, state: 'off' }, icon).step === 'off' && welcome(null, icon).step === 'off', 'welcome with Cloud Islands off: on to Home');

// opened from Home's chip: the same screen, its way out is Back (data-signin leave), never Skip, on every step
const ways = (v) => [...v.actions.matchAll(/data-signin="(\w[\w-]*)"/g)].map((m) => m[1]).join(' ');
const cOut = welcome({ ...base, state: 'signed_out' }, icon, '', { chip: true });
check(/data-signin="leave" data-pad-back>/.test(cOut.actions) && (cOut.actions.match(/data-pad-back/g) || []).length === 1, "chip: the pad's B is Back, as Esc");
check(cOut.step === 'choose' && cOut.body === wPc.body && ways(cOut) === 'leave' && cOut.actions.includes('>Back<') && !cOut.actions.includes('Skip'), 'chip: the same choice, Back instead of Skip');
check(ways(welcome(phoneSt, icon, '', { chip: true })) === 'browser leave', 'chip phone: this PC, Back (one Back: leaving stops the wait)');
check(ways(welcome({ ...base, state: 'waiting', phase: { phase: 'browser' } }, icon, '', { chip: true })) === 'phone leave', 'chip browser: my phone, Back');
check(ways(welcome({ ...base, state: 'waiting', phase: { phase: 'starting' } }, icon, '', { chip: true })) === 'leave', 'chip busy: Back');
check(ways(wPhone) === 'back browser skip' && ways(wBusy) === 'back skip', 'at start: Back steps back, Skip leaves');
// signed in, from the chip: the account, Sign out, the account page, Back; nothing leaves by itself
const cIn = welcome({ ...base, state: 'signed_in', account_name: '<Ana>' }, icon, '', { chip: true });
check(cIn.step === 'account' && cIn.body.includes('Signed in as &lt;Ana&gt;') && cIn.body.includes('This Windows PC joins Cloud Islands'), 'chip signed in: who, escaped');
check(ways(cIn) === 'sign-out account leave' && cIn.actions.includes('welcome-aside'), 'chip signed in: Sign out (aside), account page, Back');
// right after Sign out: says so, how to revoke, the account page, both ways in again
const cGone = welcome({ ...base, state: 'signed_out' }, icon, '', { chip: true, note: 'signed-out' });
check(cGone.step === 'choose' && cGone.body.includes('Signed out on this Windows PC.') && cGone.body.includes('remove it on your account page') && cGone.body.includes('data-signin="browser"'), 'after Sign out: the note, the ways in');
check(ways(cGone) === 'account leave', 'after Sign out: account page, Back');
// expired: says so, Sign out offered, both ways in, at start and from the chip
const cOld = welcome({ ...base, state: 'expired' }, icon, '', { chip: true });
check(cOld.body.includes('expired') && cOld.body.includes('data-signin="phone"') && ways(cOld) === 'sign-out leave', 'chip expired: Sign out, Back');
check(ways(welcome({ ...base, state: 'expired' }, icon)) === 'sign-out skip', 'start expired: Sign out, Skip');
check(welcome({ ...base, state: 'off' }, icon, '', { chip: true }).step === 'off', 'chip with Cloud Islands off: back to Home');

// signed in, but the account's passkeys died with the server's old address: the chip warns, the launcher starts on the screen, and the
// card offers the new passkey and no Sign out (which would lock the account), at start and from the chip
const stuck = { ...base, state: 'signed_in', account_name: 'Ana', needs_passkey: true };
check(needsPasskey(stuck) && !needsPasskey({ ...stuck, needs_passkey: false }) && !needsPasskey({ ...base, state: 'expired', needs_passkey: true }), 'a new passkey is needed only while signed in');
check(chip(stuck, icon).includes('class="chip on-art warn"') && chip(stuck, icon).includes('Set up a new passkey'), 'chip: asks for the new passkey');
check(welcomeAtStart(stuck, false) && !welcomeAtStart(stuck, true) && !welcomeAtStart({ ...stuck, needs_passkey: false }, false), 'start: the new-passkey screen until Skip');
const sStuck = welcome(stuck, icon);
const cStuck = welcome(stuck, icon, '', { chip: true });
check(sStuck.step === 'passkey' && cStuck.step === 'passkey' && cStuck.body.includes('Signed in as Ana') && cStuck.body.includes('do not sign out: this Windows PC'), 'new passkey: who and why');
check(ways(sStuck) === 'new-passkey skip' && ways(cStuck) === 'new-passkey leave', 'new passkey: its button and the way out, no Sign out');
check(!cStuck.body.includes('Finish in your browser') && welcome(stuck, icon, '', { chip: true, note: 'passkey-opened' }).body.includes('Finish in your browser: create the new passkey there.'), 'new passkey: says where to finish once the page is open');

check(countdown(0) === '0:00' && countdown(61.4) === '1:01' && countdown(-5) === '0:00', 'countdown');

if (bad) { console.log(`${bad} failed`); process.exit(1); }
console.log('cloud-signin-model: all passed');
