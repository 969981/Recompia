// "Save logs for a bug report": one button on the Co-op page and one in Settings. The zip is made by the launcher (logbundle.rs).
import { api } from './api.js';
import { t } from './i18n.js';
import { errText, toast } from './ui.js';

export async function saveLogs() {
  try {
    const { path } = await api.saveLogBundle();
    toast(t('Saved {path}. Attach it to your bug report.', { path }), 'ok');
  } catch (e) {
    toast(errText(e), 'error');
  }
}
