// Steam shortcuts: 'game' starts the game at once, 'launcher' opens this window. Adds or removes one and says what happened.
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

  async function steamDo(command, target) {
    if (st.steamBusy) return;
    st.steamBusy = true;
    h.draw();
    try {
      toast(fromBackend(await command({ target })), 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
    st.steamBusy = false;
    await refresh();
  }

  return {
    load: refresh,
    actions: {
      'steam-add-game': () => steamDo(api.steamAdd, 'game'),
      'steam-add-launcher': () => steamDo(api.steamAdd, 'launcher'),
      'steam-remove-game': () => steamDo(api.steamRemove, 'game'),
      'steam-remove-launcher': () => steamDo(api.steamRemove, 'launcher'),
    },
  };
}
