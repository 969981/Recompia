// Settings > About > Themes: adding a theme pack (a folder or a .zip), removing one, opening the themes folder. The pick itself is the
// Theme row (settings.js hands it to themes.js chooseTheme); the checking of a pack is src-tauri/src/themes.rs.
import { api } from '../../api.js';
import { t } from '../../i18n.js';
import { errText, toast } from '../../ui.js';
import { chooseTheme, currentTheme, reloadThemes } from '../../themes.js';

export function createThemeActions(h) {
  const { st } = h;

  async function busy(job) {
    if (st.themeBusy) return;
    st.themeBusy = true;
    h.draw();
    try {
      await job();
    } catch (e) {
      toast(errText(e), 'error');
    }
    st.themeBusy = false;
    h.draw();
  }

  async function add(path) {
    const added = await api.addTheme({ path });
    await reloadThemes();
    await api.applyBranding().catch((e) => console.warn('branding', e));   // a pack that is the chosen theme again brings its icon and Steam pictures
    toast(t('Added the theme pack {name}.', { name: added.name }), 'ok');
  }

  return {
    actions: {
      'theme-add-folder': () => busy(async () => {
        const path = await api.pickFolder({ title: t('Choose a theme pack folder'), start: null });
        if (path) await add(path);
      }),
      'theme-add-zip': () => busy(async () => {
        const path = await api.pickThemeFile();
        if (path) await add(path);
      }),
      'theme-remove': (pack) => busy(async () => {
        await api.removeThemePack({ pack });
        await reloadThemes();   // a chosen theme of this pack drops back to the launcher's own look
        await chooseTheme(currentTheme());   // the icon and the Steam pictures follow
      }),
      'theme-open-folder': () => busy(() => api.openThemesDir()),
    },
  };
}
