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
  play: '<path d="M8 5.5v13l11-6.5z"/>',
  display: '<rect x="3" y="4.5" width="18" height="12" rx="2"/><path d="M9 20h6M12 16.5V20"/>',
  gear: '<circle cx="12" cy="12" r="3.2"/><path d="M12 3v2.2M12 18.8V21M4.2 7.5l1.9 1.1M17.9 15.4l1.9 1.1M4.2 16.5l1.9-1.1M17.9 8.6l1.9-1.1"/>',
  close: '<path d="M6 6l12 12M18 6L6 18"/>',
  fan: '<circle cx="12" cy="12" r="2.2"/><path d="M12 9.8c0-3.2 1.6-5 3.4-5 1.5 0 2.4 1.2 2.4 2.4 0 1.9-2.6 2.6-5.8 2.6zM12 14.2c0 3.2-1.6 5-3.4 5-1.5 0-2.4-1.2-2.4-2.4 0-1.9 2.6-2.6 5.8-2.6zM9.8 12c-3.2 0-5-1.6-5-3.4 0-1.5 1.2-2.4 2.4-2.4 1.9 0 2.6 2.6 2.6 5.8zM14.2 12c3.2 0 5 1.6 5 3.4 0 1.5-1.2 2.4-2.4 2.4-1.9 0-2.6-2.6-2.6-5.8z"/>',
};

function icon(name, extraClass = '') {
  return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" class="${extraClass}" aria-hidden="true">${ICONS[name] || ''}</svg>`;
}

const NAV = [
  { id: 'monitor', label: 'Monitor', icon: 'activity' },
  { id: 'stream', label: 'Stream', icon: 'play' },
  { id: 'applications', label: 'Apps', icon: 'grid' },
  { id: 'gpu', label: 'GPU', icon: 'cpu' },
  { id: 'config', label: 'Display', icon: 'display' },
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
function metricTile({ label, value, percent, values, sub, temp, fan, max = 100, tone }) {
  const toneClass = tone ? `tone-${tone}` : toneFor(percent);
  return `<div class="metric-tile ${toneClass}">
    <div class="metric-tile-head"><span class="metric-label">${escapeHtml(label)}</span><span class="metric-value num">${value == null ? '—' : escapeHtml(value)}</span></div>
    ${sparkline(values || [], max)}
    <div class="metric-sub num">${sub ? `<span>${escapeHtml(sub)}</span>` : ''}${temp ? `<span class="metric-temp">${escapeHtml(temp)}</span>` : ''}${fan ? `<span class="fan">${icon('fan')}${escapeHtml(fan)} RPM</span>` : ''}</div>
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
    const button = event.currentTarget.querySelector('button[type="submit"], button:not([type])');
    const notice = event.currentTarget.querySelector('.notice');
    button.disabled = true;
    notice.textContent = '';
    notice.className = 'notice';
    try {
      await json(await api('/auth/login', { method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(event.currentTarget))) }));
      window.location.assign(`/steamshine/${DEFAULT_PAGE}`);
    } catch (error) {
      notice.textContent = error.message;
      notice.className = 'notice error';
      button.disabled = false;
    }
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
    stream: renderStream,
    applications: renderApplications,
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

/** @brief Format a rational frame rate as a short decimal without trailing zeros. */
function formatRate(rate) {
  const numerator = Number(rate?.numerator) || 0;
  const denominator = Number(rate?.denominator) || 0;
  if (!numerator || !denominator) return null;
  const value = numerator / denominator;
  return Number.isInteger(value) ? String(value) : value.toFixed(2).replace(/0$/, '');
}

/** @brief Format a geometry object as "1920 × 1080 @ 60". */
function formatGeometry(geometry) {
  const width = Number(geometry?.width) || 0;
  const height = Number(geometry?.height) || 0;
  if (!width || !height) return null;
  const rate = formatRate(geometry.frame_rate);
  return `${width} × ${height}${rate ? ` @ ${rate}` : ''}`;
}

/** @brief Format a bit rate given in bits per second as Mbps. */
function formatMbps(bitsPerSecond) {
  const value = Number(bitsPerSecond) || 0;
  if (value <= 0) return null;
  return `${(value / 1e6).toFixed(value < 1e7 ? 1 : 0)} Mbps`;
}

/** @brief Build one label/value row, dropping rows whose value is unknown. */
function specRow(label, value) {
  if (value == null || value === '') return '';
  return `<div class="row"><span class="k">${escapeHtml(label)}</span><span class="v num">${escapeHtml(value)}</span></div>`;
}

/** @brief Human wording for the capture source a stream is running on. */
const SOURCE_LABELS = {
  owned_private: 'SteamShine private session',
  existing_gamescope: 'Steam Game Mode session',
  physical: 'Attached display',
  retained_owned: 'Retained private session',
};

/** @brief Human wording for the adaptive-bitrate congestion state. */
const CONGESTION_LABELS = {
  clean: { text: 'Stable', tone: 'ok' },
  warning: { text: 'Easing off', tone: 'warn' },
  congested: { text: 'Congested', tone: 'danger' },
  recovering: { text: 'Recovering', tone: 'warn' },
  unknown: { text: 'Measuring', tone: 'idle' },
};

/** @brief Turn a stable snake_case diagnostics reason into readable prose. */
function humanizeReason(reason) {
  return String(reason || '').replaceAll('_', ' ').replace(/^./, (character) => character.toUpperCase());
}

/** @brief Format a byte count for recording capacity and completed-file rows. */
function formatBytes(bytes) {
  const value = Number(bytes) || 0;
  if (value < 1024) return `${value} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let scaled = value / 1024;
  let unit = units[0];
  for (let index = 1; index < units.length && scaled >= 1024; index += 1) { scaled /= 1024; unit = units[index]; }
  return `${scaled.toFixed(scaled >= 10 ? 1 : 2)} ${unit}`;
}

/** @brief Render the idle banner shown while no client is streaming. */
function streamIdleBanner(status) {
  const encoder = status?.encoder ? `${status.encoder} encoder ready` : 'Encoder ready';
  return `<div class="stream-banner idle">
    <span class="status-dot dot-idle"></span>
    <div class="stream-banner-text"><strong>No client is streaming</strong><span>${escapeHtml(encoder)} — start a stream from Moonlight and this page fills in.</span></div>
  </div>`;
}

/** @brief Render the live banner naming the geometry, codec, and link state. */
function streamLiveBanner(state, congestionState) {
  const geometry = formatGeometry(state.active?.encode_geometry) || formatGeometry(state.selected?.encode_geometry) || formatGeometry(state.requested?.stream_geometry);
  const codec = (state.active?.codec_name || state.selected?.codec_name || '').toUpperCase();
  const range = state.active?.color?.hdr_active ? 'HDR10' : 'SDR';
  const summary = [geometry, codec, range].filter(Boolean).join('  ·  ');
  const congestion = CONGESTION_LABELS[congestionState] || CONGESTION_LABELS.unknown;
  return `<div class="stream-banner live">
    <span class="status-dot dot-ok"></span>
    <div class="stream-banner-text"><strong>Streaming now</strong><span>${escapeHtml(summary || 'Negotiating…')}</span></div>
    <span class="badge${congestion.tone === 'idle' ? '' : ` badge-${congestion.tone}`}">${escapeHtml(congestion.text)}</span>
  </div>`;
}

/** @brief Render the four headline tiles for an active stream. */
function streamTiles(state, adaptive, history) {
  const observed = state.observed || {};
  const actualMbps = (Number(observed.actual_bitrate_bps) || 0) / 1e6;
  const targetMbps = (Number(observed.target_bitrate_bps) || 0) / 1e6;
  const encodeFps = Number(observed.encode_fps) || 0;
  const sourceFps = Number(observed.source_fps) || 0;
  const latency = Number(observed.network_age_p99_ms) || 0;
  const lost = Number(adaptive.lost_packets) || 0;

  const lossDelta = Math.max(0, lost - history.lastLost);
  history.lastLost = lost;
  history.bitrate.push(actualMbps);
  history.fps.push(encodeFps);
  history.latency.push(latency);
  history.loss.push(lossDelta);
  ['bitrate', 'fps', 'latency', 'loss'].forEach((name) => { if (history[name].length > 60) history[name].shift(); });

  const latencyTone = latency >= 60 ? 'danger' : latency >= 30 ? 'warn' : 'ok';
  const lossTone = lossDelta > 0 ? 'warn' : 'ok';

  return `<div class="grid">
    ${metricTile({
      label: 'Bitrate',
      value: formatMbps(observed.actual_bitrate_bps),
      values: history.bitrate,
      max: Math.max(targetMbps, ...history.bitrate, 1) * 1.15,
      sub: targetMbps ? `target ${targetMbps.toFixed(targetMbps < 10 ? 1 : 0)} Mbps` : undefined,
      tone: 'neutral',
    })}
    ${metricTile({
      label: 'Frame rate',
      value: encodeFps ? `${encodeFps.toFixed(0)} fps` : null,
      values: history.fps,
      max: Math.max(sourceFps, ...history.fps, 1) * 1.15,
      sub: sourceFps ? `source ${sourceFps.toFixed(0)} fps` : undefined,
      tone: 'neutral',
    })}
    ${metricTile({
      label: 'Network latency',
      value: latency ? `${latency.toFixed(0)} ms` : null,
      values: history.latency,
      max: Math.max(60, ...history.latency),
      sub: observed.encode_age_p99_ms ? `encode ${Number(observed.encode_age_p99_ms).toFixed(0)} ms p99` : undefined,
      tone: latencyTone,
    })}
    ${metricTile({
      label: 'Packets lost',
      value: String(lost),
      values: history.loss,
      max: Math.max(4, ...history.loss),
      sub: lossDelta > 0 ? `+${lossDelta} in the last 2 s` : 'none in the last 2 s',
      tone: lossTone,
    })}
  </div>`;
}

/** @brief Render what the client receives and how well the link is holding up. */
function streamDetailSections(state, adaptive) {
  const selected = state.selected || {};
  const active = state.active || {};
  const requested = state.requested || {};
  const observed = state.observed || {};
  const color = active.color || selected.color || {};
  const codec = (active.codec_name || selected.codec_name || '').toUpperCase();
  const profile = active.profile || selected.profile || '';
  const encoder = active.backend || selected.backend || '';
  const requestedGeometry = formatGeometry(requested.stream_geometry);
  const deliveredGeometry = formatGeometry(active.encode_geometry) || formatGeometry(selected.encode_geometry);

  const delivering = [
    specRow('Client', requested.client_id),
    specRow('Delivered', deliveredGeometry),
    requestedGeometry && requestedGeometry !== deliveredGeometry ? specRow('Client asked for', requestedGeometry) : '',
    specRow('Codec', codec ? `${codec}${profile ? ` · ${profile}` : ''}` : null),
    specRow('Dynamic range', color.hdr_active ? 'HDR10' : color.hdr_selected ? 'HDR10 selected, not active' : 'SDR'),
    specRow('Colour', color.bit_depth ? `${color.bit_depth}-bit${color.colorspace ? ` · ${color.colorspace}` : ''}` : null),
    specRow('Captured from', SOURCE_LABELS[selected.source_origin] || selected.source_origin),
    specRow('Encoder', encoder ? `${encoder}${active.render_node ? ` · ${active.render_node}` : ''}` : null),
  ].join('');

  const congestion = CONGESTION_LABELS[adaptive.state] || CONGESTION_LABELS.unknown;
  const mbps = (kbps) => (Number(kbps) ? `${(Number(kbps) / 1000).toFixed(1)} Mbps` : null);
  const quality = [
    specRow('Link', congestion.text),
    specRow('Bitrate target', mbps(adaptive.target_kbps) || formatMbps(observed.target_bitrate_bps)),
    specRow('Bitrate ceiling', mbps(adaptive.maximum_kbps)),
    specRow('Learned for next session', mbps(adaptive.learned_next_kbps)),
    specRow('Bitrate adjustments', adaptive.enabled ? `${Number(adaptive.updates_applied) || 0} applied` : 'Adaptive bitrate off'),
    specRow('Frames waiting', `${Number(observed.capture_queue_frames) || 0} capture · ${Number(observed.encoder_queue_frames) || 0} encode · ${Number(observed.network_queue_frames) || 0} network`),
    specRow('Capture age p99', observed.capture_age_p99_ms ? `${Number(observed.capture_age_p99_ms).toFixed(0)} ms` : null),
  ].join('');

  return `<div class="grid-2">
    <div class="section"><h3>Delivering to Moonlight</h3><div class="rows">${delivering || '<div class="empty">Not negotiated yet.</div>'}</div></div>
    <div class="section"><h3>Connection quality</h3><div class="rows">${quality || '<div class="empty">No measurements yet.</div>'}</div></div>
  </div>`;
}

/** @brief Render why the delivered stream differs from the client request. */
function streamAdjustments(state, adaptive) {
  const reasons = [...(state.fallback_reasons || [])];
  const outputReason = state.observed?.output_status_reason;
  if (outputReason && outputReason !== 'ok') reasons.push(outputReason);
  if (adaptive.reason && adaptive.state && !['clean', 'unknown'].includes(adaptive.state)) reasons.push(adaptive.reason);
  const unique = [...new Set(reasons.filter(Boolean))];
  if (!unique.length) return '';
  return `<div class="section"><h3>Adjustments</h3>
    <p class="field-hint">Why the delivered stream differs from what the client asked for.</p>
    <div class="reason-list">${unique.map((reason) => `<span class="badge badge-warn">${escapeHtml(humanizeReason(reason))}</span>`).join('')}</div></div>`;
}

/** @brief Render the live stream page: headline tiles, delivered settings, quality, and recording. */
async function renderStream() {
  const history = { bitrate: [], fps: [], latency: [], loss: [], lastLost: 0 };
  let connectedClientId = '';
  shell(`<div class="page-header">
      <div><h2>Stream</h2><p>What SteamShine is sending to Moonlight right now, refreshed every 2 seconds.</p></div>
      <div class="btn-row">
        <button id="open-stream-profiles" class="icon-btn" type="button" aria-label="Client profiles" title="Client profiles">${icon('gear')}</button>
        <a class="btn-ghost btn-sm" href="/sunshine/config">Sunshine settings</a>
      </div>
    </div>
    <div id="stream-live" class="stack"><div class="empty">Loading stream state…</div></div>
    <div class="section stack">
      <div class="metric-tile-head"><div><h3 style="margin:0">Recording</h3><p class="field-hint">Saves the exact encoded video already being sent to the client. No second encoder, no audio.</p></div><button id="recording-toggle" class="btn-primary" type="button">Loading…</button></div>
      <div id="recording-status" class="status-line">Loading recording state…</div>
      <form id="recording-capacity-form" class="btn-row">
        <label style="max-width:16rem">Storage limit (MB)<input name="capacity_mb" type="number" min="1" max="102400" value="500" required></label>
        <button class="btn-ghost" type="submit">Save limit</button>
      </form>
      <video id="recording-player" class="recording-player" controls preload="metadata" hidden></video>
      <div id="recordings-list"><div class="empty">Loading recordings…</div></div>
    </div>`, { authenticated: true, activeId: 'stream' });

  let recordingState = null;
  const renderRecordings = (state) => {
    recordingState = state;
    const status = document.querySelector('#recording-status');
    const toggle = document.querySelector('#recording-toggle');
    const input = document.querySelector('#recording-capacity-form input');
    const root = document.querySelector('#recordings-list');
    if (!status || !toggle || !input || !root) return;
    toggle.disabled = state.state === 'finalizing';
    toggle.textContent = state.enabled ? 'Stop recording' : 'Start recording';
    toggle.className = state.enabled ? 'btn-danger' : 'btn-primary';
    status.innerHTML = `<span class="status-dot ${state.state === 'recording' ? 'dot-ok' : state.state === 'error' ? 'dot-danger' : 'dot-idle'}"></span>${escapeHtml(state.state)} · ${formatBytes(state.used_bytes)} / ${formatBytes(state.capacity_bytes)}${state.last_error ? ` · ${escapeHtml(state.last_error)}` : ''}`;
    if (document.activeElement !== input) input.value = state.capacity_mb;
    const recordings = state.recordings || [];
    root.innerHTML = recordings.length ? `<table><thead><tr><th>Recorded</th><th>Video</th><th>Size</th><th></th></tr></thead><tbody>${recordings.map((item) => `<tr><td>${escapeHtml(new Date(item.created_at_unix_ms || 0).toLocaleString())}</td><td>${escapeHtml(item.codec || 'video')} · ${escapeHtml(item.width || '—')}×${escapeHtml(item.height || '—')} · ${escapeHtml(item.fps || '—')} fps${item.hdr ? ' · HDR' : ''}</td><td class="num">${formatBytes(item.size_bytes)}</td><td style="text-align:right"><div class="btn-row"><button class="btn-sm" type="button" data-watch-recording="${escapeHtml(item.id)}">Watch</button><a class="btn-sm btn-ghost" href="/api/steamshine/v1/stream/recordings/${encodeURIComponent(item.id)}/download">Download</a><button class="btn-sm btn-danger" type="button" data-delete-recording="${escapeHtml(item.id)}">Delete</button></div></td></tr>`).join('')}</tbody></table>` : '<div class="empty">No completed recordings.</div>';
    root.querySelectorAll('[data-watch-recording]').forEach((button) => button.onclick = () => {
      const player = document.querySelector('#recording-player');
      player.src = `/api/steamshine/v1/stream/recordings/${encodeURIComponent(button.dataset.watchRecording)}/video`;
      player.hidden = false;
      player.play().catch(() => {});
    });
    root.querySelectorAll('[data-delete-recording]').forEach((button) => button.onclick = async () => {
      if (!await confirmDialog({ title: 'Delete recording', message: 'Delete this completed recording permanently?' })) return;
      try {
        const result = await json(await api(`/stream/recordings/${encodeURIComponent(button.dataset.deleteRecording)}`, { method: 'DELETE' }));
        toast(result.message, 'ok');
        await loadRecordings();
      } catch (error) { toast(error.message, 'error'); }
    });
  };

  const loadRecordings = async () => renderRecordings(await json(await api('/stream/recordings')));

  document.querySelector('#recording-toggle').onclick = async () => {
    try {
      const result = await json(await api('/stream/recordings/toggle', { method: 'POST', body: JSON.stringify({ enabled: !recordingState?.enabled }) }));
      toast(result.message, 'ok');
      await loadRecordings();
    } catch (error) { toast(error.message, 'error'); }
  };
  document.querySelector('#recording-capacity-form').onsubmit = async (event) => {
    event.preventDefault();
    try {
      const capacity = Number(event.currentTarget.elements.capacity_mb.value);
      const result = await json(await api('/stream/recordings/settings', { method: 'POST', body: JSON.stringify({ capacity_mb: capacity }) }));
      toast(result.message, 'ok');
      await loadRecordings();
    } catch (error) { toast(error.message, 'error'); }
  };

  document.querySelector('#open-stream-profiles').onclick = () => openStreamProfiles(connectedClientId);

  const renderLive = async () => {
    let status;
    try { status = await json(await api('/status')); } catch (error) { return; }
    const root = document.querySelector('#stream-live');
    if (!root) return;
    const state = status.stream_negotiation || {};
    const adaptive = status.adaptive_bitrate || {};
    connectedClientId = state.requested?.client_id || '';
    root.innerHTML = state.available
      ? streamLiveBanner(state, adaptive.state) + streamTiles(state, adaptive, history) + streamDetailSections(state, adaptive) + streamAdjustments(state, adaptive)
      : streamIdleBanner(status);
  };

  const refreshLiveState = async () => Promise.all([renderLive(), loadRecordings().catch(() => {})]);
  await refreshLiveState();
  pollTimer = setInterval(refreshLiveState, 2000);
}

/** @brief Build the policy form for one automatically recorded client profile. */
function streamProfileEditor(profile) {
  const options = (name, entries, current) => `<select name="${name}">${entries.map(([value, text]) => `<option value="${value}"${current === value ? ' selected' : ''}>${escapeHtml(text)}</option>`).join('')}</select>`;
  return `<form id="profile-policy" class="stack">
    <h4>${escapeHtml(profile.client_id)}</h4>
    <div class="form-grid">
      <label>Network<input name="network_class" maxlength="32" required value="${escapeHtml(profile.network_class)}" placeholder="default, lan, wifi, vpn"></label>
      <label>Resolution${options('geometry_policy', [['fit', 'Fit (recommended)'], ['exact', 'Exact match'], ['virtual_fallback', 'Virtual fallback']], profile.geometry_policy)}</label>
      <label>Frame rate${options('fps_policy', [['auto', 'Automatic'], ['custom', 'Cap it']], profile.fps_policy)}</label>
      <label>Frame rate cap (0 = automatic)<input name="fps_ceiling" type="number" min="0" max="240" value="${Number(profile.fps_ceiling) || 0}"></label>
      <label>Codec${options('codec_policy', [['auto', 'Automatic'], ['h264', 'H.264'], ['hevc', 'HEVC'], ['av1', 'AV1']], profile.codec_policy)}</label>
      <label>HDR${options('hdr_policy', [['auto', 'Automatic'], ['off', 'Never'], ['require', 'Require']], profile.hdr_policy)}</label>
      <label>Bitrate cap (Kbps, 0 = automatic)<input name="bitrate_ceiling_kbps" type="number" min="0" max="200000" value="${Number(profile.bitrate_ceiling_kbps) || 0}"></label>
      <label>Priority${options('quality_preset', [['balanced', 'Balanced'], ['low_latency', 'Low latency'], ['quality', 'Quality']], profile.quality_preset)}</label>
      <label>Orientation${options('orientation', [['auto', 'Automatic'], ['landscape', 'Landscape'], ['portrait', 'Portrait']], profile.orientation)}</label>
      <label>Safe area (%)<input name="safe_area_percent" type="number" min="0" max="25" value="${Number(profile.safe_area_percent) || 0}"></label>
      <label><span>Use on the next connection</span><input name="active" type="checkbox"${profile.active ? ' checked' : ''}></label>
    </div>
    <div class="btn-row"><button class="btn-primary">Save</button><button type="button" class="btn-ghost" data-a="cancel-edit">Cancel</button><button type="button" class="btn-danger" data-a="forget">Forget this client</button></div>
    <div class="notice"></div>
  </form>`;
}

/** @brief Open the floating editor for clients recorded automatically on first connection. */
async function openStreamProfiles(connectedClientId = '') {
  const backdrop = document.createElement('div');
  backdrop.className = 'modal-backdrop';
  backdrop.innerHTML = `<div class="modal modal-wide" role="dialog" aria-label="Client profiles">
    <div class="modal-head"><h3>Client profiles</h3><button class="icon-btn" type="button" data-a="close" aria-label="Close">${icon('close')}</button></div>
    <p class="field-hint">Paired clients are recorded automatically the first time they start a stream, and everything stays automatic until you change it here.</p>
    <div id="profile-list" class="stack"><div class="empty">Loading recorded clients…</div></div>
    <div id="profile-editor"></div>
  </div>`;
  document.body.appendChild(backdrop);
  const close = () => backdrop.remove();
  backdrop.addEventListener('mousedown', (event) => { if (event.target === backdrop) close(); });
  backdrop.querySelector('[data-a="close"]').onclick = close;

  const list = backdrop.querySelector('#profile-list');
  const editor = backdrop.querySelector('#profile-editor');
  const keyOf = (profile) => `${profile.client_id}|${profile.network_class}`;

  const openEditor = (profile) => {
    editor.innerHTML = streamProfileEditor(profile);
    editor.querySelector('[data-a="cancel-edit"]').onclick = () => { editor.innerHTML = ''; };
    editor.querySelector('[data-a="forget"]').onclick = async () => {
      if (!await confirmDialog({ title: 'Forget client', message: `Forget the saved profile for ${profile.client_id}? It is recorded again the next time that client connects.` })) return;
      try {
        const result = await json(await api('/stream/profiles/reset', { method: 'POST', body: JSON.stringify({ client_id: profile.client_id, network_class: profile.network_class }) }));
        toast(result.message, 'ok');
        editor.innerHTML = '';
        await loadProfiles();
      } catch (error) { toast(error.message, 'error'); }
    };
    editor.querySelector('#profile-policy').onsubmit = async (event) => {
      event.preventDefault();
      const form = event.currentTarget;
      const data = Object.fromEntries(new FormData(form));
      const notice = form.querySelector('.notice');
      const payload = {
        ...data,
        client_id: profile.client_id,
        capability_signature: profile.capability_signature,
        active: form.elements.active.checked,
        fps_ceiling: Number(data.fps_ceiling),
        bitrate_ceiling_kbps: Number(data.bitrate_ceiling_kbps),
        safe_area_percent: Number(data.safe_area_percent),
        learned_start_kbps: Number(profile.learned_start_kbps) || 0,
      };
      try {
        const result = await json(await api('/stream/profiles', { method: 'POST', body: JSON.stringify(payload) }));
        notice.textContent = result.message;
        notice.className = 'notice ok';
        await loadProfiles(`${payload.client_id}|${payload.network_class}`);
      } catch (error) {
        notice.textContent = error.message;
        notice.className = 'notice error';
      }
    };
  };

  const renderList = (profiles, selectedKey) => {
    if (!profiles.length) {
      list.innerHTML = '<div class="empty">No clients recorded yet. A profile appears here the first time a paired client starts a stream.</div>';
      editor.innerHTML = '';
      return;
    }
    list.innerHTML = `<div class="profile-rows">${profiles.map((profile, index) => {
      const custom = profile.geometry_policy !== 'fit' || profile.fps_policy !== 'auto' || profile.codec_policy !== 'auto' || profile.hdr_policy !== 'auto' || Number(profile.bitrate_ceiling_kbps) > 0;
      const learned = Number(profile.learned_start_kbps) ? ` · learned ${(profile.learned_start_kbps / 1000).toFixed(1)} Mbps` : '';
      return `<button type="button" class="profile-row${keyOf(profile) === selectedKey ? ' selected' : ''}" data-index="${index}">
        <span class="profile-row-main"><strong>${escapeHtml(profile.client_id)}</strong><span class="field-hint">${escapeHtml(profile.network_class)}${escapeHtml(learned)}</span></span>
        <span class="profile-row-tags">${profile.client_id === connectedClientId ? '<span class="badge badge-ok">connected</span>' : ''}<span class="badge">${custom ? 'custom' : 'automatic'}</span></span>
      </button>`;
    }).join('')}</div>`;
    list.querySelectorAll('[data-index]').forEach((button) => button.onclick = () => {
      const profile = profiles[Number(button.dataset.index)];
      if (!profile) return;
      renderList(profiles, keyOf(profile));
      openEditor(profile);
    });
  };

  const loadProfiles = async (selectedKey = '') => {
    try { renderList((await json(await api('/stream/profiles'))).profiles || [], selectedKey); }
    catch (error) { list.innerHTML = `<div class="notice error">${escapeHtml(error.message)}</div>`; }
  };
  await loadProfiles();
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
  shell(`<div class="page-header"><div><h2>Virtual display</h2><p>Choose how SteamShine obtains the display it streams. Saving requires a restart.</p></div><a class="btn-ghost btn-sm" href="/sunshine/config">Sunshine settings</a></div>
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
    </form>`, { authenticated: true, activeId: 'config' });
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
  shell(`<div class="page-header"><div><h2>Terminal</h2></div>
      <div class="btn-row"><button id="term-restart" class="btn-ghost btn-sm">Restart session</button></div></div>
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
