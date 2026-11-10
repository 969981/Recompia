// Repair and its Undo: the dialogs are in pages/repair.js; this keeps the last backup (st.repairBackup) and reloads what changed.
import { api } from '../../api.js';
import { num, t } from '../../i18n.js';
import { toast } from '../../ui.js';
import { runRepair, runUndo } from '../repair.js';

export function createRepairActions(h) {
  const { st } = h;

  return {
    load() {
      api.repairLast().then((info) => {
        st.repairBackup = info;
        h.draw();
      }).catch((e) => console.warn('repair_last', e));
    },
    actions: {
      async 'repair-open'() {
        await h.flush();
        const r = await runRepair();
        if (!r) return;
        if (r.launcher) {
          try { localStorage.removeItem('theme'); localStorage.removeItem('pkp.linkplay'); localStorage.removeItem('language'); } catch { /* nothing to clear */ }
          location.reload();
          return;
        }
        st.repairBackup = await api.repairLast().catch(() => null);
        await h.reload().catch((e) => console.error(e));
        h.draw();
      },
      async 'repair-undo'() {
        await h.flush();
        if (await runUndo(st.repairBackup)) {
          st.repairBackup = await api.repairLast().catch(() => null);
          await h.reload().catch((e) => console.error(e));
          toast(t('Reload the launcher (close and open it) to use the restored launcher settings'), 'ok');
        }
        h.draw();
      },
    },
  };
}
