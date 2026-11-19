// Tests for the channel and version picker (src/version-model.js, src/builds-model.js forChannel). Run: node scripts/test-version-model.mjs
// (exit 1 on a failure)
import { EXPERIMENTAL, LATEST, STABLE, channelOf, pickKind, pickedOf, pickerNote, versionChoice } from '../src/version-model.js';
import { forChannel } from '../src/builds-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const b = (tag, prerelease, relation = 'older', installable = true) => ({ tag, prerelease, relation, installable });
const stable = [b('v0.16.0', false, 'newer'), b('v0.15.1', false, 'installed'), b('v0.15.0', false), b('v0.14.0', false, 'older', false)];
const experimental = [b('v0.17.0-alpha.2', true, 'newer'), b('v0.16.1-beta.1', true, 'newer'), b('v0.15.0-alpha.1', true)];

// the channel: Experimental only when launcher.json says so; the retired Testing is Stable with Latest
check(channelOf({ channel: 'experimental' }) === EXPERIMENTAL && channelOf({ channel: ' Experimental ' }) === EXPERIMENTAL, 'experimental');
for (const c of ['stable', 'testing', '', undefined, 'beta']) check(channelOf({ channel: c }) === STABLE, `${c} is Stable`);
check(channelOf(null) === STABLE, 'no config: Stable');
check(pickedOf({ channel: 'testing', picked_version: 'v0.16.0-beta.1' }) === LATEST, 'Testing becomes Latest');
check(pickedOf({ channel: 'stable', picked_version: ' v0.15.0 ' }) === 'v0.15.0', 'a pinned version is kept');

// Stable: Latest first, then every release; a release that cannot be installed here is shown but not pickable
let c = versionChoice(STABLE, [...stable, ...experimental], LATEST);
check(c.value === LATEST && c.options[0].value === LATEST && c.options[0].label === 'Latest (0.16.0)', 'Latest first, naming the newest');
check(c.options.length === 5 && c.options.every((o) => !o.value.includes('-')), 'only releases on Stable');
check(c.options.find((o) => o.value === 'v0.15.1').label === '0.15.1 (installed)', 'the installed one is marked');
check(c.options.find((o) => o.value === 'v0.14.0').disabled, 'not installable here: not pickable');
c = versionChoice(STABLE, stable, 'v0.15.0');
check(c.value === 'v0.15.0', 'a pinned version is shown as picked');
c = versionChoice(STABLE, stable, 'v0.9.0');
check(c.value === 'v0.9.0' && c.options.at(-1).disabled, 'a pinned version no longer listed still shows');
check(versionChoice(STABLE, [], LATEST).options[0].label === 'Latest', 'no list: plain Latest');

// Experimental: every prerelease, no Latest; nothing picked yet shows a line that cannot be chosen
c = versionChoice(EXPERIMENTAL, [...stable, ...experimental], LATEST);
check(c.value === LATEST && c.options[0].disabled && c.options[0].label === 'Pick a build', 'nothing picked: Pick a build');
check(c.options.slice(1).map((o) => o.value).join() === 'v0.17.0-alpha.2,v0.16.1-beta.1,v0.15.0-alpha.1', 'every prerelease, older ones too');
check(!c.options.some((o) => o.label.startsWith('Latest')), 'no Latest on Experimental');
c = versionChoice(EXPERIMENTAL, experimental, 'v0.16.1-beta.1');
check(c.value === 'v0.16.1-beta.1' && c.options.length === 3, 'a picked build, no placeholder');
check(versionChoice(EXPERIMENTAL, [], LATEST).options.every((o) => o.disabled), 'none at all: nothing to pick');

// what a pick does
check(pickKind(stable, LATEST) === 'keep', 'Latest only saves');
check(pickKind(stable, 'v0.15.1') === 'keep', 'the installed one only saves');
check(pickKind(stable, 'v0.15.0') === 'install' && pickKind(stable, 'v0.16.0') === 'install', 'another one installs');
check(pickKind(stable, 'v0.14.0') === 'none' && pickKind(stable, 'v9.9.9') === 'none', 'not installable or unknown: nothing');

// the note
check(pickerNote(STABLE, LATEST) === '', 'Latest: no note');
check(pickerNote(STABLE, 'v0.15.0').includes('0.15.0'), 'a pinned version says so');
check(pickerNote(EXPERIMENTAL, LATEST).startsWith('Pick an Experimental build'), 'Experimental with nothing picked asks for a pick');
check(pickerNote(EXPERIMENTAL, 'v0.16.1-beta.1').includes('Nothing updates by itself'), 'Experimental never updates');

// the dialog lists (pages/builds.js loadBuilds)
check(forChannel([...stable, ...experimental], EXPERIMENTAL).length === 3 && forChannel([...stable, ...experimental], STABLE).length === 4, 'forChannel splits the list');

if (bad) {
  console.log(`${bad} failed`);
  process.exit(1);
}
console.log('version model tests passed');
