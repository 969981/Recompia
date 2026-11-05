import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

// one listener for the whole session; the page being shown sets the handler
let dlssHandler = null;
listen('dlss-progress', (ev) => dlssHandler && dlssHandler(ev.payload));
let nupHandler = null;
listen('nup-progress', (ev) => nupHandler && nupHandler(ev.payload));

function candidateHtml(c, chosen) {
  const title = c.kind === 'base' ? 'Base game' : `Update ${esc(c.version)}`;
  const used = chosen && chosen.path === c.path;
  // an incomplete folder that another one makes unnecessary (an empty base/ next to a full update) is not an error
  const unneeded = chosen && !used && !c.ok;
  const chip = used
    ? `<span class="chip ok">${icon('circle-check')}Will be used</span>`
    : c.ok ? '<span class="chip">Valid, older</span>'
    : unneeded ? '<span class="chip">Not needed</span>'
    : `<span class="chip danger">${icon('circle-x')}Incomplete</span>`;
  return `
    <div class="candidate ${used ? 'chosen' : ''}">
      <div class="row between">
        <h3>${title}</h3>
        ${chip}
      </div>
      <div class="path">${esc(c.path)}</div>
      ${unneeded ? '' : `<ul class="checks">
        ${c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(k.label)}</li>`).join('')}
      </ul>`}
    </div>`;
}

// the shaders outside the used set, translated in the background once Play is on (shaders.rs, Part::Rest)
export function backgroundHtml(s) {
  if (!s.rest_pending) return '';
  const b = s.background;
  const running = b.state === 'running';
  const pct = b.total ? Math.floor((100 * b.done) / b.total) : 0;
  const text = running ? esc(b.message)
    : b.state === 'error' ? `Stopped: ${esc(b.message)}`
    : s.background_paused ? 'Paused. Resume, or it continues the next time the launcher starts.'
    : b.state === 'cancelled' ? esc(b.message)
    : 'Waiting (it runs while the launcher is open and the game is not).';
  return `
    <div class="stack" id="bg-shaders">
      <p class="dim">You can play now: the shaders the game is known to use are translated. The other shaders are translated in the background while the launcher is open, at the lowest priority, and pause while the game runs. A shader the game needs before then is drawn without it in that run and translated when the game exits.</p>
      <p class="faint">${text}</p>
      ${running ? `<progress max="100" value="${b.total ? pct : ''}"></progress>` : ''}
      <div class="row">
        ${s.background_paused
          ? `<button class="btn" id="bg-resume">${icon('play')}Resume</button>`
          : `<button class="btn" id="bg-pause">${icon('circle-x')}Pause</button>`}
      </div>
    </div>`;
}

export function wireBackground(el, refresh) {
  el.querySelector('#bg-pause')?.addEventListener('click', async () => { await api('pause_background'); refresh(); });
  el.querySelector('#bg-resume')?.addEventListener('click', async () => { await api('resume_background'); refresh(); });
}

function shadersHtml(s, saved) {
  const p = s.progress;
  const running = p.state === 'running';
  const pct = p.total ? Math.floor((100 * p.done) / p.total) : 0;
  let body = '';
  if (running) {
    body = `<p class="dim">${esc(p.message)}</p>
      <progress max="100" value="${p.total ? pct : ''}"></progress>
      <p class="faint">${p.total ? `${pct}%. ` : ''}You can pause and continue later; finished work is kept.</p>`;
  } else if (p.state === 'error') {
    body = `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(p.message)}</div></div>`;
  } else if (s.complete) {
    body = `<p class="dim">${icon('circle-check')} Done. pkp.exe is built and the game's shaders are in <code>shader_cache/spirv</code> next to it.</p>`;
  } else if (s.playable && s.rest_pending) {
    body = backgroundHtml(s);
  } else if (s.retranslate && !s.rebuild && p.state !== 'cancelled') {
    body = `<p class="dim">This update has a newer shader translator, so the game's shaders are translated again from your romfs (needs the same free space as the first setup). You can pause and continue later. Your saves are kept.</p>`;
  } else if (s.rebuild && p.state !== 'cancelled') {
    body = `<p class="dim">This update changes the game code, so pkp.exe has to be built again from your game files (about 7 minutes). The old pkp.exe stays until the new one is ready. Your shaders and saves are kept.</p>`;
  } else {
    body = `<p class="dim">${p.state === 'cancelled' ? esc(p.message) : 'Checks your game files, builds pkp.exe from them on this PC (the download contains no game code; the compile takes a while and needs a few GB free while it runs), then translates the game\'s shaders from your romfs: the ~6,000 the game is known to use first, then Play turns on and the rest (about 100,000 in all; needs about 36 GB free while it runs and keeps about 26 GB) is translated in the background. You can pause and continue later.'}</p>`;
  }
  const label = s.complete || s.playable || ((s.rebuild || s.retranslate) && p.state !== 'cancelled') ? 'Set up again' : s.started ? 'Continue setup' : 'Set up';
  return `
    <div class="card">
      <h2>${icon('wrench')}Set up</h2>
      <div class="stack">
        ${body}
        <div class="row">
          ${running
            ? `<button class="btn" id="setup-cancel">${icon('circle-x')}Pause</button>`
            : `<button class="btn ${s.complete || s.playable ? '' : 'primary'}" id="setup-run" ${saved ? '' : 'disabled'}>${icon('wrench')}${label}</button>`}
          ${(s.complete || s.playable) && !running ? `<button class="btn primary" id="to-play">${icon('play')}Go to Play</button>` : ''}
        </div>
      </div>
    </div>`;
}

function dlssHtml(d, busy, p) {
  if (!d.available) return '';
  const pct = p && p.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : 0;
  return `
    <div class="card" id="dlss-card">
      <h2>${icon('monitor')}NVIDIA DLSS (optional)</h2>
      <div class="stack">
        <p class="dim">FSR 3 is included and needs nothing more. DLSS and DLAA need an NVIDIA RTX GPU and one more file, <code>nvngx_dlss.dll</code>, which NVIDIA licenses separately, so it is not in the download. This button gets it from NVIDIA's own DLSS repository on GitHub (version 310.9.16, checked against a fixed hash) and puts it in <code>${esc(d.dir)}</code>. By downloading it you accept the NVIDIA RTX SDKs licence: <code>licenses/LICENSE.NVIDIA-RTX-SDKs.txt</code>, also at github.com/NVIDIA/DLSS.</p>
        ${busy ? `<p class="dim">${esc(p?.message || 'Starting...')}</p><progress max="100" value="${pct}"></progress>` : ''}
        <div class="row">
          ${d.present ? `<span class="chip ok">${icon('circle-check')}nvngx_dlss.dll is installed</span>` : ''}
          <button class="btn ${d.present ? '' : 'primary'}" id="dlss-get" ${busy ? 'disabled' : ''}>${icon('monitor')}${d.present ? 'Download again' : 'Download DLSS from NVIDIA'}</button>
        </div>
      </div>
    </div>`;
}

function nupPct(p) {
  return p && p.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : 0;
}

function nupMsg(p) {
  return p && p.total ? `Unpacking the .nup: ${nupPct(p)}%${p.file ? ` (${p.file})` : ''}` : 'Reading the .nup...';
}

export async function render(el, ctx) {
  const cfg = await api('get_config');
  let path = cfg.dump_dir || '';
  let saved = path;
  let report = path ? await api('validate_dump', { path }) : null;
  let setup = await api('setup_status');
  let dlss = await api('dlss_status');
  let dlssBusy = false;
  let dlssProgress = null;
  let nupBusy = false;
  let nupProgress = null;

  function draw() {
    const ok = report && report.chosen;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${ctx.firstRun ? 'Welcome. Set up your game files.' : 'Game files'}</h1>
          <p class="dim">Pokopia Launcher does not include the game. You need your own legally dumped copy, already decrypted. The launcher only reads this folder and never writes to it (a .nup is unpacked into its own copy).</p>
        </div>

        <div class="card">
          <h2>${icon('folder-search')}Dump folder</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="C:/path/to/Pokopia_dump" value="${esc(path)}" />
              <button class="btn" id="browse" ${nupBusy ? 'disabled' : ''}>${icon('folder-open')}Browse</button>
              <button class="btn" id="browse-nup" ${nupBusy ? 'disabled' : ''}>${icon('file-archive')}.nup</button>
            </div>
            ${nupBusy ? `<p class="dim" id="nup-msg">${nupMsg(nupProgress)}</p><progress max="100" value="${nupPct(nupProgress)}"></progress>` : ''}
            <p class="faint">Or pick a <code>.nup</code> package instead of a folder: it is unpacked once into <code>nup_dump/</code> next to the game and used from there (needs the free space of the full game).</p>
            <p class="faint">Expected layout: <code>base/</code> and <code>updates/&lt;version&gt;/</code>, each with <code>exefs/</code> (main, sdk, subsdk0, rtld, main.npdm) and <code>romfs/</code>. The highest valid update is used, otherwise the base game.</p>
          </div>
        </div>

        ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map(esc).join('<br>')}</div></div>` : ''}
        ${report && report.candidates.length ? `<div class="stack">${report.candidates.map((c) => candidateHtml(c, report.chosen)).join('')}</div>` : ''}

        <div class="row">
          <button class="btn ${ok && saved !== path ? 'primary' : ''}" id="use" ${ok && setup.progress.state !== 'running' ? '' : 'disabled'}>${icon('circle-check')}Use these game files</button>
          ${ctx.firstRun ? '' : '<button class="btn quiet" id="cancel">Back</button>'}
        </div>

        ${shadersHtml(setup, ok && saved === path)}

        ${dlssHtml(dlss, dlssBusy, dlssProgress)}
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
    el.querySelector('#browse-nup').addEventListener('click', async () => {
      const picked = await api('pick_nup', { start: null });
      if (!picked) return;
      nupBusy = true;
      nupProgress = null;
      draw();
      try {
        path = await api('import_nup', { path: picked });
        report = await api('validate_dump', { path });
      } catch (e) {
        toast(errText(e), 'error');
      }
      nupBusy = false;
      draw();
    });
    el.querySelector('#use').addEventListener('click', async () => {
      try {
        await api('save_dump', { path });
        saved = path;
        toast('Game files saved. Next: Set up.', 'ok');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#cancel')?.addEventListener('click', () => ctx.go('play'));
    el.querySelector('#to-play')?.addEventListener('click', () => ctx.go('play'));
    el.querySelector('#setup-run')?.addEventListener('click', async () => {
      try {
        await api('start_setup');
        setup = await api('setup_status');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#setup-cancel')?.addEventListener('click', () => api('cancel_setup'));
    wireBackground(el, refreshBackground);
    el.querySelector('#dlss-get')?.addEventListener('click', async () => {
      dlssBusy = true;
      dlssProgress = null;
      draw();
      try {
        await api('fetch_dlss');
        toast('nvngx_dlss.dll downloaded and verified. Restart the game to use DLSS.', 'ok');
      } catch (e) {
        toast(errText(e), 'error');
      }
      dlssBusy = false;
      dlss = await api('dlss_status');
      draw();
    });
  }

  nupHandler = (p) => {
    nupProgress = p;
    if (!nupBusy) return;
    const msg = el.querySelector('#nup-msg');
    if (msg) {
      msg.textContent = nupMsg(p);
      const bar = msg.nextElementSibling;
      if (bar) bar.value = nupPct(p);
    }
  };

  dlssHandler = (p) => {
    dlssProgress = p;
    const card = el.querySelector('#dlss-card');
    if (card && dlssBusy) {
      card.outerHTML = dlssHtml(dlss, true, p);
      refreshIcons();
    }
  };

  async function refreshBackground() {
    setup = await api('setup_status');
    draw();
  }

  // background ticks replace only that block, so the path field keeps its focus
  ctx.onBackground(async (b) => {
    if (b.state !== 'running') return refreshBackground();
    setup = { ...setup, background: b };
    const block = el.querySelector('#bg-shaders');
    if (!block) return;
    block.outerHTML = backgroundHtml(setup);
    refreshIcons();
    wireBackground(el, refreshBackground);
  });

  ctx.onSetup(async (p) => {
    const was = setup.progress.state;
    setup = p.state === 'running' ? { ...setup, progress: p } : await api('setup_status');
    // progress ticks only touch the setup card's text, so the path field keeps its focus
    if (was === 'running' && p.state === 'running' && el.querySelector('progress')) {
      const card = el.querySelector('#setup-cancel')?.closest('.card');
      if (card) {
        card.outerHTML = shadersHtml(setup, true);
        refreshIcons();
        el.querySelector('#setup-cancel')?.addEventListener('click', () => api('cancel_setup'));
        return;
      }
    }
    draw();
  });
  draw();
}
