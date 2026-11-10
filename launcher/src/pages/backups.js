// Save backups: the question and the actions behind Settings > Saves > Backups (savebackup.rs). The list of copies and the restore are
// shown here; the on/off setting and the interval are plain pkp.ini keys ([backup] interval_minutes, keep) drawn by the settings schema.
import { api } from '../api.js';
import { confirm, errText, esc, toast } from '../ui.js';
import { language, t } from '../i18n.js';

const KIND = { auto: t('Automatic'), exit: t('When the game closed'), manual: t('Back up now'), 'before-restore': t('Before a restore') };

export const size = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`);
export const when = (iso) => {
  const d = new Date(iso ?? '');
  return Number.isNaN(+d) ? String(iso ?? '') : d.toLocaleString(language, { dateStyle: 'medium', timeStyle: 'short' });
};

/** The short value on the Backups row: how many copies there are. */
export function backupText(status) {
  if (!status) return t('Checking');
  return status.count ? (status.count === 1 ? t('1 backup') : t('{count} backups', { count: status.count })) : t('None yet');
}

/** The line under it: how big they are together and when the newest was made ('' when there is none). */
export function backupDetail(status) {
  return status?.count ? t('Together {size}. The newest is from {when}.', { size: size(status.bytes), when: when(status.last?.created) }) : '';
}

/** Radio list of the copies, newest first selected. Resolves the chosen folder name, or null (Cancel, Esc, click outside). */
function pick(list) {
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <form method="dialog">
        <div class="modal-body">
          <h2 id="modal-title">${esc(t('Restore a backup'))}</h2>
          <p>${esc(t('Pick the moment to go back to. Your current save is copied first, so you can undo this by restoring the copy called "Before a restore".'))}</p>
          <ul class="backup-list">
            ${list.map((e, i) => `<li><label class="backup-choice">
              <input type="radio" name="backup" value="${esc(e.name)}" ${i === 0 ? 'checked' : ''} />
              <span><strong>${esc(when(e.created))}</strong>
                <span class="backup-note">${esc(KIND[e.kind] ?? e.kind)}, ${esc(size(e.bytes))}${e.profile ? `, ${esc(e.profile)}` : ''}</span></span>
            </label></li>`).join('')}
          </ul>
        </div>
        <div class="modal-actions">
          <button class="btn quiet" type="button" data-answer="">${esc(t('Cancel'))}</button>
          <button class="btn primary" type="submit" data-answer="ok">${esc(t('Restore'))}</button>
        </div>
      </form>`;
    document.body.appendChild(dlg);
    dlg.querySelector('form').addEventListener('submit', (e) => {
      e.preventDefault();
      dlg.close('ok');
    });
    dlg.addEventListener('close', () => {
      const chosen = dlg.returnValue === 'ok' ? dlg.querySelector('input[name="backup"]:checked')?.value ?? null : null;
      dlg.remove();
      resolve(chosen);
    });
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[type="button"][data-answer]')) dlg.close('');
      else if (e.target === dlg) dlg.close('');
    });
    dlg.showModal();
  });
}

/** Back up now. Resolves true when something was copied. */
export async function backupNow() {
  try {
    const r = await api.backupNow();
    if (r.result === 'made') toast(t('Backup made'), 'ok');
    else if (r.result === 'unchanged') toast(t('Nothing has changed since the last backup'), 'ok');
    else toast(t('There is no save to back up yet'), 'ok');
    return r.result === 'made';
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
}

/** The whole restore flow: list, pick, one more question, restore. Resolves true when the save was put back. */
export async function runRestore() {
  let list;
  try {
    list = await api.backupList();
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
  if (!list.length) {
    toast(t('There are no backups yet'), 'error');
    return false;
  }
  const name = await pick(list);
  if (!name) return false;
  const e = list.find((x) => x.name === name);
  const yes = await confirm({
    title: t('Restore this backup?'),
    html: `<p>${esc(t('Your save goes back to {when}. Anything you did in the game after that moment is gone from the save, but it is copied first as "Before a restore", so you can come back to it.', { when: '\u0001' })).replace('\u0001', `<strong>${esc(when(e.created))}</strong>`)}</p><p>${esc(t('The game must be closed.'))}</p>`,
    ok: t('Restore'),
    danger: true,
  });
  if (!yes) return false;
  try {
    await api.backupRestore({ name });
    toast(t('Save restored'), 'ok');
    return true;
  } catch (err) {
    toast(errText(err), 'error');
    return false;
  }
}
