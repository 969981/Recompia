// Tests for the pure part of the Mods page (src/mods-model.js). Run: node launcher/scripts/test-mods-model.mjs
import { allowQuestion, badges, byLine, canMove, cardAction, installQuestion, isSlider, settingValue } from '../src/mods-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const mod = { id: 'm', name: 'Mod', version: '1.0.0', authors: 'A, B', license: 'MIT', gameplay: true, network: 'x.example', needs_yes: true, state: 'off' };
const texts = (m) => badges(m).map((b) => b.text);
check(texts(mod).join() === 'Not allowed yet,Changes gameplay,Goes online', 'badges of a gameplay mod with new code that goes online');
check(texts({ ...mod, gameplay: false, network: '', needs_yes: false }).join() === 'Looks only', 'a plain look-only mod');
check(texts({ ...mod, state: 'error' })[0] === 'Problem', 'a broken mod says so first');

check(byLine(mod) === 'By A, B. Licence: MIT.', 'by line from the core');
check(byLine({ authors: ['A', 'B'], license: '' }) === 'By A, B. Licence: not stated.', 'by line from the catalog, licence missing');
check(byLine({ authors: [], license: 'MIT' }) === 'Licence: MIT.', 'no authors');

const card = { id: 'c', name: 'Card', version: '1.1.0', authors: ['A'], license: 'MIT', gameplay: false, network: false, available: true, installed: '', newer: false };
check(cardAction(card, false) === 'install', 'install');
check(cardAction(card, true) === 'closed', 'a preview list installs nothing');
check(cardAction({ ...card, available: false }, false) === 'soon', 'no download: coming soon');
check(cardAction({ ...card, installed: '1.1.0' }, false) === 'installed', 'installed');
check(cardAction({ ...card, installed: '1.0.0', newer: true }, false) === 'update', 'a newer version');
check(cardAction({ ...card, installed: '1.0.0', newer: true }, true) === 'closed', 'no update from a preview list');

const iq = installQuestion({ ...card, gameplay: true, network: true });
check(iq.title === 'Install Card 1.1.0?' && iq.ok === 'Install', 'install title');
check(iq.lines[0] === 'By A. Licence: MIT.' && /rights/.test(iq.lines[1]) && /Link Play/.test(iq.lines[2]) && iq.lines[3] === 'It goes online.', 'install lines: who, code, gameplay, online');
check(installQuestion({ ...card, installed: '1.0.0' }).ok === 'Update', 'update question');
const aq = allowQuestion(mod);
check(aq.title === 'Allow Mod 1.0.0 to run?' && aq.lines.some((l) => l === 'It goes online (x.example).'), 'allow question names the hosts');

check(settingValue({ kind: 'bool' }, { checked: true }).number === 1 && settingValue({ kind: 'bool' }, { checked: false }).number === 0, 'bool');
check(settingValue({ kind: 'choice' }, { value: '2' }).number === 2, 'choice by index');
check(settingValue({ kind: 'number' }, { value: '1.25' }).number === 1.25, 'number');
check(settingValue({ kind: 'key' }, { value: 'F2' }).text === 'F2', 'key as text');
check(isSlider({ kind: 'number', min: 0.5, max: 1.6 }) && !isSlider({ kind: 'number', min: 0, max: 0 }), 'slider only with a range');
check(!canMove([1, 2], 0, -1) && canMove([1, 2], 0, 1) && !canMove([1, 2], 1, 1), 'move at the ends');

console.log(bad ? `TEST FAILED (${bad})` : 'mods model tests passed');
process.exit(bad ? 1 : 0);
