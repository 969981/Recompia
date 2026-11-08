// Repair and reset: the dialogs for repair.rs. Step 1 picks what to fix, step 2 shows exactly which files change (nothing is touched
// before that), step 3 does it. The Rust side keeps a copy of the changed settings files, so Undo is one button in Settings > About.
import { api, confirm, errText, esc, icon, toast } from '../ui.js';

const CHOICES = [
  { id: 'game_settings', label: 'Reset game settings', note: 'Graphics, performance, network and timing choices (pkp.ini) go back to the defaults. Your game files, profile and shader folders are kept.' },
  { id: 'controls', label: 'Reset controls', note: 'The game makes its default key and pad bindings again the next time it starts.' },
  { id: 'launcher_prefs', label: 'Reset launcher settings', note: 'Launcher options go back to the defaults. The game folder, the game files folder and the update channel are kept.' },
  { id: 'caches', label: 'Clear caches', note: 'Cache files the game builds again. The first start afterwards may stutter once. Your shaders are not touched.' },
  { id: 'leftovers', label: 'Remove old copies', note: 'Old versions of the game runtime and the launcher that updates set aside. The current ones stay.' },
];

const size = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`);

/** Step 1. Resolves the chosen action ids, or null for Cancel, Esc or a click outside. */
function choose() {
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <form method="dialog">
        <div class="modal-body">
          <h2 id="modal-title">Repair and reset</h2>
          <p>Pick what to fix. Nothing changes until you have seen the list of files on the next step. Your saves, profiles, shaders and game files are never touched.</p>
          <ul class="repair-list">
            ${CHOICES.map((c) => `
              <li><label class="repair-choice">
                <input type="checkbox" name="repair" value="${c.id}" />
                <span><strong>${esc(c.label)}</strong><span class="repair-note">${esc(c.note)}</span></span>
              </label></li>`).join('')}
          </ul>
        </div>
        <div class="modal-actions">
          <button class="btn quiet" type="button" data-answer="">Cancel</button>
          <button class="btn primary" type="submit" data-answer="ok">Review</button>
        </div>
      </form>`;
    document.body.appendChild(dlg);
    dlg.querySelector('form').addEventListener('submit', (e) => {
      e.preventDefault();
      dlg.close('ok');
    });
    dlg.addEventListener('close', () => {
      const picked = dlg.returnValue === 'ok' ? [...dlg.querySelectorAll('input[name="repair"]:checked')].map((i) => i.value) : null;
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
    <p>These files will change:</p>
    <ul class="modal-list">
      ${plan.changes.map((c) => `<li>${icon(c.what === 'remove' ? 'trash-2' : 'rotate-ccw')}<span><span class="selectable">${esc(c.file)}</span>
        <span class="repair-note">${c.what === 'remove' ? 'removed' : 'back to defaults'}, ${size(c.bytes)}${c.backup ? ', a copy is kept' : ''}</span></span></li>`).join('')}
    </ul>
    <p>${keeps ? 'A copy of the settings files is kept, so Undo last repair in Settings > About puts them back. ' : ''}Caches and old copies are not copied: the game builds the caches again, and old copies are not needed.</p>`;
};

/**
 * The whole flow. Resolves { changed, launcher } or null when the player stopped or nothing needed changing.
 * `launcher` is true when the launcher's own settings were reset (the page then reloads them).
 */
export async function runRepair() {
  const actions = await choose();
  if (actions === null) return null;
  if (!actions.length) {
    toast('Pick at least one thing to fix', 'error');
    return null;
  }
  let plan;
  try {
    plan = await api('repair_plan', { actions });
  } catch (e) {
    toast(errText(e), 'error');
    return null;
  }
  if (!plan.changes.length) {
    toast('Nothing needed changing', 'ok');
    return null;
  }
  const yes = await confirm({ title: 'Check before changing', html: reviewHtml(plan), ok: 'Repair now', danger: true });
  if (!yes) return null;
  try {
    const report = await api('repair_apply', { actions });
    toast(report.done.length > 1 ? `Repaired: ${report.done.length} changes` : report.done[0] || 'Repaired', 'ok');
    return { changed: report.done.length > 0, launcher: actions.includes('launcher_prefs') };
  } catch (e) {
    toast(errText(e), 'error');
    return { changed: true, launcher: false };
  }
}

/** Undo the last repair, after a question. Resolves true when something was put back. */
export async function runUndo(info) {
  const files = (info?.files ?? []).join(', ');
  const yes = await confirm({ title: 'Undo last repair', body: `This puts back ${files || 'the files'} as ${(info?.files ?? []).length > 1 ? 'they were' : 'it was'} before the repair.`, ok: 'Undo' });
  if (!yes) return false;
  try {
    const report = await api('repair_undo');
    toast(`Put back: ${report.done.length} file${report.done.length === 1 ? '' : 's'}`, 'ok');
    return true;
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
}
