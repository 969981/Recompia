// Online presence: "Reset my online ID". Emptying online_salt in launcher.json is the whole reset; online.rs makes a new salt, so a new
// ID, the next time the game starts with the setting on.
import { api } from '../../api.js';
import { t } from '../../i18n.js';
import { confirm, errText, toast } from '../../ui.js';

export function createOnlineActions(h) {
  return {
    actions: {
      async 'online-reset'() {
        const yes = await confirm({
          title: t('Reset your online ID?'),
          body: t('The next time you play online, you start with a new online ID. What the old ID sent stays with the old ID and is not linked to the new one.'),
          ok: t('Reset my online ID'),
        });
        if (!yes) return;
        try {
          await h.flush();
          await api.saveConfig({ config: { ...(await api.getConfig()), online_salt: '' } });
          await h.reload();
          toast(t('Online ID reset'), 'ok');
        } catch (e) {
          toast(errText(e), 'error');
        }
      },
    },
  };
}
