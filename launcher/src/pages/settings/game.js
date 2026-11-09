// Game files and shaders: change the files, set up again, the setup log, shader storage.
import { api } from '../../api.js';
import { confirm } from '../../ui.js';
import { askCompact, store } from '../../store.js';

export function createGameActions(h) {
  const { ctx } = h;

  return {
    load: () => store.refreshStorage(),
    actions: {
      'change-files': () => ctx.go('setup'),
      async 'setup-again'() {
        const yes = await confirm({ title: 'Set up again?', body: 'This builds the game and prepares its shaders again, which takes a while. Your saves are kept.', ok: 'Set up' });
        if (yes && (await store.startSetup())) ctx.go('home');
      },
      'setup-log': () => api.openSetupLog(),
      'compact-shaders': () => askCompact(),
      async 'expand-shaders'() {
        const st = store.state.storage;
        const yes = await confirm({
          title: 'Use loose shader files?',
          body: `This writes the ${(st?.archive_shaders ?? 0).toLocaleString('en')} packed shaders back out as separate files, about ${Math.ceil((st?.archive_raw_bytes ?? 0) / 2 ** 30)} GB. The game runs the same either way, and a later update stops reading loose files.`,
          ok: 'Use loose files',
        });
        if (yes) await store.expandShaders();
      },
      'stop-repack': () => store.cancelRepack(),
      'open-log': () => api.openGameFile({ name: 'pkp.log' }),
    },
  };
}
