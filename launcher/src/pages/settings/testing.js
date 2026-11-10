// The Testing tab's buttons: send logs (export and open the bug form), read them in a window, open the export folder.
import { api } from '../../api.js';
import { store } from '../../store.js';
import { sendLogs, viewLogs } from '../testing-logs.js';

export function createTestingActions() {
  return {
    actions: {
      'logs-send': () => sendLogs(store.state.app?.version ?? ''),
      'logs-view': viewLogs,
      'logs-folder': () => api.testingLogsOpenDir(),
    },
  };
}
