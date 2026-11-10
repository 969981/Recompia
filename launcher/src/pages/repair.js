// Repair and reset: the dialogs for repair.rs. Step 1 picks what to fix, step 2 shows exactly which files change (nothing is touched
// before that), step 3 does it. The Rust side keeps a copy of the changed settings files, so Undo is one button in Settings > About.
import { api } from '../api.js';
import { t } from '../i18n.js';
import { fromBackend } from '../backend-text.js';
import { confirm, errText, esc, icon, toast } from '../ui.js';
import { GAME_GROUPS, canReview, groupLabels, groupsPayload } from '../repair-model.js';

const CHOICES = [
  { id: 'game_settings', label: t('Reset game settings'), note: t('Graphics, performance, network and timing choices (pkp.ini) go back to the defaults. Your game files, profile and shader folders are kept.') },
  { id: 'controls', label: t('Reset controls'), note: t('The game makes its default key and pad bindings again the next time it starts.') },
  { id: 'launcher_prefs', label: t('Reset launcher settings'), note: t('Launcher options go back to the defaults. The game folder, the game files folder and the update channel are kept.') },
  { id: 'caches', label: t('Clear caches'), note: t('Cache files the game builds again. The first start afterwards may stutter once. Your shaders are not touched.') },
  { id: 'leftovers', label: t('Remove old copies'), note: t('Old versions of the game runtime and the launcher that updates set aside. The current ones stay.') },
];

/** The panel under "Reset game settings": one tick per group, all ticked, shown only while that box is ticked. */
const groupsHtml = () => `
  <div class="repair-groups" id="repair-groups" hidden>
    <p class="repair-note">${esc(t('Choose which settings go back to the defaults. Everything is ticked; untick what you want to keep.'))}</p>
    <ul class="repair-list">
      ${GAME_GROUPS.map((g) => `
        <li><label class="repair-choice">
          <input type="checkbox" name="group" value="${g.id}" checked />
          <span><strong>${esc(g.label)}</strong><span class="repair-note">${esc(g.note)}</span></span>
        </label></li>`).join('')}
    </ul>
    <p class="repair-warn" role="alert" hidden>${esc(t('Tick at least one group, or untick Reset game settings.'))}</p>
  </div>`;

const size = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`);

/** Step 1. Resolves { actions, groups } (groups: the game settings groups, null for all of them), or null for Cancel, Esc or a click outside. */
function choose() {
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <form method="dialog">
        <div class="modal-body">
          <h2 id="modal-title">${esc(t('Repair and reset'))}</h2>
          <p>${esc(t('Pick what to fix. Nothing changes until you have seen the list of files on the next step. Your saves, profiles, shaders and game files are never touched.'))}</p>
          <ul class="repair-list">
            ${CHOICES.map((c) => `
              <li><label class="repair-choice">
                <input type="checkbox" name="repair" value="${c.id}" ${c.id === 'game_settings' ? 'aria-controls="repair-groups" aria-expanded="false"' : ''} />
                <span><strong>${esc(c.label)}</strong><span class="repair-note">${esc(c.note)}</span></span>
              </label>${c.id === 'game_settings' ? groupsHtml() : ''}</li>`).join('')}
          </ul>
        </div>
        <div class="modal-actions">
          <button class="btn quiet" type="button" data-answer="">${esc(t('Cancel'))}</button>
          <button class="btn primary" type="submit" data-answer="ok">${esc(t('Review'))}</button>
        </div>
      </form>`;
    document.body.appendChild(dlg);
    const checked = (name) => [...dlg.querySelectorAll(`input[name="${name}"]:checked`)].map((i) => i.value);
    const panel = dlg.querySelector('#repair-groups'), warn = panel.querySelector('.repair-warn');
    const game = dlg.querySelector('input[name="repair"][value="game_settings"]');
    const sync = () => {
      panel.hidden = !game.checked;
      game.setAttribute('aria-expanded', String(game.checked));
      warn.hidden = true;
    };
    game.addEventListener('change', sync);
    panel.addEventListener('change', () => { warn.hidden = true; });
    dlg.querySelector('form').addEventListener('submit', (e) => {
      e.preventDefault();
      const actions = checked('repair');
      if (actions.length && !canReview(actions, checked('group'))) {
        warn.hidden = false;   // game settings are picked but no group: stay open
        return;
      }
      dlg.close('ok');
    });
    dlg.addEventListener('close', () => {
      let picked = null;
      if (dlg.returnValue === 'ok') {
        const actions = checked('repair');
        picked = { actions, groups: actions.includes('game_settings') ? groupsPayload(checked('group')) : null };
      }
      dlg.remove();
      resolve(picked);
    });
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[type="button"][data-answer]')) dlg.close('');
      else if (e.target === dlg) dlg.close('');
    });
    dlg.showModal();
  });
}

const reviewHtml = (plan) => {
  const keeps = plan.changes.some((c) => c.backup);
  return `
    <p>${esc(t('These files will change:'))}</p>
    <ul class="modal-list">
      ${plan.changes.map((c) => `<li>${icon(c.what === 'remove' ? 'trash-2' : 'rotate-ccw')}<span><span class="selectable">${esc(c.file)}</span>
        <span class="repair-note">${esc(c.what === 'remove' ? t('removed') : c.groups?.length ? t('{groups} back to defaults, the other settings stay', { groups: groupLabels(c.groups) }) : t('back to defaults'))}, ${size(c.bytes)}${c.backup ? esc(t(', a copy is kept')) : ''}</span></span></li>`).join('')}
    </ul>
    <p>${keeps ? `${esc(t('A copy of the settings files is kept, so Undo last repair in Settings > About puts them back.'))} ` : ''}${esc(t('Caches and old copies are not copied: the game builds the caches again, and old copies are not needed.'))}</p>`;
};

/**
 * The whole flow. Resolves { changed, launcher } or null when the player stopped or nothing needed changing.
 * `launcher` is true when the launcher's own settings were reset (the page then reloads them).
 */
export async function runRepair() {
  const choice = await choose();
  if (choice === null) return null;
  const { actions, groups } = choice;
  if (!actions.length) {
    toast(t('Pick at least one thing to fix'), 'error');
    return null;
  }
  let plan;
  try {
    plan = await api.repairPlan({ actions, groups });
  } catch (e) {
    toast(errText(e), 'error');
    return null;
  }
  if (!plan.changes.length) {
    toast(t('Nothing needed changing'), 'ok');
    return null;
  }
  const yes = await confirm({ title: t('Check before changing'), html: reviewHtml(plan), ok: t('Repair now'), danger: true });
  if (!yes) return null;
  try {
    const report = await api.repairApply({ actions, groups });
    toast(report.done.length > 1 ? t('Repaired: {count} changes', { count: report.done.length }) : fromBackend(report.done[0]) || t('Repaired'), 'ok');
    return { changed: report.done.length > 0, launcher: actions.includes('launcher_prefs') };
  } catch (e) {
    toast(errText(e), 'error');
    return { changed: true, launcher: false };
  }
}

/** Undo the last repair, after a question. Resolves true when something was put back. */
export async function runUndo(info) {
  const files = (info?.files ?? []).join(', ');
  const yes = await confirm({ title: t('Undo last repair'), body: (info?.files ?? []).length > 1 ? t('This puts back {files} as they were before the repair.', { files }) : t('This puts back {files} as it was before the repair.', { files: files || t('the files') }), ok: t('Undo') });
  if (!yes) return false;
  try {
    const report = await api.repairUndo();
    toast(report.done.length === 1 ? t('Put back: 1 file') : t('Put back: {count} files', { count: report.done.length }), 'ok');
    return true;
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
}
