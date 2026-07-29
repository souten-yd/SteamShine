/**
 * @file app.js
 * @brief SteamShine browser client for the shared Sunshine Web facade.
 */

const app = document.querySelector('#app');
let csrfToken = '';
let pollTimer = null;
let terminalSocket = null;
let terminalInstance = null;

/** @brief Escape arbitrary strings before putting them into a rendered template. */
function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[character]);
}

/** @brief Send one request to the SteamShine facade. */
function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (options.body && !(options.body instanceof Blob)) headers.set('Content-Type', 'application/json');
  if (csrfToken) headers.set('X-SteamShine-CSRF-Token', csrfToken);
  return fetch(`/api/steamshine/v1${path}`, { credentials: 'same-origin', ...options, headers });
}

/** @brief Parse a JSON response and throw a safe message on failure. */
async function json(response) {
  const result = await response.json().catch(() => ({}));
  if (!response.ok || result.status === false) throw new Error(result.message || result.error || `Request failed (${response.status})`);
  return result;
}

/** @brief Stop any page-scoped background timers before rendering a new page. */
function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

/** @brief Show a transient toast message. */
function toast(message, kind = 'info') {
  let wrap = document.querySelector('.toast-wrap');
  if (!wrap) {
    wrap = document.createElement('div');
    wrap.className = 'toast-wrap';
    document.body.appendChild(wrap);
  }
  const el = document.createElement('div');
  el.className = `toast ${kind}`;
  el.textContent = message;
  wrap.appendChild(el);
  setTimeout(() => el.remove(), 4000);
}

/** @brief Show a confirmation modal for a destructive action. Resolves true/false. */
function confirmDialog({ title, message, confirmLabel = 'Confirm', danger = true }) {
  return new Promise((resolve) => {
    const backdrop = document.createElement('div');
    backdrop.className = 'modal-backdrop';
    backdrop.innerHTML = `<div class="modal" role="alertdialog"><h3>${escapeHtml(title)}</h3><p>${escapeHtml(message)}</p><div class="btn-row"><button class="btn-ghost" data-a="cancel">Cancel</button><button class="${danger ? 'btn-danger' : 'btn-primary'}" data-a="confirm">${escapeHtml(confirmLabel)}</button></div></div>`;
    document.body.appendChild(backdrop);
    const close = (value) => { backdrop.remove(); resolve(value); };
    backdrop.addEventListener('mousedown', (e) => { if (e.target === backdrop) close(false); });
    backdrop.querySelector('[data-a="cancel"]').onclick = () => close(false);
    backdrop.querySelector('[data-a="confirm"]').onclick = () => close(true);
  });
}

/** @brief Inline icon set (stroke-based, 24x24 viewbox). */
const ICONS = {
  home: '<path d="M4 11.5 12 4l8 7.5"/><path d="M6 10v9h5v-5h2v5h5v-9"/>',
  activity: '<path d="M3 12h4l2 7 4-14 2 7h6"/>',
  grid: '<rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/>',
  sliders: '<path d="M5 5v6M5 15v4M12 5v3M12 12v7M19 5v10M19 19v0"/><circle cx="5" cy="12.5" r="1.7"/><circle cx="12" cy="9.5" r="1.7"/><circle cx="19" cy="16" r="1.7"/>',
  cpu: '<rect x="7" y="7" width="10" height="10" rx="1.5"/><rect x="10" y="10" width="4" height="4"/><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"/>',
  key: '<circle cx="8" cy="14.5" r="3.5"/><path d="M10.8 12 19 3.8M16 7l2.3 2.3M13.3 9.7 15.5 12"/>',
  users: '<circle cx="9" cy="8" r="3"/><path d="M3 20c0-3.3 2.7-6 6-6s6 2.7 6 6"/><circle cx="17.5" cy="9.5" r="2.3"/><path d="M15 20c0-2.4 1.3-4.5 3.2-5.4"/>',
  file: '<path d="M7 3h7l4 4v14H7z"/><path d="M14 3v4h4"/><path d="M9.5 12h6M9.5 15.5h6"/>',
  terminal: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/>',
  logout: '<path d="M15 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h9"/><path d="M10 12h11M17 8l4 4-4 4"/>',
  fan: '<circle cx="12" cy="12" r="2.2"/><path d="M12 9.8c0-3.2 1.6-5 3.4-5 1.5 0 2.4 1.2 2.4 2.4 0 1.9-2.6 2.6-5.8 2.6zM12 14.2c0 3.2-1.6 5-3.4 5-1.5 0-2.4-1.2-2.4-2.4 0-1.9 2.6-2.6 5.8-2.6zM9.8 12c-3.2 0-5-1.6-5-3.4 0-1.5 1.2-2.4 2.4-2.4 1.9 0 2.6 2.6 2.6 5.8zM14.2 12c3.2 0 5 1.6 5 3.4 0 1.5-1.2 2.4-2.4 2.4-1.9 0-2.6-2.6-2.6-5.8z"/>',
};

function icon(name, extraClass = '') {
  return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" class="${extraClass}" aria-hidden="true">${ICONS[name] || ''}</svg>`;
}

const NAV = [
  { id: 'monitor', label: 'Monitor', icon: 'activity' },
  { id: 'stream', label: 'Stream', icon: 'sliders' },
  { id: 'applications', label: 'Apps', icon: 'grid' },
  { id: 'gpu', label: 'GPU', icon: 'cpu' },
  { id: 'settings', label: 'Settings', icon: 'sliders' },
  { id: 'pairing', label: 'Pin', icon: 'key' },
  { id: 'clients', label: 'Clients', icon: 'users' },
  { id: 'terminal', label: 'Terminal', icon: 'terminal' },
];
const DEFAULT_PAGE = 'monitor';

/** @brief Build an inline SVG sparkline from a rolling value buffer. */
function sparkline(values, max = 100) {
  const w = 100;
  const h = 30;
  const pts = values
    .map((v, i) => {
      if (v == null) return null;
      const x = (i / Math.max(1, values.length - 1)) * w;
      const y = h - (Math.min(v, max) / max) * h;
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    })
    .filter(Boolean)
    .join(' ');
  if (!pts) return '<div class="sparkline-empty"></div>';
  return `<svg class="sparkline" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" aria-hidden="true"><polygon points="0,${h} ${pts} ${w},${h}" fill="currentColor" opacity="0.12"/><polyline points="${pts}" fill="none" stroke="currentColor" stroke-width="1.6" vector-effect="non-scaling-stroke"/></svg>`;
}

/** @brief Tone class (chrome/amber/red) for a percentage-based reading. */
function toneFor(percent) {
  if (percent == null) return 'tone-neutral';
  if (percent >= 90) return 'tone-danger';
  if (percent >= 72) return 'tone-warn';
  return 'tone-neutral';
}

/** @brief Render one metric tile (label + value + sparkline + temp/fan sub-row). */
function metricTile({ label, value, percent, values, sub, temp, fan }) {
  const tone = toneFor(percent);
  return `<div class="metric-tile ${tone}">
    <div class="metric-tile-head"><span class="metric-label">${escapeHtml(label)}</span><span class="metric-value num">${value == null ? '—' : escapeHtml(value)}</span></div>
    ${sparkline(values || [])}
    <div class="metric-sub num">${sub ? `<span>${escapeHtml(sub)}</span>` : ''}${temp ? `<span>${escapeHtml(temp)}</span>` : ''}${fan ? `<span class="fan">${icon('fan')}${escapeHtml(fan)} RPM</span>` : ''}</div>
  </div>`;
}

/** @brief Render the shared SteamShine shell around a page's content. */
function shell(content, { authenticated = false, activeId = '' } = {}) {
  if (!authenticated) {
    app.innerHTML = `<div class="shell unauth">${content}</div>`;
    return;
  }
  const navLinks = NAV.map((item) => `<a class="nav-link${item.id === activeId ? ' active' : ''}" href="/steamshine/${item.id}" data-nav="${item.id}">${icon(item.icon)}<span>${item.label}</span></a>`).join('');
  app.innerHTML = `<div class="shell">
    <header class="mobile-topbar">
      <img src="/steamshine/images/logo-mark-64.png" alt="SteamShine">
      <span>SteamShine</span>
      <button id="mobile-logout" class="icon-btn" aria-label="Log out">${icon('logout')}</button>
    </header>
    <nav class="sidenav">
      <div class="brand"><img src="/steamshine/images/logo-mark-64.png" alt="SteamShine"><div class="brand-text"><h1>SteamShine</h1><p>Sunshine for SteamOS</p></div></div>
      ${navLinks}
      <div class="nav-spacer"></div>
      <div class="nav-foot"><button id="logout" class="nav-link" style="width:100%">${icon('logout')}<span>Log out</span></button></div>
    </nav>
    <main class="main">${content}</main>
  </div>`;
  document.querySelectorAll('[data-nav]').forEach((a) => a.addEventListener('click', (e) => { e.preventDefault(); navigate(a.getAttribute('href')); }));
  document.querySelector('#logout')?.addEventListener('click', logout);
  document.querySelector('#mobile-logout')?.addEventListener('click', logout);
}

/** @brief Redirect to a SteamShine route. */
function navigate(path) { history.pushState({}, '', path); render().catch(showError); }

/** @brief Render a safe error state. */
function showError(error) {
  stopPolling();
  shell(`<div class="auth-card"><div class="brand"><img src="/steamshine/images/logo-mark-64.png" alt="SteamShine"><div class="brand-text"><h1>SteamShine is unavailable</h1></div></div><p class="notice error">${escapeHtml(error.message || error)}</p><button id="retry" class="btn-primary">Try again</button></div>`);
  document.querySelector('#retry').onclick = () => render().catch(showError);
}

/** @brief Render the initial credential setup view. */
function renderSetup() {
  shell(`<div class="auth-card">
    <div class="brand"><img src="/steamshine/images/logo-mark-64.png" alt="SteamShine"><div class="brand-text"><h1>Create shared credentials</h1><p>These credentials also work in the Sunshine Web UI.</p></div></div>
    <form id="setup">
      <label>Username<input name="username" maxlength="64" required autocomplete="username"></label>
      <label>Password<input name="password" type="password" required autocomplete="new-password"></label>
      <label>Confirm password<input name="confirm_password" type="password" required autocomplete="new-password"></label>
      <button class="btn-primary">Create credentials</button>
      <div class="notice"></div>
    </form></div>`);
  document.querySelector('#setup').onsubmit = async (event) => {
    event.preventDefault();
    const form = new FormData(event.currentTarget);
    try { await json(await api('/setup/credentials', { method: 'POST', body: JSON.stringify(Object.fromEntries(form)) })); navigate('/steamshine/login'); }
    catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; }
  };
}

/** @brief Render the login view. */
function renderLogin() {
  shell(`<div class="auth-card">
    <div class="brand"><img src="/steamshine/images/logo-mark-64.png" alt="SteamShine"><div class="brand-text"><h1>Sign in</h1></div></div>
    <form id="login">
      <label>Username<input name="username" required autocomplete="username"></label>
      <label>Password<input name="password" type="password" required autocomplete="current-password"></label>
      <button class="btn-primary">Sign in</button>
      <div class="notice"></div>
    </form>
    <p class="auth-foot"><a href="/sunshine/">Open the Sunshine Web UI</a></p>
    </div>`);
  document.querySelector('#login').onsubmit = async (event) => {
    event.preventDefault();
    try {
      const data = await json(await api('/auth/login', { method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(event.currentTarget))) }));
      csrfToken = data.csrf_token;
      navigate(`/steamshine/${DEFAULT_PAGE}`);
    } catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; }
  };
}

/** @brief Render an authenticated page for the current path. */
async function renderAuthenticated(session) {
  csrfToken = session.csrf_token;
  stopPolling();
  const page = location.pathname.split('/').filter(Boolean)[1] || DEFAULT_PAGE;
  const renderers = {
    pairing: renderPairing,
    config: renderVirtualDisplayConfig,
    clients: renderClients,
    monitor: renderMonitor,
    stream: renderStreamNegotiation,
    applications: renderApplications,
    settings: renderSettings,
    gpu: renderGpu,
    terminal: renderTerminal,
  };
  return (renderers[page] || renderMonitor)(session);
}

/** @brief Render the PC monitor: CPU/RAM/GPU/VRAM metric tiles, core bars, GPU detail. */
async function renderMonitor() {
  const history = { cpu: [], ram: [], gpu: [], vram: [] };
  shell(`<div class="page-header"><div><h2>Monitor</h2><p>Live system telemetry, refreshed every 2 seconds.</p></div></div>
    <div id="monitor-root" class="stack"><div class="empty">Loading…</div></div>`, { authenticated: true, activeId: 'monitor' });

  const renderOnce = async () => {
    let m;
    try { m = await json(await api('/system/metrics')); } catch (error) { return; }
    const root = document.querySelector('#monitor-root');
    if (!root) return;
    history.cpu.push(m.cpu.percent); if (history.cpu.length > 60) history.cpu.shift();
    history.ram.push(m.memory.percent); if (history.ram.length > 60) history.ram.shift();
    history.gpu.push(m.gpu?.utilization_percent ?? null); if (history.gpu.length > 60) history.gpu.shift();
    const vramPct = m.gpu?.vram_used_bytes != null && m.gpu.vram_total_bytes ? (m.gpu.vram_used_bytes / m.gpu.vram_total_bytes) * 100 : null;
    history.vram.push(vramPct); if (history.vram.length > 60) history.vram.shift();

    const gb = (bytes) => (bytes / 1024 ** 3).toFixed(1);
    const tiles = [
      metricTile({ label: 'CPU', value: `${m.cpu.percent.toFixed(0)}%`, percent: m.cpu.percent, values: history.cpu, temp: m.cpu.temperature_c != null ? `${m.cpu.temperature_c.toFixed(0)}°C` : undefined }),
      metricTile({ label: 'RAM', value: `${m.memory.percent.toFixed(0)}%`, percent: m.memory.percent, values: history.ram, sub: `${gb(m.memory.used)} / ${gb(m.memory.total)} GB` }),
      metricTile({ label: 'GPU', value: m.gpu ? `${(m.gpu.utilization_percent ?? 0).toFixed(0)}%` : 'N/A', percent: m.gpu?.utilization_percent ?? null, values: history.gpu, temp: m.gpu?.temperature_c != null ? `${m.gpu.temperature_c.toFixed(0)}°C` : undefined, fan: m.gpu?.fan_rpm != null ? `${m.gpu.fan_rpm}` : undefined }),
      metricTile({ label: 'VRAM', value: vramPct != null ? `${vramPct.toFixed(0)}%` : 'N/A', percent: vramPct, values: history.vram, sub: vramPct != null ? `${gb(m.gpu.vram_used_bytes)} / ${gb(m.gpu.vram_total_bytes)} GB` : undefined }),
    ].join('');

    const coreBars = (m.cpu.per_cpu || []).map((p) => `<div class="core-bar${p >= 90 ? ' hot' : ''}" title="${p.toFixed(0)}%"><span style="height:${p}%"></span></div>`).join('');

    const gpuSection = m.gpu ? `<div class="section"><h3>GPU — ${escapeHtml(m.gpu.name)}</h3><div class="rows">
        <div class="row"><span class="k">Hotspot</span><span class="v num">${m.gpu.hotspot_c != null ? `${m.gpu.hotspot_c.toFixed(0)}°C` : 'N/A'}</span></div>
        <div class="row"><span class="k">Power draw / cap</span><span class="v num">${m.gpu.power_watts != null ? `${m.gpu.power_watts.toFixed(0)} W${m.gpu.power_cap_watts ? ` / ${m.gpu.power_cap_watts.toFixed(0)} W` : ''}` : 'N/A'}</span></div>
        <div class="row"><span class="k">Fan</span><span class="v num">${m.gpu.fan_rpm != null ? `${m.gpu.fan_rpm} RPM` : 'N/A'}</span></div>
      </div></div>` : '<div class="empty">No AMD GPU detected on this host.</div>';

    root.innerHTML = `<div class="grid">${tiles}</div>
      <div class="section"><h3>CPU cores</h3><div class="core-bars">${coreBars}</div>
        <div class="rows" style="margin-top:.7rem"><div class="row"><span class="k">Load average</span><span class="v num">${(m.cpu.load || []).map((l) => l.toFixed(2)).join(' / ')}</span></div><div class="row"><span class="k">Uptime</span><span class="v num">${formatUptime(m.uptime_seconds)}</span></div></div>
      </div>
      ${gpuSection}`;
  };
  await renderOnce();
  pollTimer = setInterval(renderOnce, 2000);
}

function formatUptime(seconds) {
  if (seconds == null) return '—';
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return d > 0 ? `${d}d ${h}h` : h > 0 ? `${h}h ${m}m` : `${m}m`;
}

/** @brief Format a bounded negotiation value for a compact status row. */
function streamValue(value) {
  if (value == null || value === '') return '—';
  if (Array.isArray(value)) return value.length ? value.join(', ') : 'None';
  if (typeof value === 'object') return Object.entries(value).map(([key, item]) => `${key}: ${item}`).join(', ');
  return String(value);
}

/** @brief Render one requested/selected/active/observed negotiation stage. */
function streamStage(title, values) {
  const rows = Object.entries(values || {}).map(([key, value]) => `<div class="row"><span class="k">${escapeHtml(key.replaceAll('_', ' '))}</span><span class="v num">${escapeHtml(streamValue(value))}</span></div>`).join('');
  return `<div class="section stream-stage"><h3>${escapeHtml(title)}</h3><div class="rows">${rows || '<div class="empty">Unavailable</div>'}</div></div>`;
}

/** @brief Render live four-stage negotiation state and bounded client/network profiles. */
async function renderStreamNegotiation() {
  shell(`<div class="page-header"><div><h2>Stream negotiation</h2><p>Requested, selected, active, and observed state. Live data refreshes every 2 seconds without owning the media path.</p></div><a class="btn-ghost btn-sm" href="/sunshine/config">Sunshine fallback settings</a></div>
    <div id="stream-state" class="grid-2"><div class="empty">Loading stream state…</div></div>
    <div class="section stack"><div><h3>Client / network profile</h3><p class="field-hint">Profiles match the paired client ID, network class, and current capability signature exactly. A changed capability signature always wins over saved preferences.</p></div>
      <form id="stream-profile-form" class="stack">
        <div class="form-grid">
          <label>Client ID<input name="client_id" maxlength="128" required placeholder="paired-client-id"></label>
          <label>Network class<input name="network_class" maxlength="32" required placeholder="lan, wifi-5ghz, tailscale"></label>
          <label>Capability signature<input name="capability_signature" maxlength="256" required placeholder="h264-hevc-av1-main10"></label>
          <label>Geometry policy<select name="geometry_policy"><option value="exact">Exact</option><option value="fit" selected>Fit</option><option value="virtual_fallback">Virtual fallback</option></select></label>
          <label>FPS policy<select name="fps_policy"><option value="auto">Automatic</option><option value="custom">Custom ceiling</option></select></label>
          <label>FPS ceiling<input name="fps_ceiling" type="number" min="0" max="240" value="0"></label>
          <label>Codec policy<select name="codec_policy"><option value="auto">Automatic</option><option value="h264">H.264</option><option value="hevc">HEVC</option><option value="av1">AV1</option></select></label>
          <label>HDR policy<select name="hdr_policy"><option value="off">Off</option><option value="auto" selected>Automatic</option><option value="require">Require</option></select></label>
          <label>Bitrate ceiling (Kbps)<input name="bitrate_ceiling_kbps" type="number" min="0" max="200000" value="0"></label>
          <label>Quality preset<select name="quality_preset"><option value="low_latency">Low latency</option><option value="balanced" selected>Balanced</option><option value="quality">Quality</option></select></label>
          <label>Orientation<select name="orientation"><option value="auto">Automatic</option><option value="landscape">Landscape</option><option value="portrait">Portrait</option></select></label>
          <label>Safe area (%)<input name="safe_area_percent" type="number" min="0" max="25" value="0"></label>
          <label><span>Use on next connection</span><input name="active" type="checkbox" checked></label>
        </div>
        <div class="btn-row"><button class="btn-primary">Save profile</button></div><div class="notice"></div>
      </form>
      <div id="stream-profiles"><div class="empty">Loading profiles…</div></div>
    </div>`, { authenticated: true, activeId: 'stream' });

  const loadProfiles = async () => {
    const profileDocument = await json(await api('/stream/profiles'));
    const profiles = profileDocument.profiles || [];
    const root = document.querySelector('#stream-profiles');
    if (!root) return;
    root.innerHTML = profiles.length ? `<table><thead><tr><th>Client / network</th><th>Policies</th><th></th></tr></thead><tbody>${profiles.map((profile) => `<tr><td><strong>${escapeHtml(profile.client_id)}</strong><br><span class="field-hint">${escapeHtml(profile.network_class)} · ${escapeHtml(profile.capability_signature)}${profile.active ? ' · next connection' : ''}</span></td><td>${escapeHtml(profile.geometry_policy)} · ${escapeHtml(profile.codec_policy)} · HDR ${escapeHtml(profile.hdr_policy)} · ${profile.bitrate_ceiling_kbps ? `${escapeHtml(profile.bitrate_ceiling_kbps)} Kbps` : 'auto bitrate'}</td><td style="text-align:right"><button class="btn-sm btn-danger" data-reset-client="${escapeHtml(profile.client_id)}" data-reset-network="${escapeHtml(profile.network_class)}">Reset</button></td></tr>`).join('')}</tbody></table>` : '<div class="empty">No learned stream profiles.</div>';
    root.querySelectorAll('[data-reset-client]').forEach((button) => button.onclick = async () => {
      if (!await confirmDialog({ title: 'Reset stream profile', message: `Reset ${button.dataset.resetClient} on ${button.dataset.resetNetwork}?` })) return;
      try {
        const result = await json(await api('/stream/profiles/reset', { method: 'POST', body: JSON.stringify({ client_id: button.dataset.resetClient, network_class: button.dataset.resetNetwork }) }));
        toast(result.message, 'ok');
        await loadProfiles();
      } catch (error) { toast(error.message, 'error'); }
    });
  };

  document.querySelector('#stream-profile-form').onsubmit = async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const data = Object.fromEntries(new FormData(form));
    const payload = { ...data, active: form.elements.active.checked, fps_ceiling: Number(data.fps_ceiling), bitrate_ceiling_kbps: Number(data.bitrate_ceiling_kbps), safe_area_percent: Number(data.safe_area_percent), learned_start_kbps: 0 };
    try {
      const result = await json(await api('/stream/profiles', { method: 'POST', body: JSON.stringify(payload) }));
      form.querySelector('.notice').textContent = result.message;
      form.querySelector('.notice').className = 'notice ok';
      await loadProfiles();
    } catch (error) {
      form.querySelector('.notice').textContent = error.message;
      form.querySelector('.notice').className = 'notice error';
    }
  };

  const renderState = async () => {
    let status;
    try { status = await json(await api('/status')); } catch (error) { return; }
    const state = status.stream_negotiation || {};
    const root = document.querySelector('#stream-state');
    if (!root) return;
    root.innerHTML = streamStage('Requested', state.requested) + streamStage('Selected', state.selected) + streamStage('Active', state.active) + streamStage('Observed', state.observed);
    const form = document.querySelector('#stream-profile-form');
    if (form && state.requested?.client_id && state.requested?.capability_signature) {
      if (!form.elements.client_id.value) form.elements.client_id.value = state.requested.client_id;
      if (!form.elements.capability_signature.value) form.elements.capability_signature.value = state.requested.capability_signature;
    }
  };
  await Promise.all([loadProfiles(), renderState()]);
  pollTimer = setInterval(renderState, 2000);
}

/** @brief Render the applications list: card grid + add/edit form + close-running control. */
async function renderApplications() {
  shell(`<div class="page-header"><div><h2>Applications</h2><p>Manage the app list Moonlight clients can launch. Launching itself happens from the Moonlight/Steam client — this page manages the registered list and can stop the currently running app.</p></div><button id="add-app" class="btn-primary">Add application</button></div>
    <div id="apps-root" class="grid-2"><div class="empty">Loading…</div></div>`, { authenticated: true, activeId: 'applications' });

  const load = async () => {
    const data = await json(await api('/apps'));
    const root = document.querySelector('#apps-root');
    const apps = data.apps || [];
    if (apps.length === 0) { root.innerHTML = '<div class="empty">No applications registered yet.</div>'; return; }
    root.innerHTML = apps.map((a) => `<div class="card">
        <div class="metric-tile-head"><h3 style="margin:0">${escapeHtml(a.name)}</h3>${a.running ? '<span class="status-line"><span class="status-dot dot-ok"></span>Running</span>' : '<span class="status-line"><span class="status-dot dot-idle"></span>Stopped</span>'}</div>
        <p class="field-hint num" style="margin:.4rem 0 .8rem">${escapeHtml(a.cmd || '(no command — Steam/detached app)')}</p>
        <div class="btn-row">
          <button class="btn-sm" data-edit="${a.index}">Edit</button>
          ${a.running ? `<button class="btn-sm btn-danger" data-close="${a.index}">Stop</button>` : ''}
          <button class="btn-sm btn-danger" data-delete="${a.index}">Delete</button>
        </div>
      </div>`).join('');
    root.querySelectorAll('[data-edit]').forEach((b) => b.onclick = () => openAppForm(apps[Number(b.dataset.edit)], Number(b.dataset.edit)));
    root.querySelectorAll('[data-close]').forEach((b) => b.onclick = async () => { try { await json(await api(`/apps/${b.dataset.close}/close`, { method: 'POST', body: '{}' })); toast('Stop requested', 'ok'); load(); } catch (e) { toast(e.message, 'error'); } });
    root.querySelectorAll('[data-delete]').forEach((b) => b.onclick = async () => {
      if (!await confirmDialog({ title: 'Delete application', message: `Remove "${escapeHtml(apps[Number(b.dataset.delete)].name)}" from the list?` })) return;
      try { await json(await api(`/apps/${b.dataset.delete}`, { method: 'DELETE' })); toast('Deleted', 'ok'); load(); } catch (e) { toast(e.message, 'error'); }
    });
  };

  const openAppForm = (existing, index) => {
    const backdrop = document.createElement('div');
    backdrop.className = 'modal-backdrop';
    backdrop.innerHTML = `<div class="modal" style="width:min(480px,100%)">
      <h3>${existing ? 'Edit' : 'Add'} application</h3>
      <form id="app-form">
        <label>Name<input name="name" required value="${escapeHtml(existing?.name || '')}"></label>
        <label>Command<input name="cmd" placeholder="/path/to/game or empty for Steam Big Picture" value="${escapeHtml(existing?.cmd || '')}"></label>
        <label>Image path<input name="image-path" value="${escapeHtml(existing?.['image-path'] || '')}"></label>
        <div class="checkbox-row"><label style="flex-direction:row-reverse;justify-content:flex-end">Elevated<input type="checkbox" name="elevated" ${existing?.elevated ? 'checked' : ''}></label></div>
        <div class="btn-row"><button type="button" class="btn-ghost" data-a="cancel">Cancel</button><button class="btn-primary">Save</button></div>
        <div class="notice"></div>
      </form>
      <p class="field-hint">For image upload, multiple prep-commands, or detached-command lists, use the <a href="/apps">full application editor</a>.</p>
    </div>`;
    document.body.appendChild(backdrop);
    backdrop.querySelector('[data-a="cancel"]').onclick = () => backdrop.remove();
    backdrop.querySelector('#app-form').onsubmit = async (event) => {
      event.preventDefault();
      const form = new FormData(event.currentTarget);
      const payload = { name: form.get('name'), cmd: form.get('cmd') || '', 'image-path': form.get('image-path') || '', elevated: form.get('elevated') === 'on', index: index ?? -1 };
      try { await json(await api('/apps', { method: 'POST', body: JSON.stringify(payload) })); backdrop.remove(); toast('Saved', 'ok'); load(); }
      catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; }
    };
  };

  document.querySelector('#add-app').onclick = () => openAppForm(null, -1);
  await load();
}

/** @brief Curated advanced-settings page over the shared /api/config store. */
async function renderSettings() {
  const config = await json(await api('/config'));
  const field = (key, label, type = 'text') => `<label>${escapeHtml(label)}<input data-key="${key}" data-kind="text" type="${type}" value="${escapeHtml(config[key] ?? '')}"></label>`;
  const check = (key, label) => `<div class="checkbox-row"><label style="flex-direction:row-reverse;justify-content:flex-end">${escapeHtml(label)}<input type="checkbox" data-key="${key}" data-kind="bool" ${['enabled', 'true', 'yes', 'on', '1'].includes(String(config[key]).toLowerCase()) ? 'checked' : ''}></label></div>`;
  shell(`<div class="page-header"><div><h2>Advanced settings</h2><p>The most commonly changed options. For everything else, open the <a href="/config">full configuration editor</a>.</p></div></div>
    <form id="settings-form" class="stack">
      <div class="section"><h3>General</h3><div class="form-grid">${field('sunshine_name', 'Host name')}${field('port', 'Port', 'number')}</div></div>
      <div class="section"><h3>Audio / Video</h3><div class="form-grid">${field('fec_percentage', 'FEC percentage', 'number')}${field('qp', 'Quantization parameter (QP)', 'number')}${field('adapter_name', 'GPU adapter name (blank = auto)')}${field('capture', 'Capture backend (blank = auto)')}</div></div>
      <div class="section"><h3>Network</h3>${check('upnp', 'Enable UPnP')}<div class="form-grid">${field('lan_encryption_mode', 'LAN encryption mode (0-2)', 'number')}</div></div>
      <div class="section"><h3>Advanced</h3><div class="form-grid">${field('min_log_level', 'Minimum log level (0-6)', 'number')}</div></div>
      <div class="btn-row"><button class="btn-primary">Save settings</button></div>
      <div class="notice"></div>
    </form>`, { authenticated: true, activeId: 'settings' });
  document.querySelector('#settings-form').onsubmit = async (event) => {
    event.preventDefault();
    const payload = {};
    event.currentTarget.querySelectorAll('[data-key]').forEach((input) => {
      payload[input.dataset.key] = input.dataset.kind === 'bool' ? (input.checked ? 'enabled' : 'disabled') : input.value;
    });
    try { await json(await api('/config', { method: 'POST', body: JSON.stringify(payload) })); event.currentTarget.querySelector('.notice').textContent = 'Saved.'; event.currentTarget.querySelector('.notice').className = 'notice ok'; }
    catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; event.currentTarget.querySelector('.notice').className = 'notice error'; }
  };
}

/** @brief AMD GPU performance profile page: presets + custom profile management. */
async function renderGpu() {
  const [caps, profiles] = await Promise.all([
    json(await api('/gpu/capabilities')),
    json(await api('/gpu/profiles')),
  ]);
  const root = document.createElement('div');
  shell(`<div class="page-header"><div><h2>GPU performance</h2><p>${caps.gpu_name ? `Detected: ${escapeHtml(caps.gpu_name)}` : 'No AMD GPU detected — profile controls are disabled.'}</p></div><button id="add-profile" class="btn-primary" ${caps.gpu_name ? '' : 'disabled'}>New profile</button></div>
    <div id="gpu-root"></div>`, { authenticated: true, activeId: 'gpu' });
  document.querySelector('#gpu-root').replaceWith(renderGpuBody(caps, profiles.profiles || [], profiles.active || ''));
  wireGpuHandlers(caps, profiles.profiles || []);
  document.querySelector('#add-profile')?.addEventListener('click', () => openProfileForm(caps, null));
}

function renderGpuBody(caps, profiles, active) {
  const wrap = document.createElement('div');
  wrap.id = 'gpu-root';
  wrap.className = 'stack';
  const cards = profiles.map((p) => `<div class="profile-card${p.name === active ? ' active' : ''}" data-activate="${escapeHtml(p.name)}">
      ${p.name === active ? '<span class="active-tag">ACTIVE</span>' : ''}
      <h4>${escapeHtml(p.name)}</h4>
      <p class="profile-desc">${escapeHtml(p.description || '')}</p>
      <div class="profile-meta"><span>${p.power_cap_watts}W</span><span>${escapeHtml(p.cpu_governor)}</span>${p.gpu_clock_offset_mhz ? `<span>+${p.gpu_clock_offset_mhz}MHz</span>` : ''}</div>
      ${!p.builtin ? `<div class="btn-row"><button class="btn-sm" data-edit="${escapeHtml(p.name)}">Edit</button><button class="btn-sm btn-danger" data-delete="${escapeHtml(p.name)}">Delete</button></div>` : ''}
    </div>`).join('');
  const capRow = (label, ok, detail) => `<div class="row"><span class="k">${escapeHtml(label)}</span><span class="v">${ok ? `<span class="badge badge-ok">supported</span>` : `<span class="badge badge-warn">unsupported</span>`}${detail ? ` <span class="field-hint">${escapeHtml(detail)}</span>` : ''}</span></div>`;
  wrap.innerHTML = `
    <div class="section"><h3>Profiles</h3><div class="profile-grid">${cards || '<div class="empty">No profiles.</div>'}</div></div>
    <div class="callout info">Write operations use a briefly-elevated capability to reach root-owned sysfs files, then drop it immediately. Unsupported fields are skipped rather than failing the whole profile.</div>
    <div class="section"><h3>Detected capabilities</h3><div class="rows">
      ${capRow('GPU power limit', caps.power_cap_supported, caps.power_cap_supported ? `${caps.power_cap_min_watts}–${caps.power_cap_max_watts} W` : undefined)}
      ${capRow('GPU performance level', caps.perf_level_supported)}
      ${capRow('GPU clock/voltage offset (overdrive)', caps.od_clk_voltage_supported, caps.od_clk_voltage_supported ? undefined : 'Not exposed by this GPU/driver — undervolt controls are disabled.')}
      ${capRow('CPU frequency scaling', caps.cpu_freq_supported, caps.cpu_freq_supported ? `${caps.cpu_min_freq_mhz}–${caps.cpu_max_freq_mhz} MHz, governors: ${(caps.cpu_governors || []).join(', ')}` : undefined)}
    </div></div>`;
  return wrap;
}

function wireGpuHandlers(caps, profiles) {
  const root = document.querySelector('#gpu-root');
  root.querySelectorAll('[data-activate]').forEach((el) => el.addEventListener('click', async (e) => {
    if (e.target.closest('[data-edit],[data-delete]')) return;
    try { const r = await json(await api(`/gpu/profiles/${encodeURIComponent(el.dataset.activate)}/activate`, { method: 'POST', body: '{}' })); toast(`Applied (${(r.applied || []).length} fields, ${(r.skipped || []).length} skipped)`, 'ok'); renderGpu(); }
    catch (error) { toast(error.message, 'error'); }
  }));
  root.querySelectorAll('[data-edit]').forEach((b) => b.addEventListener('click', (e) => { e.stopPropagation(); openProfileForm(caps, profiles.find((p) => p.name === b.dataset.edit)); }));
  root.querySelectorAll('[data-delete]').forEach((b) => b.addEventListener('click', async (e) => {
    e.stopPropagation();
    if (!await confirmDialog({ title: 'Delete profile', message: `Delete profile "${b.dataset.delete}"?` })) return;
    try { await json(await api(`/gpu/profiles/${encodeURIComponent(b.dataset.delete)}`, { method: 'DELETE' })); toast('Deleted', 'ok'); renderGpu(); }
    catch (error) { toast(error.message, 'error'); }
  }));
}

function openProfileForm(caps, existing) {
  const backdrop = document.createElement('div');
  backdrop.className = 'modal-backdrop';
  const odDisabled = caps.od_clk_voltage_supported ? '' : 'disabled';
  backdrop.innerHTML = `<div class="modal" style="width:min(480px,100%)">
    <h3>${existing ? 'Edit' : 'New'} profile</h3>
    <form id="profile-form" class="stack">
      <label>Name<input name="name" required value="${escapeHtml(existing?.name || '')}" ${existing ? 'readonly' : ''}></label>
      <label>Description<input name="description" value="${escapeHtml(existing?.description || '')}"></label>
      <div class="slider-row"><div class="slider-head"><span>Power limit</span><span class="v" id="pw-out">${existing?.power_cap_watts ?? caps.power_cap_default_watts} W</span></div>
        <input type="range" name="power_cap_watts" min="${caps.power_cap_min_watts || 0}" max="${caps.power_cap_max_watts || 0}" value="${existing?.power_cap_watts ?? (caps.power_cap_default_watts || 0)}" ${caps.power_cap_supported ? '' : 'disabled'}></div>
      <label>CPU governor<select name="cpu_governor">${(caps.cpu_governors || []).map((g) => `<option value="${g}" ${existing?.cpu_governor === g ? 'selected' : ''}>${g}</option>`).join('')}</select></label>
      <div class="slider-row"><div class="slider-head"><span>CPU max clock</span><span class="v" id="cpu-out">${existing?.cpu_max_freq_mhz ?? caps.cpu_max_freq_mhz} MHz</span></div>
        <input type="range" name="cpu_max_freq_mhz" min="${caps.cpu_min_freq_mhz || 0}" max="${caps.cpu_max_freq_mhz || 0}" value="${existing?.cpu_max_freq_mhz ?? (caps.cpu_max_freq_mhz || 0)}" ${caps.cpu_freq_supported ? '' : 'disabled'}></div>
      <div class="slider-row ${odDisabled ? 'unsupported' : ''}"><div class="slider-head"><span>GPU clock offset</span><span class="v" id="clk-out">${existing?.gpu_clock_offset_mhz ?? 0} MHz</span></div>
        <input type="range" name="gpu_clock_offset_mhz" min="-100" max="200" value="${existing?.gpu_clock_offset_mhz ?? 0}" ${odDisabled}></div>
      <div class="slider-row ${odDisabled ? 'unsupported' : ''}"><div class="slider-head"><span>GPU undervolt</span><span class="v" id="v-out">${existing?.gpu_voltage_offset_mv ?? 0} mV</span></div>
        <input type="range" name="gpu_voltage_offset_mv" min="-150" max="0" value="${existing?.gpu_voltage_offset_mv ?? 0}" ${odDisabled}></div>
      <div class="btn-row"><button type="button" class="btn-ghost" data-a="cancel">Cancel</button><button class="btn-primary">Save</button></div>
      <div class="notice"></div>
    </form></div>`;
  document.body.appendChild(backdrop);
  backdrop.querySelectorAll('input[type="range"]').forEach((r) => r.addEventListener('input', () => {
    const out = { power_cap_watts: 'pw-out', cpu_max_freq_mhz: 'cpu-out', gpu_clock_offset_mhz: 'clk-out', gpu_voltage_offset_mv: 'v-out' }[r.name];
    const unit = r.name === 'power_cap_watts' ? 'W' : r.name === 'cpu_max_freq_mhz' ? 'MHz' : r.name === 'gpu_clock_offset_mhz' ? 'MHz' : 'mV';
    if (out) backdrop.querySelector(`#${out}`).textContent = `${r.value} ${unit}`;
  }));
  backdrop.querySelector('[data-a="cancel"]').onclick = () => backdrop.remove();
  backdrop.querySelector('#profile-form').onsubmit = async (event) => {
    event.preventDefault();
    const form = new FormData(event.currentTarget);
    const payload = {
      name: form.get('name'), description: form.get('description') || '',
      power_cap_watts: Number(form.get('power_cap_watts')), cpu_governor: form.get('cpu_governor'),
      cpu_max_freq_mhz: Number(form.get('cpu_max_freq_mhz')), gpu_clock_offset_mhz: Number(form.get('gpu_clock_offset_mhz') || 0),
      gpu_voltage_offset_mv: Number(form.get('gpu_voltage_offset_mv') || 0),
    };
    try { await json(await api('/gpu/profiles', { method: 'POST', body: JSON.stringify(payload) })); backdrop.remove(); toast('Saved', 'ok'); renderGpu(); }
    catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; }
  };
}

/** @brief Render the restart-required SteamOS virtual-display policy form. */
async function renderVirtualDisplayConfig() {
  const config = await json(await api('/config/virtual-display'));
  const candidates = await json(await api('/config/virtual-display/sources')).catch(() => ({ sources: [] }));
  const enabled = config.steamos_virtual_display_enabled === 'enabled';
  const sourceOptions = (candidates.sources || []).map((source) => `<option value="${escapeHtml(source.pid)}">PID ${escapeHtml(source.pid)} — ${escapeHtml(source.description || 'Gamescope')} (${escapeHtml(source.render_node || 'unknown GPU')})</option>`).join('');
  shell(`<div class="page-header"><div><h2>Virtual display</h2><p>Choose how SteamShine obtains the streamed display.</p></div></div>
    <form id="virtual-display-config" class="section stack">
      <div class="checkbox-row"><label style="flex-direction:row-reverse;justify-content:flex-end">Enable SteamOS virtual display<input name="enabled" type="checkbox" ${enabled ? 'checked' : ''}></label></div>
      <label>Virtual display mode<select name="mode"><option value="off" ${config.steamos_virtual_display_mode === 'off' ? 'selected' : ''}>Off</option><option value="auto" ${config.steamos_virtual_display_mode === 'auto' ? 'selected' : ''}>Auto</option><option value="force" ${config.steamos_virtual_display_mode === 'force' ? 'selected' : ''}>Force</option></select></label>
      <label>Gamescope session source<select name="session_source"><option value="auto" ${config.steamos_session_source === 'auto' ? 'selected' : ''}>Auto</option><option value="existing_gamescope" ${config.steamos_session_source === 'existing_gamescope' ? 'selected' : ''}>Existing Game Mode only</option><option value="owned_private" ${config.steamos_session_source === 'owned_private' ? 'selected' : ''}>SteamShine private session only</option></select></label>
      <label>Local presentation<select name="local_presentation"><option value="auto" ${config.steamos_local_presentation === 'auto' ? 'selected' : ''}>Auto</option><option value="off" ${config.steamos_local_presentation === 'off' ? 'selected' : ''}>Off</option><option value="mirror" ${config.steamos_local_presentation === 'mirror' ? 'selected' : ''}>Mirror</option></select></label>
      <label>Existing Gamescope PID (0 = automatic)<input name="existing_gamescope_pid" type="number" min="0" step="1" list="gamescope-source-pids" value="${escapeHtml(config.steamos_existing_gamescope_pid || '0')}"></label>
      <datalist id="gamescope-source-pids">${sourceOptions}</datalist>
      <p class="field-hint">${sourceOptions ? 'Verified resident Game Mode candidates are listed above.' : 'No verified resident Game Mode candidate is currently available.'}</p>
      <div class="checkbox-row"><label style="flex-direction:row-reverse;justify-content:flex-end">Keep a SteamShine-owned session after disconnect<input name="keep_session_alive" type="checkbox" ${config.steamos_keep_session_alive !== 'disabled' ? 'checked' : ''}></label></div>
      <div class="btn-row"><button class="btn-primary">Save policy</button></div>
      <div class="notice"></div>
    </form>`, { authenticated: true, activeId: 'settings' });
  document.querySelector('#virtual-display-config').onsubmit = async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    try {
      const result = await json(await api('/config/virtual-display', { method: 'POST', body: JSON.stringify({ enabled: form.elements.enabled.checked, mode: form.elements.mode.value, session_source: form.elements.session_source.value, local_presentation: form.elements.local_presentation.value, keep_session_alive: form.elements.keep_session_alive.checked, existing_gamescope_pid: Number(form.elements.existing_gamescope_pid.value) }) }));
      form.querySelector('.notice').textContent = result.message; form.querySelector('.notice').className = 'notice ok';
    } catch (error) { form.querySelector('.notice').textContent = error.message; form.querySelector('.notice').className = 'notice error'; }
  };
}

/** @brief Render the Moonlight pairing (Pin) page. */
function renderPairing() {
  shell(`<div class="page-header"><div><h2>Pin pairing</h2><p>Enter the 4-digit PIN shown by your Moonlight client.</p></div></div>
    <form id="pairing" class="section stack" style="max-width:26rem">
      <label>Client name<input name="name" maxlength="128" required></label>
      <label>Four digit PIN<input name="pin" inputmode="numeric" pattern="[0-9]{4}" maxlength="4" minlength="4" required></label>
      <button class="btn-primary">Submit PIN</button>
      <div class="notice"></div>
    </form>`, { authenticated: true, activeId: 'pairing' });
  document.querySelector('#pairing').onsubmit = async (event) => {
    event.preventDefault();
    try { const data = await json(await api('/pairing/pin', { method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(event.currentTarget))) })); event.currentTarget.querySelector('.notice').textContent = data.message; event.currentTarget.querySelector('.notice').className = 'notice ok'; }
    catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; event.currentTarget.querySelector('.notice').className = 'notice error'; }
  };
}

/** @brief Render paired clients and their revoke actions. */
async function renderClients() {
  const data = await json(await api('/clients'));
  const clients = data.named_certs || [];
  shell(`<div class="page-header"><div><h2>Paired clients</h2><p>Moonlight clients that can currently connect.</p></div></div>
    <div class="section"><table><thead><tr><th>Name</th><th></th></tr></thead><tbody>${clients.map((client) => `<tr><td>${escapeHtml(client.name || client.uuid || 'Unknown client')}</td><td style="text-align:right"><button class="btn-sm btn-danger" data-client="${escapeHtml(client.uuid || '')}">Revoke</button></td></tr>`).join('') || '<tr><td colspan="2" class="empty">No paired clients</td></tr>'}</tbody></table></div>`,
  { authenticated: true, activeId: 'clients' });
  document.querySelectorAll('[data-client]').forEach((button) => button.addEventListener('click', async () => {
    if (!await confirmDialog({ title: 'Revoke client', message: `Revoke pairing for "${button.closest('tr').querySelector('td').textContent}"?` })) return;
    try { await json(await api(`/clients/${encodeURIComponent(button.dataset.client)}`, { method: 'DELETE' })); renderClients(); } catch (error) { showError(error); }
  }));
}

/** @brief Render the full PTY web terminal (xterm.js over WebSocket). */
async function renderTerminal() {
  const status = await json(await api('/terminal/status'));
  const wsOrigin = `https://${location.hostname}:${status.ws_port}/`;
  shell(`<div class="page-header"><div><h2>Terminal</h2><p>A real shell on the SteamShine host, running as the same unprivileged user as Sunshine.</p></div>
      <div class="btn-row"><button id="term-restart" class="btn-ghost btn-sm">Restart session</button></div></div>
    <div class="callout info">The terminal connects over a separate port (<span class="num">${status.ws_port}</span>) using the same self-signed certificate. If it stays disconnected, open <a href="${wsOrigin}" target="_blank" rel="noopener">${escapeHtml(wsOrigin)}</a> once in a new tab to accept the certificate, then come back here.</div>
    <div class="terminal-wrap">
      <div class="terminal-keybar">
        <button data-key="Escape">Esc</button><button data-key="Tab">Tab</button><button data-key="ControlLeft">Ctrl</button>
        <button data-key="ArrowUp">↑</button><button data-key="ArrowDown">↓</button><button data-key="ArrowLeft">←</button><button data-key="ArrowRight">→</button>
        <button data-seq="">^C</button><button data-seq="">^D</button>
      </div>
      <div id="term-host" class="terminal-host"></div>
      <div class="notice" id="term-notice"></div>
    </div>
    <link rel="stylesheet" href="/steamshine/vendor/xterm/xterm.css">`, { authenticated: true, activeId: 'terminal' });

  await loadScriptOnce('/steamshine/vendor/xterm/xterm.js');
  const host = document.querySelector('#term-host');
  const notice = document.querySelector('#term-notice');
  terminalInstance = new window.Terminal({ convertEol: true, fontSize: 13, theme: { background: '#050506', foreground: '#f2f3f5' } });
  terminalInstance.open(host);

  let ctrlActive = false;
  document.querySelectorAll('.terminal-keybar [data-key]').forEach((b) => b.onclick = () => {
    if (b.dataset.key === 'ControlLeft') { ctrlActive = !ctrlActive; b.classList.toggle('active', ctrlActive); return; }
    const map = { Escape: '', Tab: '\t', ArrowUp: '[A', ArrowDown: '[B', ArrowLeft: '[D', ArrowRight: '[C' };
    sendTerminalInput(map[b.dataset.key] || '');
  });
  document.querySelectorAll('.terminal-keybar [data-seq]').forEach((b) => b.onclick = () => sendTerminalInput(b.dataset.seq));

  const connect = () => {
    const socket = new WebSocket(`wss://${location.hostname}:${status.ws_port}/api/steamshine/v1/terminal/stream`);
    socket.binaryType = 'arraybuffer';
    terminalSocket = socket;
    socket.onopen = () => { socket.send(JSON.stringify({ type: 'auth', csrf_token: csrfToken })); notice.textContent = ''; notice.className = 'notice'; };
    socket.onmessage = (event) => {
      if (typeof event.data === 'string') return;
      terminalInstance.write(new Uint8Array(event.data));
    };
    socket.onclose = () => { notice.textContent = 'Disconnected. Reconnecting…'; notice.className = 'notice'; setTimeout(connect, 1500); };
    socket.onerror = () => { notice.textContent = 'Connection error.'; notice.className = 'notice error'; };
  };
  connect();

  terminalInstance.onData((data) => sendTerminalInput(data));
  terminalInstance.onResize(({ cols, rows }) => { if (terminalSocket?.readyState === WebSocket.OPEN) terminalSocket.send(JSON.stringify({ type: 'resize', cols, rows })); });

  document.querySelector('#term-restart').onclick = async () => {
    if (!await confirmDialog({ title: 'Restart terminal session', message: 'This ends the current shell session and starts a new one.' })) return;
    try { await json(await api('/terminal/stop', { method: 'POST', body: '{}' })); terminalInstance.clear(); } catch (error) { toast(error.message, 'error'); }
  };
}

function sendTerminalInput(data) {
  if (terminalSocket?.readyState === WebSocket.OPEN) terminalSocket.send(JSON.stringify({ type: 'input', data }));
}

const loadedScripts = new Set();
function loadScriptOnce(src) {
  if (loadedScripts.has(src)) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const el = document.createElement('script');
    el.src = src;
    el.onload = () => { loadedScripts.add(src); resolve(); };
    el.onerror = reject;
    document.body.appendChild(el);
  });
}

/** @brief Invalidate the current session and return to login. */
async function logout() {
  stopPolling();
  if (terminalSocket) { terminalSocket.close(); terminalSocket = null; }
  try { await json(await api('/auth/logout', { method: 'POST', body: '{}' })); } finally { csrfToken = ''; navigate('/steamshine/login'); }
}

/** @brief Choose a setup, login, or authenticated view from the shared facade state. */
async function render() {
  const setup = await json(await api('/setup/status'));
  if (!setup.configured) return renderSetup();
  const sessionResponse = await api('/session');
  if (sessionResponse.status === 401) return renderLogin();
  await renderAuthenticated(await json(sessionResponse));
}

window.addEventListener('popstate', () => render().catch(showError));
render().catch(showError);
