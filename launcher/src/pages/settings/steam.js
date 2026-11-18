// Steam shortcuts: the launcher shortcut opens this window (add or remove); the old game shortcut an earlier launcher added can only be removed.
import { api } from '../../api.js';
import { errText, toast } from '../../ui.js';
import { fromBackend } from '../../backend-text.js';

export function createSteamActions(h) {
  const { st } = h;

  async function refresh() {
    try {
      st.steam = await api.steamStatus();
    } catch (e) {
      console.warn('steam_status', e);
    }
    h.draw();
  }

  async function steamDo(command, args) {
    if (st.steamBusy) return;
    st.steamBusy = true;
    h.draw();
    try {
      toast(fromBackend(await command(args)), 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
    st.steamBusy = false;
    await refresh();
  }

  return {
    load: refresh,
    actions: {
      'steam-add-launcher': () => steamDo(api.steamAdd),
      'steam-remove-game': () => steamDo(api.steamRemove, { target: 'game' }),
      'steam-remove-launcher': () => steamDo(api.steamRemove, { target: 'launcher' }),
    },
  };
}
