// Online presence, the launcher side of the switch: pkp.ini [pkp] online_presence (online.rs reads it while the game runs, and the
// game's own Settings screen has it on its Online tab). The Home chip, Settings > Game and the one-time question all write it here.
import { api } from './api.js';
import { t } from './i18n.js';
import { confirm } from './ui.js';

export const ONLINE_KEY = 'pkp.online_presence';
const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v ?? '').trim().toLowerCase());

/** On in this pkp.ini map (read_settings' { 'section.key': value }). Missing: off. */
export const onlineOn = (pkp) => truthy(pkp?.[ONLINE_KEY]);

/** The edit that switches it. */
export const onlineEdit = (on) => ({ file: 'pkp', section: 'pkp', key: 'online_presence', value: on ? '1' : '0' });

export const setOnline = (on) => api.writeSettings({ edits: [onlineEdit(on)] });

/**
 * Before the first Play with this launcher: asks once whether to share that the player is online. No is the default (Enter, Esc and a
 * click outside all answer no), it is never asked again (launcher.json online_asked), and a player who already switched it on is not
 * asked. Never stops Play: a failure here only means no question.
 */
export async function askOnlineOnce() {
  try {
    const cfg = await api.getConfig();
    if (cfg.online_asked) return;
    await api.saveConfig({ config: { ...cfg, online_asked: true } });
    const data = await api.readSettings();
    if (onlineOn(data.pkp)) return;
    const yes = await confirm({
      title: t('Share that you are online?'),
      body: t('While you play, the launcher can tell the Recompia online server that you are playing: an online name you choose (or anonymous), your version, Windows or Linux, and how many Pokemon have a home in your save. Your save and your user ID are never sent. You can change this any time on Home, in Settings > Game or in the game\'s own Settings.'),
      ok: t('Yes, share it'), cancel: t('No, thanks'), focusCancel: true,
    });
    if (yes) await setOnline(true);
  } catch (e) {
    console.warn('online question', e);
  }
}
