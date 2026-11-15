// Play time as the launcher shows it, from the seconds the game counted for a save (src-tauri play_time.rs): whole hours and minutes.
import { num, t } from './i18n.js';

/** "3 h 38 min", "12 h" or "45 min" ("0 min" under a minute); hours above 999 get the language's thousands separator. */
export function playTimeText(seconds) {
  const min = Math.floor(Math.max(0, Number(seconds) || 0) / 60);
  const h = Math.floor(min / 60);
  const m = min % 60;
  if (!h) return t('{n} min', { n: m });
  return m ? t('{h} h {m} min', { h: num(h), m }) : t('{h} h', { h: num(h) });
}

/** The line a profile shows, or '' when its save has no play time yet. `times` is api.profilesPlayTime()'s answer. */
export function playTimeLine(times, profileId) {
  const s = times?.[profileId];
  return s == null ? '' : t('Play time: {time}', { time: playTimeText(s) });
}
