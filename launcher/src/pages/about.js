import { getVersion } from '@tauri-apps/api/app';
import { icon } from '../util.js';

export async function render(el) {
  let version = '';
  try {
    version = await getVersion();
  } catch {
    version = 'dev';
  }
  el.innerHTML = `
    <div class="page">
      <div class="hero">
        <h1>About</h1>
        <p class="dim">Launcher for the Pokopia PC port (a static recompilation, pkp.exe).</p>
      </div>
      <div class="card">
        <h2>${icon('info')}Pokopia Launcher</h2>
        <dl class="kv">
          <dt>Version</dt><dd>${version}</dd>
          <dt>Licence</dt><dd>GPL-3.0-or-later</dd>
          <dt>Built with</dt><dd>Tauri 2, Lucide icons (ISC)</dd>
        </dl>
      </div>
      <div class="banner warn">
        ${icon('triangle-alert')}
        <div><strong>No game files included.</strong> You need your own legally dumped copy of the game. The launcher and the port contain no game data, and this project is not affiliated with or endorsed by Nintendo, The Pokemon Company, or Koei Tecmo.</div>
      </div>
    </div>`;
}
