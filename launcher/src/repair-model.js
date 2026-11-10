// The groups "Reset game settings" can reset on their own (the dialog in pages/repair.js). The ids are the ones in repair.rs
// (GAME_GROUPS plus "other"); a test keeps the two lists in step. Plain functions, so they run without the page.

import { t } from './i18n.js';

export const GAME_GROUPS = [
  { id: 'graphics', label: t('Graphics and performance'), note: t('Speed and picture tuning that is not on the Display or Graphics pages: shader and texture handling, frame pacing, memory and upscaling extras.') },
  { id: 'network', label: t('Network and Link Play'), note: t('Server and Link Play options. Your Link Play identity is kept.') },
  { id: 'gameplay', label: t('Gameplay options'), note: t('Camera aim, running jump and the other game tweaks in Settings > Game.') },
  { id: 'clock', label: t('Game clock and events'), note: t('The clock offset and the event you picked in Settings > Game.') },
  { id: 'backups', label: t('Save backup schedule'), note: t('How often saves are copied and how many copies stay. The backups themselves are never touched.') },
  { id: 'other', label: t('Everything else'), note: t('Any other advanced option that was set by hand in pkp.ini.') },
];

export const ALL_GROUP_IDS = GAME_GROUPS.map((g) => g.id);

/** What to send for the ticked groups: null when all of them (the full reset, exactly as before), else the ids in list order. */
export function groupsPayload(checked) {
  const picked = ALL_GROUP_IDS.filter((id) => checked.includes(id));
  return picked.length === ALL_GROUP_IDS.length ? null : picked;
}

/** The dialog may go on when something is picked and, if game settings are picked, at least one group of them. */
export function canReview(actions, checkedGroups) {
  if (!actions.length) return false;
  return !actions.includes('game_settings') || checkedGroups.length > 0;
}

/** "Graphics and performance, Gameplay options" for the review list. */
export const groupLabels = (ids) => ids.map((id) => GAME_GROUPS.find((g) => g.id === id)?.label ?? id).join(', ');
