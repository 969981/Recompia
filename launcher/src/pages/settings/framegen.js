// Frame generation: what switching it on does when AMD's DLL is missing (store.fetchFramegen shows the download in the sidebar pill).
import { api } from '../../api.js';
import { confirm, errText, toast } from '../../ui.js';
import { store } from '../../store.js';
import { t } from '../../i18n.js';
import { FRAMEGEN_DOWNLOAD } from '../../settings-schema.js';

export function createFramegen(h) {
  const { st } = h;

  return {
    /** The player switched frame generation on without the DLL: ask, fetch, and save the pick only once the file is in place. */
    async pick(f) {
      if (!(await confirm(FRAMEGEN_DOWNLOAD))) return h.draw(true);
      h.draw(true);         // off while the file comes (the row is locked meanwhile)
      try {
        await store.fetchFramegen();
        st.framegen = await api.framegenStatus();
        if (!st.framegen) throw new Error(t('The file is not in the game folder after the download.'));
        await h.commit(f, true);
      } catch (e) {
        toast(errText(e), 'error');
        h.draw(true);
      }
    },
  };
}
