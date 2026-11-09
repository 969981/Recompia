// DLSS: the question before the file is downloaded, the download (store.fetchDlss, shown by the sidebar pill) and what a pick of
// DLSS/DLAA does when the file is missing.
import { api } from '../../api.js';
import { confirm, errText, toast } from '../../ui.js';
import { store } from '../../store.js';
import { DLSS_DOWNLOAD, readField } from '../../settings-schema.js';

export function createDlss(h) {
  const { st } = h;

  const ask = () => confirm(DLSS_DOWNLOAD);

  async function download() {
    await store.fetchDlss();
    st.dlss = await api.dlssStatus();
    h.saved();
    h.draw();
  }

  return {
    /** The player picked an upscaler that needs the file: ask, save the pick, fetch; undo the pick if the fetch fails. */
    async pick(f, value) {
      const before = readField(f, st.src);
      if (!(await ask())) return h.draw(true);
      h.commit(f, value);
      try {
        await download();
      } catch (e) {
        toast(errText(e), 'error');
        h.commit(f, before);   // the upscaler would only run as off without the file
      }
    },
    actions: {
      async 'dlss-get'() {
        if (await ask()) await download();
      },
    },
  };
}
