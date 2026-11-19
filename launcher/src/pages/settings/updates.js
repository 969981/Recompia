// About > updates: check for a new version, install it, open a release page. Picking a version is the version picker's
// (pages/version-picker.js).
import { api } from '../../api.js';
import { store } from '../../store.js';

export function createUpdateActions(h) {
  const { st } = h;

  return {
    actions: {
      async 'check-updates'() {
        st.checking = true;
        h.draw();
        await store.refreshNews(true);
        st.checking = false;
        st.checked = true;
        h.draw();
      },
      update: () => store.installUpdate(),
      'open-url': (url) => api.openLink({ url }),
      'licences-open': () => api.openLicensesDir(),
    },
  };
}
