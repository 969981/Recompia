// Co-op saves: a save played together, kept apart from the singleplayer saves (launcher/src-tauri/src/coop*.rs). The creator names each
// player, copies the invite and sends it to them; a player pastes it and says which of the named players they are. Play starts the game
// as that player, in the save's private room; the player whose copy of the save is the newest hosts.
import { api } from '../api.js';
import { t } from '../i18n.js';
import { PUBLIC_RELAY } from '../linkplay-model.js';
import { askText, confirm, errText, esc, group, icon, row, toast } from '../ui.js';
import { store } from '../store.js';
import { gamePhase } from '../action.js';

const busy = (s) => ['starting', 'running', 'finishing'].includes(gamePhase(s.game));
const MAX_NAME = 24;
const INVITE_LENGTH = 2000;

export async function render(el, ctx) {
  let saves = await api.coopList();
  // commits run one at a time, in the order the player made them
  let tail = Promise.resolve();
  const inOrder = (fn) => { tail = tail.then(fn).catch((e) => toast(errText(e), 'error')); return tail; };

  const players = (s) => s.profile.members
    .map((m) => `<span class="chip${m.id === s.profile.me ? ' on' : ''}">${esc(m.id === s.profile.me ? `${m.name} (${t('You')})` : m.name)}`
      + `${m.id === s.profile.me || m.id === s.profile.members[0].id ? '' : `<button class="btn quiet sm" data-act="remove" data-id="${esc(s.profile.id)}" data-member="${esc(m.id)}" aria-label="${esc(t('Remove {name}', { name: m.name }))}" title="${esc(t('Remove {name}', { name: m.name }))}">${icon('x')}</button>`}</span>`)
    .join(' ');

  const hostLine = (s) => (s.host.role === 'host'
    ? t('Your copy of the save is the newest, so you host the next session.')
    : t('{name} has the newest copy of the save and hosts the next session.', { name: s.profile.members.find((m) => m.id === s.host.host)?.name ?? '' }));

  const saveHtml = (s) => group(s.profile.name, [
    row(t('Players'), players(s)),
    row(hostLine(s), `<button class="btn quiet sm" data-act="add" data-id="${esc(s.profile.id)}">${icon('plus')}${esc(t('Add a player'))}</button>`
      + `<button class="btn quiet sm" data-act="invite" data-id="${esc(s.profile.id)}">${icon('copy')}${esc(t('Copy invite'))}</button>`
      + `<button class="btn primary sm" data-act="play" data-id="${esc(s.profile.id)}"${busy(store.state) ? ' disabled' : ''}>${icon('play')}${esc(t('Play'))}</button>`),
  ]);

  function draw() {
    el.querySelector('#coop-list').innerHTML = saves.length ? saves.map(saveHtml).join('') : `<p class="faint">${esc(t('No co-op saves yet.'))}</p>`;
  }

  el.innerHTML = `<div class="page coop">
    <div class="lp-head">
      <h1>${esc(t('Co-op'))}</h1>
      <div>
        <button class="btn primary" data-act="create">${icon('plus')}${esc(t('New co-op save'))}</button>
        <button class="btn" data-act="join">${esc(t('Join with an invite'))}</button>
      </div>
    </div>
    <p class="faint">${esc(t('A co-op save is played together and kept apart from your singleplayer saves. Players join with an invite.'))}</p>
    <div id="coop-list"></div>
  </div>`;
  draw();

  const create = () => inOrder(async () => {
    const name = await askText({ title: t('New co-op save'), label: t('Name of the co-op save'), ok: t('Create'), max: MAX_NAME });
    if (!name) return;
    const mine = await askText({ title: t('New co-op save'), label: t('Your name in this co-op save'), ok: t('Create'), max: MAX_NAME });
    if (!mine) return;
    saves = await api.coopCreate({ name, myName: mine, relay: PUBLIC_RELAY });
    draw();
  });

  const addPlayer = (id) => inOrder(async () => {
    const name = await askText({ title: t('Add a player'), label: t('Name of the new player'), ok: t('Add a player'), max: MAX_NAME });
    if (!name) return;
    saves = await api.coopAddMember({ id, name });
    draw();
  });

  const removePlayer = (id, member) => inOrder(async () => {
    saves = await api.coopRemoveMember({ id, member });
    draw();
  });

  async function copyInvite(id) {
    try {
      await navigator.clipboard.writeText(await api.coopInvite({ id }));
      toast(t('Invite copied. Send it only to the players: it holds the key to your private room.'), 'ok');
    } catch (e) { toast(errText(e), 'error'); }
  }

  // which of the named players the person joining is: the creator's place is never offered. The dialog is gone when confirm() resolves,
  // so the choice is taken as the radios are pressed
  function whoAmI(members) {
    const options = members.slice(1);
    if (options.length === 1) return Promise.resolve(options[0].id);
    let chosen = options[0].id;
    const html = options.map((m, i) => `<label class="backup-choice"><input type="radio" name="coop-who" value="${esc(m.id)}" data-act="who"${i === 0 ? ' checked' : ''} /> <span>${esc(m.name)}</span></label>`).join('');
    return confirm({ title: t('Join with an invite'), html: `<div>${html}</div>`, ok: t('Join'), onAct: (input) => { chosen = input.value; } })
      .then((ok) => (ok ? chosen : null));
  }

  const join = () => inOrder(async () => {
    const invite = await askText({ title: t('Join with an invite'), label: t('Paste the invite'), ok: t('Join'), max: INVITE_LENGTH });
    if (!invite) return;
    const members = await api.coopInviteMembers({ invite });
    if (members.length < 2) {
      await api.coopAccept({ invite, member: '' });   // the reasons are the backend's: not an invite, or no player to be
      return;
    }
    const member = await whoAmI(members);
    if (!member) return;
    saves = await api.coopAccept({ invite, member });
    draw();
  });

  el.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]');
    if (!b) return;
    const { act, id, member } = b.dataset;
    if (act === 'create') create();
    else if (act === 'join') join();
    else if (act === 'add') addPlayer(id);
    else if (act === 'remove') removePlayer(id, member);
    else if (act === 'invite') copyInvite(id);
    else if (act === 'play') inOrder(() => api.coopPlay({ id }));
  });

  ctx.on(() => draw());
}
