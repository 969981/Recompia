import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

// Link Play over pokopia-native's co-op protocol ([net] protocol = native, runtime/docs/CONFIG.md): Alone (mode = off), Automatic on the LAN
// (mode = auto), Visit by address (mode = join:<ip>[:port]; with a room code, a host that plays over the internet without a relay) or
// Internet (mode = auto, internet = true, relay = <ip>[:port], room_code). In auto mode one setting both hosts and visits: the game decides
// by Invite Others to Visit / Visit a Friend. The Internet host's code is the launcher's (room_code_new, kept in pkp.ini and localStorage);
// the running game's own is in room-code.txt (room_code_host). The old relay protocol ([net] protocol = linkplay) stays under Advanced.
const HOST_PORT = 47600;
const RELAY_PORT = 47610;
const LS = 'pkp.linkplay';

const stored = () => { try { return JSON.parse(localStorage.getItem(LS)) || {}; } catch { return {}; } };
const keep = (patch) => { try { localStorage.setItem(LS, JSON.stringify({ ...stored(), ...patch })); } catch { /* private window: fine */ } };

const truthy = (s) => /^(1|true|yes|on)$/i.test((s || '').trim());
const ipv4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(:\d{1,5})?$/;
const validAddr = (s) => {
  const m = ipv4.exec(s.trim());
  if (!m) return false;
  if ([m[1], m[2], m[3], m[4]].some((x) => +x > 255)) return false;
  return !m[5] || (+m[5].slice(1) > 0 && +m[5].slice(1) < 65536);
};
const net = (key, value) => ({ file: 'pkp', section: 'net', key, value });
const pkp = (key, value) => ({ file: 'pkp', section: 'pkp', key, value });

export async function render(el) {
  const data = await api('read_settings');
  const ini = (k, d = '') => (data.pkp[k] ?? d).trim();
  const mem = stored();
  let protocol = ini('net.protocol', 'native').toLowerCase() === 'linkplay' ? 'linkplay' : 'native';
  let modeKey = ini('net.mode', 'auto').toLowerCase();
  let internet = truthy(ini('net.internet'));
  let relayAddr = ini('net.relay');
  let code = ini('net.room_code');
  let server = ini('net.server', 'auto');
  let interact = truthy(ini('pkp.link_interact_everywhere'));
  let story = truthy(ini('pkp.link_story_coop'));

  const modeOf = () => (modeKey === 'off' || modeKey === '0' || !modeKey ? 'alone'
    : modeKey.startsWith('join:') ? 'address'
    : internet ? 'internet' : 'auto');
  let mode = modeOf();
  let picked = mode;
  let role = mem.role === 'visit' ? 'visit' : 'host';   // Internet: which code field to show (the setting itself is the same)
  let hostCode = mode === 'internet' && role === 'host' && code && code.toLowerCase() !== 'new' ? code : mem.hostCode || '';
  let visitCode = mode === 'internet' && role === 'visit' ? code : mem.visitCode || '';
  let joinAddr = mode === 'address' ? modeKey.slice(5) : mem.joinAddr || '';
  let joinCode = mode === 'address' && internet ? code : mem.joinCode || '';
  let relayIn = relayAddr || mem.relayAddr || '';
  let gameCode = null;   // room-code.txt of a running (or the last) game with internet play
  let relay = await api('relay_status');
  let lan = null;
  let advanced = protocol === 'linkplay';

  const write = (edits) => api('write_settings', { edits });
  async function loadLan() { try { lan = await api('lan_report'); } catch { lan = null; } }
  async function loadGameCode() { try { gameCode = await api('room_code_host'); } catch { gameCode = null; } }

  const copyRow = (addr, note) => `<div class="row wrap"><code>${esc(addr)}</code><span class="faint">${esc(note)}</span><button class="btn quiet" data-copy="${esc(addr)}">${icon('copy')}Copy</button></div>`;
  function lanRows(port) {
    if (!lan) return '';
    const rows = [];
    lan.lan.forEach((a, i) => rows.push(copyRow(`${a.ip}:${port}`, `${i === 0 ? 'same Wi-Fi or network' : 'other network'} (${a.adapter})`)));
    lan.overlay.forEach((a) => rows.push(copyRow(`${a.ip}:${port}`, `${a.label} (${a.adapter})`)));
    return rows.join('') || '<span class="faint">none found</span>';
  }
  function lanWarn() {
    if (!lan || !lan.warning) return '';
    const hint = lan.firewall_hint ? `<div class="dim">${esc(lan.firewall_hint)}</div>` : '';
    return `<div class="banner warn">${icon('triangle-alert')}<div>${esc(lan.warning)}${hint}</div></div>`;
  }

  function autoHtml() {
    return `
      <div class="card">
        <h2>${icon('wifi')}Automatic (same network)</h2>
        <p class="dim">The game hosts when you pick <b>Invite Others to Visit</b> (TCP and UDP port ${HOST_PORT}) and finds hosts on your network when you pick <b>Visit a Friend</b>.</p>
        ${lanWarn()}
        <div class="field"><label class="name">This PC's address<span class="hint">For visitors whose game does not find you: they pick Visit by address and type this.</span></label>
          <div class="stack">${lanRows(HOST_PORT)}</div></div>
      </div>`;
  }

  function addressHtml() {
    return `
      <div class="card">
        <h2>${icon('link')}Visit by address</h2>
        <p class="dim">Your game asks this host only. The host keeps Automatic (or Internet) and picks Invite Others to Visit; their Link Play page shows the address.</p>
        <div class="field"><label class="name">Host address<span class="hint">Like 192.168.1.20 or 192.168.1.20:${HOST_PORT}. The port is optional.</span></label>
          <div class="control"><input type="text" id="a-addr" spellcheck="false" placeholder="192.168.1.20" value="${esc(joinAddr)}"></div></div>
        <div class="field"><label class="name">Room code<span class="hint">Only for a host that plays over the internet without a relay (their router forwards port ${HOST_PORT}). Leave empty on a LAN.</span></label>
          <div class="control"><input type="text" id="a-code" spellcheck="false" placeholder="XXXX-XXXX-XXXX-XXXX" value="${esc(joinCode)}"></div></div>
        <div class="row" style="margin-top: var(--space-4)"><button class="btn primary" id="a-save">${icon('save')}Save</button>
          ${mode === 'address' ? `<span class="chip ok">${icon('circle-check')}Visiting ${esc(joinAddr)}</span>` : ''}</div>
      </div>`;
  }

  function internetHtml() {
    const shown = gameCode || hostCode;
    const running = !!relay;
    const relayPort = relay ? relay.addr.split(':').pop() : RELAY_PORT;
    const roleOpt = (id, title) => `<label class="candidate"><input type="radio" name="role" value="${id}" ${role === id ? 'checked' : ''}><div><b>${title}</b></div></label>`;
    return `
      <div class="card">
        <h2>${icon('globe')}Internet</h2>
        <p class="dim">Everyone goes through a relay (a small server: pokopia-native's coop-relay or this launcher's). Every link proves the room code; the traffic is not encrypted.</p>
        <div class="row wrap">${roleOpt('host', 'I host')}${roleOpt('visit', 'I visit')}</div>
        <div class="field"><label class="name">Relay address<span class="hint">IPv4 address, port ${RELAY_PORT} unless given.${role === 'host' ? ' Empty: no relay, visitors use Visit by address with your public address (forward TCP and UDP ' + HOST_PORT + ' on your router).' : ''}</span></label>
          <div class="control"><input type="text" id="i-relay" spellcheck="false" placeholder="203.0.113.5:${RELAY_PORT}" value="${esc(relayIn)}"></div></div>
        ${role === 'visit' ? `
        <div class="field"><label class="name">Room code<span class="hint">The host's code, XXXX-XXXX-XXXX-XXXX.</span></label>
          <div class="control"><input type="text" id="i-code" spellcheck="false" placeholder="XXXX-XXXX-XXXX-XXXX" value="${esc(visitCode)}"></div></div>` : `
        <div class="field"><label class="name">Room code<span class="hint">${gameCode ? 'The code your game uses (room-code.txt).' : 'Used from the next time the game starts.'} Give it to your visitors with the relay address.</span></label>
          <div class="control"><code>${esc(shown || 'none yet')}</code><button class="btn quiet" id="copy-code">${icon('copy')}Copy</button><button class="btn quiet" id="new-code">${icon('refresh-cw')}New code</button></div></div>
        <div class="field"><label class="name">Relay on this PC<span class="hint">Optional. The launcher runs a relay while it stays open; visitors need your public address with TCP and UDP ${RELAY_PORT} forwarded, or one of these.</span></label>
          <div class="row wrap">${running
            ? `<span class="chip ok">${icon('circle-check')}Relay on ${esc(relay.addr)}, ${relay.clients} links</span><button class="btn" id="relay-stop">Stop relay</button>`
            : `<button class="btn" id="relay-start">${icon('server')}Run a relay here</button>`}</div>
          ${running ? `<div class="stack">${lanRows(relayPort)}</div>` : ''}</div>`}
        <div class="row" style="margin-top: var(--space-4)"><button class="btn primary" id="i-save">${icon('save')}Save</button>
          ${mode === 'internet' ? `<span class="chip ok">${icon('circle-check')}Internet play${relayAddr ? ` through ${esc(relayAddr)}` : ''}</span>` : ''}</div>
      </div>`;
  }

  function advancedHtml() {
    return `
      <details class="card" ${advanced ? 'open' : ''} id="adv">
        <summary class="dim">Advanced</summary>
        <label class="candidate" style="margin-top: var(--space-3)"><input type="checkbox" id="old-proto" ${protocol === 'linkplay' ? 'checked' : ''}><div><b>Old Link Play protocol</b>
          <div class="dim">The relay protocol of earlier builds of this port ([net] protocol = linkplay). Only players with those builds can meet you; no cross-play with pokopia-native. The choices above do not apply then.</div></div></label>
        ${protocol === 'linkplay' ? `
        <div class="field"><label class="name">Relay server ([net] server)<span class="hint">auto: the game runs its own relay on port 39660 and finds others on the LAN; or address:port of an old relay.</span></label>
          <div class="control"><input type="text" id="old-server" spellcheck="false" value="${esc(server)}"><button class="btn quiet" id="old-save">${icon('save')}Save</button></div></div>` : ''}
      </details>`;
  }

  function draw() {
    const opt = (id, title, sub) => `<label class="candidate"><input type="radio" name="mode" value="${id}" ${picked === id ? 'checked' : ''} ${protocol === 'linkplay' ? 'disabled' : ''}><div><b>${title}</b><div class="dim">${sub}</div></div></label>`;
    const on = picked !== 'alone' || protocol === 'linkplay';
    el.innerHTML = `
      <div class="page">
        <div class="hero"><h1>Link Play</h1><p class="dim">Play with other people on one island, on the same network or over the internet. Cross-play with pokopia-native.</p></div>
        <div class="card">
          <h2>${icon('users')}How do you want to play?</h2>
          <div class="stack">
            ${opt('alone', 'Alone', 'No Link Play. Local wireless in the game finds nobody.')}
            ${opt('auto', 'Automatic (LAN)', 'Host or visit on the same Wi-Fi or network. Hosts are found by themselves.')}
            ${opt('address', 'Visit by address', "Join one host by its address, when it is not found by itself.")}
            ${opt('internet', 'Internet', 'Host or visit through a relay, with a room code.')}
          </div>
        </div>
        ${protocol === 'native' && picked === 'auto' ? autoHtml() : ''}
        ${protocol === 'native' && picked === 'address' ? addressHtml() : ''}
        ${protocol === 'native' && picked === 'internet' ? internetHtml() : ''}
        ${on ? `<div class="card">
          <label class="candidate"><input type="checkbox" id="interact" ${interact || story ? 'checked' : ''} ${story ? 'disabled' : ''}><div><b>Interact everywhere (experimental)</b>
            <div class="dim">Outside Palette Town the game puts everyone in Spectator Mode. This lets visitors use moves and items, pick up, place and build in the story areas too.</div></div></label>
          <label class="candidate"><input type="checkbox" id="story" ${story ? 'checked' : ''}><div><b>Story co-op (experimental)</b>
            <div class="dim">Visitors can talk to the story characters too. The host still does the story steps. Turns on Interact everywhere.</div></div></label>
          <div class="dim">Every player of a session needs the same two settings, or the host is not listed.</div>
        </div>` : ''}
        ${on ? `<div class="banner info">${icon('info')}<div>
          In the game, open Link Play and pick <b>Local connection</b> (Online connection is Nintendo's service). Cross-play works with pokopia-native players on game version 2.0.1 (not 1.0.5), not with earlier Link Play builds of this port. Each player needs their own save: a copied save cannot join its original. Changes apply the next time the game starts.</div></div>` : ''}
        ${on && protocol === 'native' && picked !== 'internet' ? `<div class="banner info">${icon('shield')}<div>
          If Windows Firewall asks about pkp.exe when you host, allow it on private networks (TCP and UDP ${HOST_PORT}). On Steam Deck or other Proton/Linux devices, finding hosts on the network may not work: pick Visit by address and type the host's address.</div></div>` : ''}
        ${advancedHtml()}
      </div>`;
    refreshIcons();
    bind();
  }

  // every native choice also writes protocol = native (leaves the old protocol)
  async function apply(edits, next) {
    await write([net('protocol', 'native'), ...edits]);
    protocol = 'native';
    if (next) next();
    mode = modeOf();
  }

  async function saveAddress() {
    const addr = el.querySelector('#a-addr').value.trim();
    const raw = el.querySelector('#a-code').value.trim();
    if (!validAddr(addr)) return toast('Type the host\'s IPv4 address, like 192.168.1.20 or 192.168.1.20:47600.', 'error');
    try {
      const c = raw ? await api('room_code_check', { code: raw }) : '';
      await apply([net('mode', `join:${addr}`), net('internet', c ? 'true' : 'false'), net('room_code', c), net('relay', '')], () => {
        modeKey = `join:${addr}`; internet = !!c; code = c; relayAddr = '';
      });
      joinAddr = addr; joinCode = c;
      keep({ joinAddr, joinCode });
      toast('Saved. Start the game and pick Visit a Friend.', 'ok');
    } catch (e) { toast(errText(e), 'error'); }
    draw();
  }

  async function saveInternet() {
    const r = el.querySelector('#i-relay').value.trim();
    if (r && !validAddr(r)) return toast('The relay address is an IPv4 address with an optional port, like 203.0.113.5:47610.', 'error');
    if (role === 'visit' && !r) return toast('Type the relay address the host gave you.', 'error');
    try {
      let c;
      if (role === 'visit') {
        const raw = el.querySelector('#i-code').value.trim();
        if (!raw) return toast('Type the host\'s room code.', 'error');
        c = await api('room_code_check', { code: raw });
        visitCode = c;
      } else {
        if (!hostCode) hostCode = await api('room_code_new');
        c = hostCode;
      }
      await apply([net('mode', 'auto'), net('internet', 'true'), net('relay', r), net('room_code', c)], () => {
        modeKey = 'auto'; internet = true; relayAddr = r; code = c;
      });
      relayIn = r;
      keep({ relayAddr: r, hostCode, visitCode, role });
      toast(role === 'host' ? 'Saved. Start the game and pick Invite Others to Visit.' : 'Saved. Start the game and pick Visit a Friend.', 'ok');
    } catch (e) { toast(errText(e), 'error'); }
    draw();
  }

  const copy = async (text) => {
    try { await navigator.clipboard.writeText(text); toast('Copied.', 'ok'); } catch { toast('Cannot copy here. Select the text and press Ctrl+C.', 'error'); }
  };

  function bind() {
    el.querySelectorAll('input[name="mode"]').forEach((r) => r.addEventListener('change', async () => {
      picked = r.value;
      try {
        if (picked === 'alone') {
          await apply([net('mode', 'off')], () => { modeKey = 'off'; });
          toast('Link Play is off.', 'ok');
        } else if (picked === 'auto') {
          await apply([net('mode', 'auto'), net('internet', 'false'), net('relay', ''), net('room_code', '')], () => {
            modeKey = 'auto'; internet = false; relayAddr = ''; code = '';
          });
          await loadLan();
          toast('Automatic Link Play on your network is on. It applies the next time the game starts.', 'ok');
        } else if (picked === 'internet') {
          if (!hostCode) hostCode = await api('room_code_new');
          await loadGameCode();
          if (relay) await loadLan();
        }
      } catch (e) { toast(errText(e), 'error'); }
      draw();
    }));
    el.querySelectorAll('input[name="role"]').forEach((r) => r.addEventListener('change', () => {
      relayIn = el.querySelector('#i-relay')?.value.trim() ?? relayIn;
      role = r.value; keep({ role }); draw();
    }));
    el.querySelector('#a-save')?.addEventListener('click', saveAddress);
    el.querySelector('#i-save')?.addEventListener('click', saveInternet);
    el.querySelectorAll('[data-copy]').forEach((b) => b.addEventListener('click', () => copy(b.dataset.copy)));
    el.querySelector('#copy-code')?.addEventListener('click', () => copy(gameCode || hostCode));
    el.querySelector('#new-code')?.addEventListener('click', async () => {
      try {
        hostCode = await api('room_code_new');
        keep({ hostCode });
        if (mode === 'internet' && role === 'host') { await write([net('room_code', hostCode)]); code = hostCode; }
        gameCode = null;
        toast('New room code. Tell your visitors; it applies the next time the game starts.', 'ok');
      } catch (e) { toast(errText(e), 'error'); }
      draw();
    });
    el.querySelector('#relay-start')?.addEventListener('click', async () => {
      try {
        relay = await api('relay_start', { port: RELAY_PORT });
        relayIn = `127.0.0.1:${relay.addr.split(':').pop()}`;
        await loadLan();
        toast('Relay running. Press Save to host through it.', 'ok');
      } catch (e) { toast(errText(e), 'error'); }
      draw();
    });
    el.querySelector('#relay-stop')?.addEventListener('click', async () => {
      try { await api('relay_stop'); relay = null; } catch (e) { toast(errText(e), 'error'); }
      draw();
    });
    el.querySelector('#interact')?.addEventListener('change', async (ev) => {
      const v = ev.target.checked;
      try {
        await write([pkp('link_interact_everywhere', v ? 'true' : 'false')]);
        interact = v;
        toast(v ? 'Interact everywhere is on. Every player needs it on; it applies the next time the game starts.' : 'Interact everywhere is off.', 'ok');
      } catch (e) { ev.target.checked = !v; toast(errText(e), 'error'); }
    });
    el.querySelector('#story')?.addEventListener('change', async (ev) => {
      const v = ev.target.checked;
      try {
        await write(v ? [pkp('link_story_coop', 'true'), pkp('link_interact_everywhere', 'true')] : [pkp('link_story_coop', 'false')]);
        story = v;
        if (v) interact = true;
        toast(v ? 'Story co-op is on (with Interact everywhere). Every player needs both on; it applies the next time the game starts.' : 'Story co-op is off.', 'ok');
      } catch (e) { ev.target.checked = !v; toast(errText(e), 'error'); }
      draw();
    });
    el.querySelector('#adv')?.addEventListener('toggle', (ev) => { advanced = ev.target.open; });
    el.querySelector('#old-proto')?.addEventListener('change', async (ev) => {
      const v = ev.target.checked;
      try {
        await write([net('protocol', v ? 'linkplay' : 'native')]);
        protocol = v ? 'linkplay' : 'native';
        advanced = true;
        toast(v ? 'Old Link Play protocol on. It applies the next time the game starts.' : 'Back to the pokopia-native protocol.', 'ok');
      } catch (e) { ev.target.checked = !v; toast(errText(e), 'error'); }
      draw();
    });
    el.querySelector('#old-save')?.addEventListener('click', async () => {
      const v = el.querySelector('#old-server').value.trim() || 'auto';
      try { await write([net('server', v)]); server = v; toast('Saved.', 'ok'); } catch (e) { toast(errText(e), 'error'); }
    });
  }

  if (picked === 'auto') await loadLan();
  if (picked === 'internet') { await loadGameCode(); if (relay) await loadLan(); }
  draw();
}
