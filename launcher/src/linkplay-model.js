// Link Play page: pkp.ini values <-> what the page shows, and the edits a choice makes. Pure: no DOM, no Tauri, so `node --test` runs it.
//
// The contract, unchanged from the page before it (runtime/docs/CONFIG.md [net], docs/multiplayer/README.md):
//   mode off, 0 or an empty value          Off. A missing key is auto, like the runtime.
//   mode auto, internet false              Same network. One setting both hosts and visits: the game decides by Invite Others / Visit a Friend.
//   mode join:<ip>[:port], internet false  Same network, Join directly: scanning asks that host only.
//   mode auto, internet true, relay        Internet through a relay: hosts and visitors write the same file. The relay lists every room and the
//                                          game's search (Visit a Friend) shows them; the game's own Link Code keeps a room private.
//   mode auto, internet true, no relay     Internet host without a relay (router forwards 47600). A visitor cannot use this, so it opens as Host.
//   mode join:<ip>, internet true          Internet visit without a relay.
//   room_code                              gone (rooms are listed); every choice clears it, as the launcher does at start (settings.rs).
//   protocol linkplay                      the old relay Link Play. Any Link Play choice writes protocol = native; the toggles do not (as before).
// Internet goes through PUBLIC_RELAY unless the player turns on Advanced > Use my own relay or address (`server` 'own', kept in
// localStorage while nothing is written yet); a file whose relay is another address, or with no relay, opens in 'own'. Host or Visit is
// only asked there (the role lives in localStorage while the file cannot tell).
// `[net] server` belongs to the old protocol and is never written here.
import { t } from './i18n.js';

export const HOST_PORT = 47600;
export const RELAY_PORT = 47610;

/** The project's public relay (launcher/relay, run as a service): where Internet hosts open their rooms and visitors find them. IPv4 only:
 *  the game's [net] relay takes no host name (coop_cfg_parse_ex). */
export const PUBLIC_RELAY = '204.216.111.91:47610';

const truthy = (s) => /^(1|true|yes|on)$/i.test((s || '').trim());
const net = (key, value) => ({ file: 'pkp', section: 'net', key, value });
const pkpKey = (key, value) => ({ file: 'pkp', section: 'pkp', key, value });

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$/;

/** { ip: [a, b, c, d], port } of `IPv4[:port]`, or null. Refuses what coop_cfg_parse_ex refuses: 0.x, multicast and up, port 0. */
export function parseAddr(s) {
  const m = IPV4.exec(String(s ?? '').trim());
  if (!m) return null;
  const ip = m.slice(1, 5).map(Number);
  if (ip.some((x) => x > 255) || ip[0] === 0 || ip[0] >= 224) return null;
  const port = m[5] === undefined ? null : +m[5];
  if (port !== null && (port < 1 || port > 65535)) return null;
  return { ip, port };
}

export const validAddr = (s) => !!parseAddr(s);

/** coop_addr_allowed for a LAN: loopback, RFC 1918, link-local. With internet off the game turns Link Play off for any other join address. */
export function localAddr(s) {
  const a = parseAddr(s);
  if (!a) return false;
  const [x, y] = a.ip;
  return x === 127 || x === 10 || (x === 172 && y >= 16 && y <= 31) || (x === 192 && y === 168) || (x === 169 && y === 254);
}

/** 127.x: this PC itself. */
export const loopbackAddr = (s) => parseAddr(s)?.ip[0] === 127;

/**
 * What is wrong with a typed address, '' when nothing. `lan`: it is a Join directly address. `remote`: it is another PC's (a visitor
 * typing the host's or the relay's), so 127.x, which means the visitor's own PC, is refused (#245).
 */
export function addrProblem(s, { port = HOST_PORT, lan = false, remote = false } = {}) {
  const v = String(s ?? '').trim();
  if (!v) return '';
  if (!validAddr(v)) return t('Type an address like 192.0.2.10 or 192.0.2.10:{port}', { port });
  if (remote && loopbackAddr(v)) return t('That is your own PC. Type the address the host shares with you.');
  return lan && !localAddr(v) ? t('Not on your network. Use Internet for this address.') : '';
}

/** The keys this page cares about, read the way the runtime reads them. */
export function readIni(pkp) {
  const ini = (k, d = '') => (pkp[k] ?? d).trim();
  return {
    legacy: ini('net.protocol', 'native').toLowerCase() === 'linkplay',
    mode: ini('net.mode', 'auto').toLowerCase(),
    internet: truthy(ini('net.internet')),
    relay: ini('net.relay'),
    interact: truthy(ini('pkp.link_interact_everywhere')),
    story: truthy(ini('pkp.link_story_coop')),
  };
}

const isOff = (n) => n.mode === 'off' || n.mode === '0' || !n.mode;
const joinOf = (n) => (n.mode.startsWith('join:') ? n.mode.slice(5) : '');

export const viewOf = (n) => (isOff(n) ? 'off' : n.internet ? 'internet' : 'lan');

/**
 * The page's state for this file and this browser's memory. `mem` is localStorage: { server, role, through, joinAddr, relayAddr }.
 * `relayAddr` is only ever an own relay: the public one is never remembered, so Advanced opens with the player's own.
 */
export function uiFrom(pkp, mem = {}) {
  const n = readIni(pkp);
  const view = viewOf(n);
  const internet = view === 'internet';
  const join = joinOf(n);
  const direct = internet && !!join;
  const pub = internet && n.relay === PUBLIC_RELAY;
  const server = internet ? (pub ? 'public' : 'own') : mem.server === 'own' ? 'own' : 'public';
  const role = direct ? 'visit' : internet && !n.relay ? 'host' : mem.role === 'visit' ? 'visit' : 'host';
  const through = direct ? false : internet && n.relay && !pub ? true : mem.through !== false;
  const remembered = (s) => (validAddr(s) && s !== PUBLIC_RELAY ? s : '');
  const memRelay = remembered(mem.relayAddr);
  const ownRelay = pub ? '' : n.relay;
  return {
    view,
    server,
    role,
    through,
    legacy: n.legacy,
    lanJoin: view === 'lan' ? join : '',
    lastJoin: remembered(mem.joinAddr),
    relay: internet && !direct && !pub ? ownRelay : ownRelay || memRelay,
    visitAddr: direct ? join : internet && ownRelay ? ownRelay : through ? memRelay : remembered(mem.joinAddr),
    interact: n.interact || n.story,
    story: n.story,
  };
}

const choice = (...edits) => [net('protocol', 'native'), ...edits];

export const offEdits = () => choice(net('mode', 'off'));

/** Same network: automatic, or one host's address. */
export const lanEdits = (addr = '') => choice(net('mode', addr ? `join:${addr}` : 'auto'), net('internet', 'false'), net('relay', ''), net('room_code', ''));

/** Internet through a relay (hosts and visitors alike), or a host without one (empty relay). */
export const hostEdits = (relay) => choice(net('mode', 'auto'), net('internet', 'true'), net('relay', relay), net('room_code', ''));

export const visitEdits = (addr, through) => choice(net('mode', through ? 'auto' : `join:${addr}`), net('internet', 'true'), net('relay', through ? addr : ''), net('room_code', ''));

export const protocolEdits = () => choice();

export const interactEdits = (on) => [pkpKey('link_interact_everywhere', on ? 'true' : 'false')];

/** Story on turns the first toggle on too and locks it; story off leaves it as it is. */
export const storyEdits = (on) => (on ? [pkpKey('link_story_coop', 'true'), pkpKey('link_interact_everywhere', 'true')] : [pkpKey('link_story_coop', 'false')]);

export function applyEdits(pkp, edits) {
  const next = { ...pkp };
  for (const e of edits) next[`${e.section}.${e.key}`] = e.value;
  return next;
}

// What the game does with these values: forms that mean the same compare equal, keys it ignores do not count.
function meaning(n) {
  if (isOff(n)) return 'off';
  // auto and auto:<port> are one choice. Anything else (join:<ip>, a bare address, a typo) is its own, so choosing Host or Same network
  // rewrites it: a join:<ip> left in the file makes the game visit-only, and a host there is unreachable (#239)
  const mode = /^auto(:\d+)?$/.test(n.mode) ? 'auto' : n.mode;
  return n.internet ? JSON.stringify([mode, n.relay]) : mode;
}

/** Would these edits change what the game does? The protocol line alone never counts: it goes with a real choice. */
export const changes = (pkp, edits) => meaning(readIni(pkp)) !== meaning(readIni(applyEdits(pkp, edits)));

/**
 * The edits for what the page shows now: [] when the file already says it, null while a field is empty or invalid, so nothing half-written
 * is saved.
 */
export function editsFor(ui, pkp) {
  let edits;
  if (ui.view === 'off') edits = offEdits();
  else if (ui.view === 'lan') {
    const addr = ui.lanJoin.trim();
    if (addrProblem(addr, { lan: true, remote: true })) return null;
    edits = lanEdits(addr);
  } else if (ui.server === 'public') edits = hostEdits(PUBLIC_RELAY);
  else if (ui.role === 'host') {
    const relay = ui.relay.trim();
    if (addrProblem(relay, { port: RELAY_PORT })) return null;
    edits = hostEdits(relay);
  } else {
    const addr = ui.visitAddr.trim();
    if (!validAddr(addr) || addrProblem(addr, { remote: true })) return null;
    edits = visitEdits(addr, ui.through);
  }
  return changes(pkp, edits) ? edits : [];
}

/** Port of a relay_status / relay_start answer. */
export const relayPort = (status) => +String(status?.addr ?? '').split(':').pop() || RELAY_PORT;

/** lan_report as rows to show: { addr, label }, best first. An overlay's label is "internet via Tailscale": the name is enough. */
export function addressRows(report, port) {
  if (!report) return [];
  return [
    ...report.lan.map((a) => ({ addr: `${a.ip}:${port}`, label: a.adapter })),
    ...report.overlay.map((a) => ({ addr: `${a.ip}:${port}`, label: a.label.replace(/^internet via /, '') })),
  ];
}

/**
 * What an internet host gives its visitors: { addr, through, port } or null. `publicIp`: this PC's public address (null: not known),
 * `relayOn`: the port of this PC's relay while it runs. A relay elsewhere is shared as it is; this PC's relay and a host without a relay
 * are reached at the public address, on the relay's port or the host port.
 */
export function internetShare(ui, { publicIp, relayOn }) {
  const relay = ui.relay.trim();
  if (relay && !loopbackAddr(relay)) return { addr: relay, through: true, port: relayPort({ addr: relay }), own: false };
  if (relay && !relayOn) return null;   // pointed at this PC's relay while it is off: nothing to share
  const port = relay ? relayOn : HOST_PORT;
  return { addr: publicIp ? `${publicIp}:${port}` : '', through: !!relay, port, own: true };
}

/** Where to go in the game (the game's own menu names, translated like the rest of the launcher). */
export function pathParts(ui) {
  const invite = t('Invite Others to Visit');
  const visit = t('Visit a Friend');
  const own = ui.view === 'internet' && ui.server === 'own';   // only there does the page know which one the player does
  const last = own ? (ui.role === 'host' ? invite : visit) : ui.view === 'lan' && ui.lanJoin.trim() ? visit : `${invite} / ${visit}`;
  return ['Link Play', t('Local connection'), last];
}
