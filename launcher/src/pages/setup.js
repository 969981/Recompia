import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

function candidateHtml(c, chosen) {
  const title = c.kind === 'base' ? 'Base game' : `Update ${esc(c.version)}`;
  const used = chosen && chosen.path === c.path;
  return `
    <div class="candidate ${used ? 'chosen' : ''}">
      <div class="row between">
        <h3>${title}</h3>
        ${used ? `<span class="chip ok">${icon('circle-check')}Will be used</span>` : c.ok ? '<span class="chip">Valid, older</span>' : `<span class="chip danger">${icon('circle-x')}Incomplete</span>`}
      </div>
      <div class="path">${esc(c.path)}</div>
      <ul class="checks">
        ${c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(k.label)}</li>`).join('')}
      </ul>
    </div>`;
}

export async function render(el, ctx) {
  const cfg = await api('get_config');
  let path = cfg.dump_dir || '';
  let report = path ? await api('validate_dump', { path }) : null;

  function draw() {
    const ok = report && report.chosen;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${ctx.firstRun ? 'Welcome. Set up your game files.' : 'Game files'}</h1>
          <p class="dim">Pokopia Launcher does not include the game. You need your own legally dumped copy, already decrypted. The launcher only reads this folder and never writes to it.</p>
        </div>

        <div class="card">
          <h2>${icon('folder-search')}Dump folder</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="C:/path/to/Pokopia_dump" value="${esc(path)}" />
              <button class="btn" id="browse">${icon('folder-open')}Browse</button>
            </div>
            <p class="faint">Expected layout: <code>base/</code> and <code>updates/&lt;version&gt;/</code>, each with <code>exefs/</code> (main, sdk, subsdk0, rtld, main.npdm) and <code>romfs/</code>. The highest valid update is used, otherwise the base game.</p>
          </div>
        </div>

        ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map(esc).join('<br>')}</div></div>` : ''}
        ${report && report.candidates.length ? `<div class="stack">${report.candidates.map((c) => candidateHtml(c, report.chosen)).join('')}</div>` : ''}

        <div class="row">
          <button class="btn primary" id="use" ${ok ? '' : 'disabled'}>${icon('circle-check')}Use these game files</button>
          ${ctx.firstRun ? '' : '<button class="btn quiet" id="cancel">Back</button>'}
        </div>
      </div>`;
    refreshIcons();

    const input = el.querySelector('#path');
    const check = async () => {
      path = input.value.trim();
      report = path ? await api('validate_dump', { path }) : null;
      draw();
    };
    input.addEventListener('change', check);
    input.addEventListener('keydown', (e) => e.key === 'Enter' && check());
    el.querySelector('#browse').addEventListener('click', async () => {
      const picked = await api('pick_folder', { title: 'Choose your decrypted Pokopia dump folder', start: input.value || null });
      if (picked) {
        input.value = picked;
        check();
      }
    });
    el.querySelector('#use').addEventListener('click', async () => {
      try {
        await api('save_dump', { path });
        toast('Game files saved.', 'ok');
        ctx.go('play');
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#cancel')?.addEventListener('click', () => ctx.go('play'));
  }
  draw();
}
