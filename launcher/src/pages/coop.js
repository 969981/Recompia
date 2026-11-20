// Recompia: Co-op's Home (games.js). A co-op save is played together and kept apart from the singleplayer saves (src-tauri/src/coop*.rs).
// The creator names every player when making it and sends the invite code (a short code the relay keeps, or the long one); the others
// enter it under Join and say which player they are. Each card says who hosts the next start and why (coop-model.js), lets the player override it for one start, and starts the
// game; the host starts first. After a start the game's log is read for a host hidden by another build or other mods (coop_diag.rs).
// Co-op plays the install Recompia (Solo) sets up: until it is ready, this page says so and leads there.
// A split screen save is the third way in: all its players play on this PC, one window each (coop_launch.rs build_split). Its card has
// the windows' choices (screen, controllers, sound; kept in this launcher, the same for every such save) and no invite or Start as.
import './coop.css';
import { api } from '../api.js';
import { t } from '../i18n.js';
import { PUBLIC_RELAY } from '../linkplay-model.js';
import { formatDate } from '../markdown.js';
import { MAX_PLAYERS, START_AS, askOnce, choosePad, creatorName, hosting, isSplit, mismatchText, nameOf, needsPlayers, others, roleLine, runningLine, windowPads } from '../coop-model.js';
import { askText, confirm, errCode, errText, esc, formDialog, icon, segmented, select, toast, toggle } from '../ui.js';
import { store } from '../store.js';
import { saveLogs } from '../log-bundle.js';
import { gamePhase } from '../action.js';
import { brandImg } from '../branding.js';

const busy = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));
/** The install is set up and current (readiness.rs): anything else is put right in Solo (Set up, Update, game files). */
const ready = (s) => s.app?.readiness?.kind === 'ready';
const MAX_NAME = 24;
const INVITE_LENGTH = 4000;
const now = () => Math.floor(Date.now() / 1000);

// kept while the launcher runs, across visits to the page: the start choice of each save (back to Auto when its game ends), the save
// whose game runs, and what its last start found
const startAs = new Map();
let playing = null;   // { id, at, seen }: seen once the game was reported busy, so an event before the start lands is not its end
const findings = new Map();
let redraw = () => {};   // the page on screen, if any
const LOG_POLL_MS = 5000;   // while it runs: a host hidden by other mods is said while the visitor still waits in the game
let poll = null;

// split screen's choices (coop_launch.rs SplitOptions): how two or three windows share the screen, what each window plays with (by
// window number, coop-model.js windowPads), and whether the other windows play sound
const SPLIT_LS = 'pkp.coop.split';
const split = (() => {
  try {
    const m = JSON.parse(localStorage.getItem(SPLIT_LS)) || {};
    return { side: !!m.side, pads: Array.isArray(m.pads) ? m.pads : [], sound: !!m.sound };
  } catch { return { side: false, pads: [], sound: false }; }
})();
const keepSplit = () => { try { localStorage.setItem(SPLIT_LS, JSON.stringify(split)); } catch { /* private window: kept for this run */ } };

function readLog(id, at) {
  return api.coopLastRun({ since: at }).then((f) => {
    if (f) findings.set(id, f); else findings.delete(id);
    redraw();
  }).catch((e) => console.warn('coop_last_run', e));
}

store.subscribe((s) => {
  if (!playing) return;
  if (busy(s)) {
    playing.seen = true;
    if (!poll) { const { id, at } = playing; poll = setInterval(() => readLog(id, at), LOG_POLL_MS); }
    return;
  }
  if (!playing.seen) return;
  const { id, at } = playing;
  playing = null;
  clearInterval(poll);
  poll = null;
  startAs.delete(id);
  readLog(id, at);
});

// co-op is new and uses a save of its own: said on the page whatever it shows, since players expect their current world to come along
const experimental = () => `<div class="banner info coop-banner">${icon('info')}<div class="grow"><strong>${esc(t('Co-op is experimental for now.'))}</strong><span>${esc(t('It needs its own save that starts from the beginning: your current save cannot be played in co-op, and co-op never uses or changes it.'))}</span></div></div>`;

const field = (id, label, opts = {}) => `<label class="form-field"><span>${esc(label)}${opts.hint ? ` <span class="hint">${esc(opts.hint)}</span>` : ''}</span>
  <input class="input" id="${id}" type="text" maxlength="${MAX_NAME}" autocomplete="off" spellcheck="false" placeholder="${esc(opts.placeholder ?? '')}" value="${esc(opts.value ?? '')}" /></label>`;

function steps() {
  const step = (n, title, text) => `<li><span class="coop-step-n">${n}</span><div><strong>${esc(title)}</strong><span>${esc(text)}</span></div></li>`;
  return `<ol class="coop-steps">
    ${step(1, t('Make a co-op save'), t('One player makes it and types in everyone who plays.'))}
    ${step(2, t('Send the invite code'), t('The others press Join and paste it. It also holds the private room, so send it only to them.'))}
    ${step(3, t('Host first, then the others'), t('The card says who hosts. The host presses Play first; the others once the host is in the game.'))}
  </ol>`;
}

export async function render(el, ctx) {
  let saves = await api.coopList();
  let tail = Promise.resolve();
  const inOrder = (fn) => { tail = tail.then(fn).catch((e) => toast(errText(e), 'error')); return tail; };

  function playersHtml(v, choice) {
    const p = v.profile;
    const hostId = isSplit(v) ? '' : choice === 'host' ? p.me : choice === 'visit' ? '' : v.host.host;
    const chips = p.members.map((m) => {
      const mine = m.id === p.me;
      const remove = v.creator && !mine && m.id !== p.members[0].id
        ? `<button class="chip-x" data-act="remove" data-id="${esc(p.id)}" data-member="${esc(m.id)}" aria-label="${esc(t('Remove {name}', { name: m.name }))}" title="${esc(t('Remove {name}', { name: m.name }))}">${icon('x')}</button>` : '';
      const host = m.id === hostId ? `<span class="coop-host-mark" title="${esc(t('Hosts'))}">${icon('crown')}</span>` : '';
      const tag = mine && !isSplit(v) ? `<span class="coop-you">${esc(t('You'))}</span>` : '';   // a split save: the windows list below says who plays where
      return `<span class="chip coop-player${mine || isSplit(v) ? ' on' : ''}">${host}${esc(m.name)}${tag ? ` ${tag}` : ''}${remove}</span>`;
    }).join('');
    const add = v.creator && p.members.length < MAX_PLAYERS
      ? `<button class="chip coop-add" data-act="add" data-id="${esc(p.id)}">${icon('plus')}${esc(t('Add a player'))}</button>` : '';
    return `<div class="coop-players">${chips}${add}</div>`;
  }

  // a split screen save's windows: how they share the screen, who plays with what, the sound, and what it costs
  function splitHtml(v) {
    const n = v.profile.members.length;
    if (!isSplit(v) || n < 2) return '';
    const id = v.profile.id;   // one set of controls per card: radio names and ids stay unique on the page
    const off = busy(store.state);
    const opt = (label, control) => `<div class="coop-split-opt"><span class="coop-role-label">${esc(label)}</span>${control}</div>`;
    const inputs = [...[1, 2, 3, 4].map((k) => [k, t('Controller {n}', { n: k })]), [0, t('Keyboard and mouse')]];
    const pads = windowPads(n, split.pads);
    const players = v.profile.members.map((m, i) => `<label class="coop-split-pad"><span class="coop-split-who">${icon('monitor')}${i + 1}<span class="clip">${esc(m.name)}</span></span>${
      select(`split-pad-${id}-${i}`, inputs, pads[i], { label: t('{name} plays with', { name: m.name }), disabled: off })}</label>`).join('');
    return `<div class="coop-split">
      <div class="coop-split-pads">${players}</div>
      <div class="coop-split-opts">
        ${n === 4 ? '' : opt(t('Screen'), segmented(`split-layout-${id}`, [['stacked', t('Top and bottom')], ['side', t('Side by side')]], split.side ? 'side' : 'stacked', { label: t('Screen'), disabled: off }))}
        <label class="coop-split-opt">${toggle(`split-sound-${id}`, split.sound, { disabled: off })}<span class="coop-role-label">${esc(t('Sound from the other windows'))}</span></label>
      </div>
      <p class="coop-split-note">${esc(t('Controllers count in the order they were connected. The keyboard and mouse play in the window you click.'))} ${esc(t('Each window is a game of its own and needs about 7 GB of memory: 32 GB is recommended for two players, 64 GB for four.'))}</p>
    </div>`;
  }

  function saveHtml(v) {
    const p = v.profile;
    const choice = startAs.get(p.id) ?? 'auto';
    const running = playing?.id === p.id && busy(store.state);
    const meta = [
      isSplit(v) ? t('Split screen on this PC') : v.creator ? t('Made by you') : t('Made by {name}', { name: creatorName(v) }),
      v.saved_at ? t('Last played {date}', { date: formatDate(new Date(v.saved_at * 1000).toISOString()) }) : t('Not played yet'),
    ].join(' · ');
    const f = findings.get(p.id);
    const finding = f ? (() => {
      const m = mismatchText(f);
      return `<div class="banner danger coop-banner">${icon('circle-alert')}<div class="grow"><strong>${esc(m.title)}</strong><span>${esc(m.body)}</span>${
        f.own_mods ? `<span class="faint">${esc(t('Your gameplay mods: {mods}', { mods: f.own_mods }))}</span>` : ''}</div>
        <div class="cluster">${f.mismatch === 'mods' ? `<button class="btn sm" data-act="mods" data-id="${esc(p.id)}">${icon('puzzle')}${esc(t('Open Mods'))}</button>` : ''}
        <button class="btn quiet sm icon-only" data-act="dismiss" data-id="${esc(p.id)}" aria-label="${esc(t('Dismiss'))}" title="${esc(t('Dismiss'))}">${icon('x')}</button></div></div>`;
    })() : '';
    const lonely = needsPlayers(v)
      ? `<div class="banner warn coop-banner">${icon('triangle-alert')}<div class="grow">${esc(isSplit(v) ? t('Add a second player to play split screen.') : t('Add the other players by name, then send them the invite code. Co-op needs at least two players.'))}</div></div>` : '';
    const playOff = busy(store.state) || !ready(store.state) || needsPlayers(v);
    const status = running
      ? `<p class="coop-role-line ok">${icon('loader-circle', 'spin')}<span>${esc(runningLine(v, choice))}</span></p>`
      : `<p class="coop-role-line">${esc(roleLine(v, choice))}</p>`;
    return `<article class="coop-save glass" data-save="${esc(p.id)}">
      <div class="coop-save-head">
        <div class="grow"><h2 class="clip">${esc(p.name)}</h2><p class="coop-meta">${esc(meta)}</p></div>
        <button class="btn quiet sm icon-only" data-act="delete" data-id="${esc(p.id)}" ${busy(store.state) ? 'disabled' : ''} aria-label="${esc(t('Delete {name} from this PC', { name: p.name }))}" title="${esc(t('Delete {name} from this PC', { name: p.name }))}">${icon('trash-2')}</button>
      </div>
      ${playersHtml(v, choice)}
      ${lonely}${finding}${splitHtml(v)}
      <div class="coop-start-row">
        <div class="coop-role">
          ${isSplit(v) ? '' : `<div class="coop-role-pick"><span class="coop-role-label">${esc(t('Start as'))}</span>${segmented(`start-${p.id}`, [['auto', t('Auto')], ['host', t('Host')], ['visit', t('Join')]], choice, { label: t('Start as'), disabled: running || needsPlayers(v) })}</div>`}
          ${needsPlayers(v) ? '' : status}
        </div>
        <div class="coop-buttons">
          ${v.creator && !isSplit(v) ? `<button class="btn" data-act="invite" data-id="${esc(p.id)}" ${needsPlayers(v) ? 'disabled' : ''}>${icon('send')}${esc(t('Invite code'))}</button>` : ''}
          <button class="btn primary" data-act="play" data-hero-play data-id="${esc(p.id)}" ${playOff ? 'disabled' : ''}>${icon(running ? 'loader-circle' : 'play', running ? 'spin' : '')}${esc(running ? t('Playing') : isSplit(v) ? t('Play split screen') : hosting(v, choice) ? t('Play as host') : t('Play and join'))}</button>
        </div>
      </div>
    </article>`;
  }

  const emptyHtml = () => `<div class="coop-empty">
      <div class="coop-choices">
        <button class="coop-choice glass hover" data-act="create">${icon('plus')}<strong>${esc(t('Start a co-op save'))}</strong><span>${esc(t('You host it. Type in your friends\' names and send them the invite code.'))}</span></button>
        <button class="coop-choice glass hover" data-act="join">${icon('log-in')}<strong>${esc(t('Join a friend'))}</strong><span>${esc(t('Paste the invite code a friend sent you.'))}</span></button>
        <button class="coop-choice glass hover" data-act="split">${icon('monitor')}<strong>${esc(t('Split screen on this PC'))}</strong><span>${esc(t('2 to 4 players on this PC, each in a window of their own. No internet needed.'))}</span></button>
      </div>
      <h2 class="group-title">${esc(t('How co-op works'))}</h2>
      ${steps()}
    </div>`;

  let lastHtml = '';
  function draw() {
    const s = store.state;
    const html = (!s.app || ready(s) ? ''
      : `<div class="banner warn">${icon('triangle-alert')}<div class="grow">${esc(t('Co-op plays the game you set up under Solo, and it is not ready yet.'))}</div>`
        + `<button class="btn sm" data-act="recompia">${esc(t('Go to Solo'))}</button></div>`)
      + experimental()
      + (saves.length ? saves.map(saveHtml).join('') + `<details class="coop-how"><summary>${icon('chevron-right')}${esc(t('How co-op works'))}</summary>${steps()}</details>` : emptyHtml());
    if (html === lastHtml) return;   // store events come often: redraw only what changed, so focus and the open details stay
    const open = el.querySelector('.coop-how')?.open;
    lastHtml = html;
    el.querySelector('#coop-body').innerHTML = html;
    if (open) el.querySelector('.coop-how').open = true;
    // with no save yet the Start and Join cards below are the way in; the row (and its logs button) waits for a first save
    el.querySelector('#coop-actions').hidden = !saves.length;
  }

  el.innerHTML = `<div class="page coop">
    <section class="coop-hero">
      <h1 class="hero-title">${brandImg('logo', `class="coop-logo" alt="${esc(t('Recompia: Co-op'))}"`, 'coop')}</h1>
      <p class="coop-lead">${esc(t('Up to 4 players, on their own PCs or together on one'))}</p>
      <div class="cluster coop-hero-actions" id="coop-actions">
        <button class="btn primary big" data-act="create">${icon('plus')}${esc(t('New co-op save'))}</button>
        <button class="btn big on-art" data-act="join">${icon('log-in')}${esc(t('Join'))}</button>
        <button class="btn big on-art" data-act="split">${icon('monitor')}${esc(t('Split screen'))}</button>
        <button class="btn big on-art icon-only" data-act="save-logs" aria-label="${esc(t('Save logs for a bug report'))}" title="${esc(t('Logs, settings and co-op save info in one zip. Room codes and invites are left out.'))}">${icon('file-archive')}</button>
      </div>
    </section>
    <div class="coop-body" id="coop-body"></div>
  </div>`;
  draw();

  // ---- the invite code -------------------------------------------------------------------------------------------------------------
  // The short code (coop_code.rs) when the public relay keeps one, and the long code always: it also works on another relay and after 6 hours.
  async function showInvite(id) {
    const v = saves.find((x) => x.profile.id === id);
    const { long, short } = await api.coopInvite({ id, relay: PUBLIC_RELAY });
    const who = others(v).join(', ');
    const copy = async (code) => {
      try {
        await navigator.clipboard.writeText(code);
        toast(t('Invite code copied'), 'ok');
      } catch (e) { toast(errText(e), 'error'); }
    };
    const longBox = `<textarea class="input" readonly rows="4" aria-label="${esc(t('Invite code'))}">${esc(long)}</textarea>`;
    const html = short
      ? `<p>${esc(t('Send this code to {names}. They press Join in Co-op and type it in.', { names: who }))}</p>
        <output class="coop-short-code" aria-label="${esc(t('Invite code'))}">${esc(short)}</output>
        <p class="faint">${esc(t('It works for 6 hours. After that, or if your friends use another relay, send the long code instead.'))}</p>
        <details class="coop-how coop-long"><summary>${icon('chevron-right')}${esc(t('Long code'))}</summary>${longBox}
          <button class="btn" data-act="copy-long">${icon('copy')}${esc(t('Copy long code'))}</button></details>`
      : `<p>${esc(t('Send this code to {names}. They press Join in Co-op and paste it.', { names: who }))}</p>${longBox}`;
    const ok = await confirm({
      title: t('Invite code for {name}', { name: v.profile.name }),
      html: `${html}<p class="faint">${esc(t('It opens this save\'s private room, so send it only to these players. If you add a player later, send the new code to everyone.'))}</p>`,
      ok: t('Copy code'), cancel: t('Close'),
      onAct: (b) => { if (b.dataset.act === 'copy-long') copy(long); },
    });
    if (ok) copy(short ?? long);
  }

  // ---- create ------------------------------------------------------------------------------------------------------------------------
  const create = () => inOrder(async () => {
    const extra = [2, 3, 4].map((n) => field(`coop-p${n}`, t('Player {n}', { n }), { placeholder: n === 2 ? t('Name of a friend') : t('Optional') })).join('');
    const form = await formDialog({
      title: t('New co-op save'), ok: t('Create'), wide: true,
      html: `<p class="faint">${esc(t('This makes a new save that starts from the beginning. Your current save is not used or changed.'))}</p>
        <div class="form-grid">${field('coop-name', t('Name of the save'), { placeholder: t('For example: Our island') })}
        ${field('coop-me', t('Your name'), { hint: t('(you host first)') })}</div>
        <p class="faint">${esc(t('Who else plays? Type each name once; each friend picks theirs when they join.'))}</p><div class="form-grid">${extra}</div>`,
      check: (f) => {
        const val = (id) => f.querySelector(`#${id}`).value.trim();
        if (!val('coop-name') || !val('coop-me') || !val('coop-p2')) return '';
        const names = [val('coop-me'), val('coop-p2'), val('coop-p3'), val('coop-p4')].filter(Boolean).map((n) => n.toLowerCase());
        return new Set(names).size === names.length ? true : t('Every player needs a different name.');
      },
      read: (f) => ['coop-name', 'coop-me', 'coop-p2', 'coop-p3', 'coop-p4'].map((id) => f.querySelector(`#${id}`).value.trim()),
    });
    if (!form) return;
    const [name, mine, ...friends] = form;
    const before = new Set(saves.map((v) => v.profile.id));
    try {
      saves = await api.coopCreate({ name, myName: mine, relay: PUBLIC_RELAY });
      const made = saves.find((v) => !before.has(v.profile.id));   // by id: Rust may tidy the name
      for (const friend of friends.filter(Boolean)) saves = await api.coopAddMember({ id: made.profile.id, name: friend });
      draw();
      await showInvite(made.profile.id);
    } finally {
      draw();   // a player that could not be added still leaves the save on screen
    }
  });

  // ---- split screen -------------------------------------------------------------------------------------------------------------
  // A save of its own whose players all play here: no invite, the players are named once and play in the windows in that order.
  const createSplit = () => inOrder(async () => {
    const players = [1, 2, 3, 4].map((n) => field(`split-p${n}`, t('Player {n}', { n }), { placeholder: n > 2 ? t('Optional') : '' })).join('');
    const ids = ['split-name', 'split-p1', 'split-p2', 'split-p3', 'split-p4'];
    const form = await formDialog({
      title: t('Split screen on this PC'), ok: t('Create'), wide: true,
      html: `<p class="faint">${esc(t('This makes a new save that starts from the beginning. Your current save is not used or changed.'))}</p>
        <div class="form-grid">${field('split-name', t('Name of the save'), { placeholder: t('For example: Our island') })}</div>
        <p class="faint">${esc(t('Who plays? Each player gets a window of their own, in this order, and makes their own character.'))}</p><div class="form-grid">${players}</div>`,
      check: (f) => {
        const [name, ...names] = ids.map((id) => f.querySelector(`#${id}`).value.trim());
        if (!name || !names[0] || !names[1]) return '';
        const given = names.filter(Boolean).map((n) => n.toLowerCase());
        return new Set(given).size === given.length ? true : t('Every player needs a different name.');
      },
      read: (f) => ids.map((id) => f.querySelector(`#${id}`).value.trim()),
    });
    if (!form) return;
    const [name, ...players2] = form;
    saves = await api.coopCreateSplit({ name, players: players2.filter(Boolean) });
    draw();
  });

  const addPlayer = (id) => inOrder(async () => {
    const local = isSplit(saves.find((x) => x.profile.id === id));
    const name = await askText({ title: t('Add a player'), body: local ? t('They get the next window on this PC.') : t('Then send the new invite code to everyone, so every PC lists the same players.'), label: t('Name of the new player'), ok: t('Add'), max: MAX_NAME });
    if (!name) return;
    saves = await api.coopAddMember({ id, name });
    draw();
    if (!local) await showInvite(id);
  });

  const removePlayer = (id, member) => inOrder(async () => {
    const v = saves.find((x) => x.profile.id === id);
    const ok = await confirm({ title: t('Remove {name}?', { name: nameOf(v, member) }), body: isSplit(v) ? '' : t('Then send the new invite code to the others, so every PC lists the same players.'), ok: t('Remove'), danger: true });
    if (!ok) return;
    saves = await api.coopRemoveMember({ id, member });
    draw();
  });

  // ---- join --------------------------------------------------------------------------------------------------------------------------
  const join = () => inOrder(async () => {
    let parsed = null;
    // the long invite of what is typed: a short code is looked up once per text (coop_code.rs; the relay limits lookups), other text comes
    // back as it is; a failed lookup is asked again on the next check, except "no invite has this code". `found` keeps the answers for read()
    const lookup = askOnce((text) => api.coopResolveInvite({ text, relay: PUBLIC_RELAY }), (e) => errCode(e) === 'invite_not_found');
    const found = new Map();
    const resolve = (text) => lookup(text).then((long) => { found.set(text, long); return { long }; }, (e) => ({ error: errText(e) }));
    const typed = (f) => f.querySelector('#coop-code').value.trim();
    const form = await formDialog({
      title: t('Join a co-op save'), ok: t('Join'), wide: true,
      html: `<p class="faint">${esc(t('A co-op save is a save of its own that starts from the beginning. Your current save is not used or changed.'))}</p>
        <label class="form-field">${esc(t('Invite code'))} <span class="hint">${esc(t('The friend who made the save sends it to you: the short code or the long one, both work.'))}</span>
          <textarea class="input" id="coop-code" rows="4" maxlength="${INVITE_LENGTH}" spellcheck="false" placeholder="K7Q-M2X · PKCOOP1..."></textarea></label>
        <div id="coop-who"></div>`,
      check: async (f) => {
        const text = typed(f);
        const who = f.querySelector('#coop-who');
        const none = (why) => { parsed = null; who.innerHTML = ''; return why; };
        if (!text) return none('');
        const r = await resolve(text);
        if (r.error) return none(r.error);
        const inv = await api.coopInviteMembers({ invite: r.long }).catch(() => null);
        if (!inv) return none(t('This is not an invite code. Type the short code (like K7Q-M2X), or copy the whole long code, starting with PKCOOP1.'));
        const have = saves.find((v) => v.profile.id === inv.id);
        if (parsed?.id !== inv.id) {
          parsed = inv;
          const choices = inv.members.slice(1);
          who.innerHTML = have
            ? `<p class="coop-note">${icon('refresh-cw')}${esc(t('You have this save already. Joining again brings its player list up to date.'))}</p>`
            : choices.length ? `<fieldset class="coop-who"><legend>${esc(t('Which player are you?'))}</legend>${choices.map((m) => `<label class="backup-choice"><input type="radio" name="coop-who" value="${esc(m.id)}" /> <span>${esc(m.name)}</span></label>`).join('')}</fieldset>` : '';
        }
        if (have) return have.creator ? t('You made this save. Send the code to the others instead.') : true;
        if (inv.members.length < 2) return t('{name} has not added any players yet. Ask them to add you and send a new code.', { name: inv.members[0]?.name ?? '' });
        return f.querySelector('input[name="coop-who"]:checked') ? true : '';
      },
      read: (f) => ({ invite: found.get(typed(f)) ?? typed(f), member: f.querySelector('input[name="coop-who"]:checked')?.value ?? '' }),
    });
    if (!form) return;
    saves = await api.coopAccept(form);
    draw();
    toast(t('Joined. Press Play once the host is in the game.'), 'ok');
  });

  // ---- delete and play ---------------------------------------------------------------------------------------------------------------
  const remove = (id) => inOrder(async () => {
    const v = saves.find((x) => x.profile.id === id);
    const rest = isSplit(v) ? [] : others(v);
    const ok = await confirm({
      title: t('Delete {name} from this PC?', { name: v.profile.name }),
      body: rest.length ? t('This PC\'s copy of the world and its mod choices are deleted for good. The other players ({names}) keep their own copies.', { names: rest.join(', ') })
        : t('This PC\'s copy of the world and its mod choices are deleted for good.'),
      ok: t('Delete'), danger: true,
    });
    if (!ok) return;
    saves = await api.coopDelete({ id, relay: PUBLIC_RELAY });
    findings.delete(id);
    startAs.delete(id);
    draw();
    toast(t('Deleted {name}', { name: v.profile.name }), 'ok');
  });

  const play = (id) => inOrder(async () => {
    findings.delete(id);
    playing = { id, at: now(), seen: false };
    try {
      const v = saves.find((x) => x.profile.id === id);
      await api.coopPlay({ id, startAs: startAs.get(id) ?? 'auto', split: { side: split.side, sound: split.sound, pads: windowPads(v.profile.members.length, split.pads) } });
    } catch (e) {
      playing = null;
      throw e;
    }
    draw();
  });

  el.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]');
    if (!b || b.disabled) return;
    const { act, id, member } = b.dataset;
    if (act === 'create') create();
    else if (act === 'join') join();
    else if (act === 'split') createSplit();
    else if (act === 'save-logs') saveLogs();
    else if (act === 'add') addPlayer(id);
    else if (act === 'remove') removePlayer(id, member);
    else if (act === 'invite') inOrder(() => showInvite(id));
    else if (act === 'delete') remove(id);
    else if (act === 'play') play(id);
    else if (act === 'dismiss') { findings.delete(id); draw(); }
    else if (act === 'mods') ctx.go('mods', { scope: id });
    else if (act === 'recompia') ctx.go('home');   // Solo's Home: Set up, Update or the game files, whichever is needed
  });
  el.addEventListener('change', (e) => {
    const what = /^split-(layout|pad|sound)-/.exec(e.target.name || e.target.id)?.[1];
    if (what) {
      if (what === 'layout') split.side = e.target.value === 'side';
      else if (what === 'sound') split.sound = e.target.checked;
      else {
        const [, sid, i] = /^split-pad-(.+)-(\d)$/.exec(e.target.id);
        const n = saves.find((x) => x.profile.id === sid).profile.members.length;
        split.pads = choosePad(n, split.pads, Number(i), Number(e.target.value));
      }
      keepSplit();
      draw();
      return;
    }
    if (!e.target.name?.startsWith('start-') || !START_AS.includes(e.target.value)) return;
    const id = e.target.name.slice(6);
    if (e.target.value === 'auto') startAs.delete(id); else startAs.set(id, e.target.value);
    draw();
  });

  ctx.on(() => draw());
  redraw = draw;
  return () => { if (redraw === draw) redraw = () => {}; };
}
