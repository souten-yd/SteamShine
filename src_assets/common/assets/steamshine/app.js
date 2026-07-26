/**
 * @file app.js
 * @brief Minimal SteamShine browser client for the shared Sunshine Web facade.
 */

const app = document.querySelector('#app');
let csrfToken = '';

/**
 * @brief Escape arbitrary strings before putting them into a rendered template.
 * @param {unknown} value Value to escape.
 * @returns {string} Safe HTML text.
 */
function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[character]);
}

/**
 * @brief Send one request to the SteamShine facade.
 * @param {string} path API path below the SteamShine version root.
 * @param {RequestInit} options Request options.
 * @returns {Promise<Response>} HTTP response.
 */
function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (options.body) headers.set('Content-Type', 'application/json');
  if (csrfToken) headers.set('X-SteamShine-CSRF-Token', csrfToken);
  return fetch(`/api/steamshine/v1${path}`, { credentials: 'same-origin', ...options, headers });
}

/**
 * @brief Parse a JSON response and throw a safe message on failure.
 * @param {Response} response HTTP response.
 * @returns {Promise<object>} Parsed response object.
 */
async function json(response) {
  const result = await response.json().catch(() => ({}));
  if (!response.ok || result.status === false) throw new Error(result.message || result.error || `Request failed (${response.status})`);
  return result;
}

/**
 * @brief Render the shared SteamShine shell.
 * @param {string} content Page content.
 * @param {boolean} authenticated Whether navigation should be shown.
 */
function shell(content, authenticated = false) {
  const navigation = authenticated ? `<nav>
    <a class="link secondary" href="/steamshine/dashboard">Dashboard</a><a class="link secondary" href="/steamshine/config">Display</a><a class="link secondary" href="/steamshine/pairing">Pairing</a><a class="link secondary" href="/steamshine/clients">Clients</a><a class="link secondary" href="/steamshine/logs">Logs</a><button id="logout" class="secondary">Log out</button>
  </nav>` : '';
  app.innerHTML = `<div class="shell"><header><div class="mark" aria-hidden="true">S</div><div><h1>SteamShine</h1><p>Sunshine control for SteamOS</p></div></header>${navigation}${content}<footer><a class="link secondary" href="/sunshine/">Open the Sunshine Web UI</a></footer></div>`;
  document.querySelector('#logout')?.addEventListener('click', logout);
}

/** @brief Redirect to a SteamShine route. @param {string} path Destination path. */
function navigate(path) { history.pushState({}, '', path); render().catch(showError); }

/** @brief Render a safe error state. @param {unknown} error Error to display. */
function showError(error) { shell(`<section><h2>SteamShine is unavailable</h2><p class="error">${escapeHtml(error.message || error)}</p><button id="retry">Try again</button></section>`); document.querySelector('#retry').onclick = () => render().catch(showError); }

/** @brief Render the initial credential setup view. */
function renderSetup() {
  shell(`<section><h2>Create shared credentials</h2><p>These credentials also work in the Sunshine Web UI.</p><form id="setup"><label>Username<input name="username" maxlength="64" required autocomplete="username"></label><label>Password<input name="password" type="password" required autocomplete="new-password"></label><label>Confirm password<input name="confirm_password" type="password" required autocomplete="new-password"></label><button>Create credentials</button><div class="notice"></div></form></section>`);
  document.querySelector('#setup').onsubmit = async (event) => { event.preventDefault(); const form = new FormData(event.currentTarget); try { await json(await api('/setup/credentials', { method: 'POST', body: JSON.stringify(Object.fromEntries(form)) })); navigate('/steamshine/login'); } catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; } };
}

/** @brief Render the login view. */
function renderLogin() {
  shell(`<section><h2>Sign in</h2><form id="login"><label>Username<input name="username" required autocomplete="username"></label><label>Password<input name="password" type="password" required autocomplete="current-password"></label><button>Sign in</button><div class="notice"></div></form></section>`);
  document.querySelector('#login').onsubmit = async (event) => { event.preventDefault(); try { const data = await json(await api('/auth/login', { method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(event.currentTarget))) })); csrfToken = data.csrf_token; navigate('/steamshine/dashboard'); } catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; } };
}

/** @brief Render an authenticated page for the current path. @param {object} session Current session. */
async function renderAuthenticated(session) {
  csrfToken = session.csrf_token;
  const page = location.pathname.split('/').filter(Boolean).pop() || 'dashboard';
  if (page === 'pairing') return renderPairing();
  if (page === 'config') return renderVirtualDisplayConfig();
  if (page === 'clients') return renderClients();
  if (page === 'logs') return renderLogs();
  const status = await json(await api('/status'));
  shell(`<section><h2>Welcome, ${escapeHtml(session.username)}</h2><div class="grid"><div class="card"><div>Active streams</div><div class="metric">${escapeHtml(status.active_streams)}</div></div><div class="card"><div>Application</div><div class="metric">${status.application_running ? 'Running' : 'Idle'}</div></div><div class="card"><div>Gamescope</div><div class="metric">${status.gamescope_active ? 'Active' : 'Idle'}</div></div><div class="card"><div>Virtual display mode</div><div class="metric">${status.virtual_display_enabled ? escapeHtml(status.virtual_display_mode) : 'Disabled'}</div></div><div class="card"><div>Session origin</div><div class="metric">${escapeHtml(status.virtual_display_origin || 'none')}</div></div><div class="card"><div>Steam location</div><div class="metric">${escapeHtml(status.steam_location || 'unknown')}</div></div><div class="card"><div>Virtual display state</div><div class="metric">${escapeHtml(status.virtual_display_state || 'Disabled')}</div></div><div class="card"><div>Private socket</div><div class="metric">${escapeHtml(status.virtual_display_socket || 'Unavailable')}</div></div><div class="card"><div>Gamescope PID</div><div class="metric">${escapeHtml(status.gamescope_pid > 0 ? status.gamescope_pid : 'Unavailable')}</div></div><div class="card"><div>Render node</div><div class="metric">${escapeHtml(status.render_node || status.game_gpu || 'Auto')}</div></div><div class="card"><div>Capture frames</div><div class="metric">${escapeHtml(status.captured_frames || 0)}</div></div><div class="card"><div>Encoded packets</div><div class="metric">${escapeHtml(status.encoded_packets || 0)}</div></div><div class="card"><div>Encoder</div><div class="metric">${escapeHtml(status.encoder || 'Auto')}</div></div></div></section>`, true);
}

/** @brief Render the restart-required SteamOS virtual-display policy form. */
async function renderVirtualDisplayConfig() {
  const config = await json(await api('/config/virtual-display'));
  const enabled = config.steamos_virtual_display_enabled === 'enabled';
  shell(`<section><h2>Virtual Display</h2><p>Choose how SteamShine obtains the streamed display.</p><form id="virtual-display-config"><label><input name="enabled" type="checkbox" ${enabled ? 'checked' : ''}> Enable SteamOS virtual display</label><label>Virtual Display Mode<select name="mode"><option value="off" ${config.steamos_virtual_display_mode === 'off' ? 'selected' : ''}>Off</option><option value="auto" ${config.steamos_virtual_display_mode === 'auto' ? 'selected' : ''}>Auto</option><option value="force" ${config.steamos_virtual_display_mode === 'force' ? 'selected' : ''}>Force</option></select></label><label>Gamescope Session Source<select name="session_source"><option value="auto" ${config.steamos_session_source === 'auto' ? 'selected' : ''}>Auto: verified Game Mode first, then private</option><option value="existing_gamescope" ${config.steamos_session_source === 'existing_gamescope' ? 'selected' : ''}>Existing Game Mode only</option><option value="owned_private" ${config.steamos_session_source === 'owned_private' ? 'selected' : ''}>SteamShine private session only</option></select></label><label>Existing Gamescope PID (0 for automatic)<input name="existing_gamescope_pid" type="number" min="0" step="1" value="${escapeHtml(config.steamos_existing_gamescope_pid || '0')}"></label><label><input name="keep_session_alive" type="checkbox" ${config.steamos_keep_session_alive !== 'disabled' ? 'checked' : ''}> Keep a SteamShine-owned session after disconnect</label><p>Existing Game Mode is selected only after process, PipeWire node, and GPU verification. Explicit stop never terminates an attached Game Mode session.</p><button>Save policy</button><div class="notice"></div></form></section>`, true);
  document.querySelector('#virtual-display-config').onsubmit = async (event) => { event.preventDefault(); const form = event.currentTarget; try { const result = await json(await api('/config/virtual-display', { method: 'POST', body: JSON.stringify({ enabled: form.elements.enabled.checked, mode: form.elements.mode.value, session_source: form.elements.session_source.value, keep_session_alive: form.elements.keep_session_alive.checked, existing_gamescope_pid: Number(form.elements.existing_gamescope_pid.value) }) })); form.querySelector('.notice').textContent = result.message; form.querySelector('.notice').className = 'notice ok'; } catch (error) { form.querySelector('.notice').textContent = error.message; } };
}

/** @brief Render the Moonlight pairing page. */
function renderPairing() { shell(`<section><h2>Moonlight pairing</h2><form id="pairing"><label>Client name<input name="name" maxlength="128" required></label><label>Four digit PIN<input name="pin" inputmode="numeric" pattern="[0-9]{4}" maxlength="4" minlength="4" required></label><button>Submit PIN</button><div class="notice"></div></form></section>`, true); document.querySelector('#pairing').onsubmit = async (event) => { event.preventDefault(); try { const data = await json(await api('/pairing/pin', { method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(event.currentTarget))) })); event.currentTarget.querySelector('.notice').textContent = data.message; event.currentTarget.querySelector('.notice').className = 'notice ok'; } catch (error) { event.currentTarget.querySelector('.notice').textContent = error.message; } }; }

/** @brief Render paired clients and their revoke actions. */
async function renderClients() { const data = await json(await api('/clients')); const clients = data.named_certs || []; shell(`<section><h2>Paired clients</h2><table><thead><tr><th>Name</th><th>Action</th></tr></thead><tbody>${clients.map((client) => `<tr><td>${escapeHtml(client.name || client.uuid || 'Unknown client')}</td><td><button class="danger" data-client="${escapeHtml(client.uuid || '')}">Revoke</button></td></tr>`).join('') || '<tr><td colspan="2">No paired clients</td></tr>'}</tbody></table></section>`, true); document.querySelectorAll('[data-client]').forEach((button) => button.addEventListener('click', async () => { try { await json(await api(`/clients/${encodeURIComponent(button.dataset.client)}`, { method: 'DELETE' })); renderClients(); } catch (error) { showError(error); } })); }

/** @brief Render the bounded diagnostic log view. */
async function renderLogs() { const data = await json(await api('/logs/recent')); shell(`<section><h2>Recent diagnostics</h2><pre>${escapeHtml(data.content || '')}</pre></section>`, true); }

/** @brief Invalidate the current session and return to login. */
async function logout() { try { await json(await api('/auth/logout', { method: 'POST', body: '{}' })); } finally { csrfToken = ''; navigate('/steamshine/login'); } }

/** @brief Choose a setup, login, or authenticated view from the shared facade state. */
async function render() { const setup = await json(await api('/setup/status')); if (!setup.configured) return renderSetup(); const sessionResponse = await api('/session'); if (sessionResponse.status === 401) return renderLogin(); await renderAuthenticated(await json(sessionResponse)); }

window.addEventListener('popstate', () => render().catch(showError));
render().catch(showError);
