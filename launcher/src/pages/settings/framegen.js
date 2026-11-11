// Frame generation: what switching it on does when AMD's DLL is missing (store.fetchFramegen shows the download in the sidebar pill).
import { api } from '../../api.js';
import { confirm, errText, toast } from '../../ui.js';
import { store } from '../../store.js';
import { FRAMEGEN_DOWNLOAD } from '../../settings-schema.js';

export function createFramegen(h) {
  const { st } = h;

  return {
    /** The player switched frame generation on without the DLL: ask, save the pick, fetch; undo the pick if the fetch fails. */
    async pick(f) {
      if (!(await confirm(FRAMEGEN_DOWNLOAD))) return h.draw(true);
      h.commit(f, true);
      try {
        await store.fetchFramegen();
        st.framegen = await api.framegenStatus();
        h.saved();
        h.draw();
      } catch (e) {
        toast(errText(e), 'error');
        h.commit(f, false);   // it would only run as off without the file
      }
    },
  };
}
