// The save folder's buttons: import a save from another place, back up and restore.
import { api } from '../../api.js';
import { confirm, errText, esc, toast } from '../../ui.js';
import { backupNow, runRestore } from '../backups.js';
import { IMPORT_FACTS } from '../../settings-schema.js';

export function createSaveActions(h) {
  const { st } = h;

  async function refreshBackups() {
    try {
      st.backups = await api.backupStatus();
    } catch (e) {
      console.warn('backup_status', e);
    }
    h.draw();
  }

  async function runImport(what, source, html) {
    if (!(await confirm({ title: `Import ${what}?`, html, body: html ? '' : IMPORT_FACTS, ok: 'Import' }))) return;
    st.importing = true;
    st.importResult = null;
    h.draw();
    try {
      await h.flush();
      const r = await api.importSave({ source });
      st.importResult = r.backup
        ? { kind: 'ok', text: `Imported ${r.files} files. Your previous save is kept in`, path: r.backup }
        : { kind: 'ok', text: `Imported ${r.files} files. There was no previous save.` };
      toast('Save imported', 'ok');
    } catch (e) {
      st.importResult = { kind: 'danger', text: errText(e) };
      toast(errText(e), 'error');
    }
    st.importing = false;
    h.draw();
  }

  return {
    load() {
      refreshBackups();
      api.saveSources().then((list) => {
        st.sources = list;
        h.draw();
      }).catch((e) => console.warn('save_sources', e));
    },
    actions: {
      import: (arg, name) => runImport(`from ${name}`, arg),
      async 'import-folder'() {
        const picked = await api.pickFolder({ title: 'Choose a folder with the save', start: null });
        if (picked) await runImport('this save', picked, `<p class="path selectable">${esc(picked)}</p><p>${esc(IMPORT_FACTS)}</p>`);
      },
      async 'backup-now'() {
        await h.flush();
        await backupNow();
        await refreshBackups();
      },
      async 'backup-restore'() {
        await h.flush();
        await runRestore();
        await refreshBackups();
      },
      'backup-open': () => api.openBackupDir(),
    },
  };
}
