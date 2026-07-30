/**
 * Run upstream Sunshine Web UI setup, login, and PIN-page browser validation.
 */
import { chromium, request } from '@playwright/test';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';

const binary = process.env.STEAMSHINE_BINARY;
const reportDirectory = process.env.STEAMSHINE_BROWSER_REPORT_DIR;
const chromiumPath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE;
const basePort = Number.parseInt(process.env.STEAMSHINE_WEB_SMOKE_BASE_PORT ?? '48989', 10);

if (!binary || !reportDirectory || !chromiumPath) {
  throw new Error('STEAMSHINE_BINARY, STEAMSHINE_BROWSER_REPORT_DIR, and PLAYWRIGHT_CHROMIUM_EXECUTABLE are required.');
}

const webPort = basePort + 1;
const baseUrl = `https://127.0.0.1:${webPort}`;
const workDirectory = await mkdtemp(join(tmpdir(), 'steamshine-web-browser-'));
const homeDirectory = join(workDirectory, 'home');
const logFile = join(reportDirectory, 'steamshine-web-browser.log');
const traceFile = join(reportDirectory, 'upstream-browser-trace.zip');
const screenshotFile = join(reportDirectory, 'upstream-browser-failure.png');
const successScreenshotFile = join(reportDirectory, 'steamshine-monitor.png');
const configFile = join(homeDirectory, 'sunshine.conf');
const consoleErrors = [];
const failedRequests = [];
const securityResults = {};
const responsiveViewports = [];
let server;
let browser;
let browserVersion = '';

/** Wait until the real HTTPS listener accepts a browser navigation. */
async function waitForWelcome(page) {
  let lastError;
  for (let attempt = 0; attempt < 30; ++attempt) {
    try {
      const response = await page.goto(`${baseUrl}/welcome/`, { waitUntil: 'domcontentloaded', timeout: 1000 });
      if (response?.status() === 200) {
        return;
      }
      lastError = new Error(`Unexpected welcome status: ${response?.status()}`);
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw lastError ?? new Error('Timed out waiting for the upstream welcome page.');
}

/** Wait until the SteamShine Monitor has rendered live metric values. */
async function waitForMonitor(page) {
  await page.getByRole('heading', { name: 'Monitor', exact: true }).waitFor({ timeout: 5000 });
  await page.waitForFunction(
    () => {
      const value = document.querySelector('.metric-tile .metric-value');
      return value !== null && value.textContent?.trim() !== '—';
    },
    undefined,
    { timeout: 5000 },
  );
}

/** Stop the spawned server and remove only the temporary browser test HOME. */
async function cleanup() {
  if (browser) {
    await browser.close();
  }
  if (server && !server.killed) {
    server.kill('SIGTERM');
    await new Promise((resolve) => server.once('exit', resolve));
  }
  await rm(workDirectory, { recursive: true, force: true });
}

try {
  await mkdir(join(homeDirectory, 'run'), { recursive: true });
  await mkdir(reportDirectory, { recursive: true });
  await writeFile(configFile, [
    `port = ${basePort}`,
    'address_family = ipv4',
    'bind_address = 127.0.0.1',
    'origin_web_ui_allowed = pc',
    'system_tray = disabled',
    'steamshine_web_ui_default = enabled',
    '',
  ].join('\n'));
  const logHandle = await import('node:fs').then(({ createWriteStream }) => createWriteStream(logFile));
  server = spawn(binary, [configFile], {
    env: { ...process.env, HOME: homeDirectory, XDG_RUNTIME_DIR: join(homeDirectory, 'run') },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  server.stdout.pipe(logHandle);
  server.stderr.pipe(logHandle);

  browser = await chromium.launch({ executablePath: chromiumPath, headless: true, args: ['--no-sandbox'] });
  browserVersion = browser.version();
  const setupContext = await browser.newContext({ ignoreHTTPSErrors: true });
  await setupContext.tracing.start({ screenshots: true, snapshots: true, sources: true });
  const setupPage = await setupContext.newPage();
  await waitForWelcome(setupPage);
  setupPage.on('console', (message) => {
    if (message.type() === 'error' && !/status of 401/.test(message.text())) consoleErrors.push(message.text());
  });
  setupPage.on('requestfailed', (request) => {
    if (request.url().startsWith(baseUrl) && !request.url().includes('/api/steamshine/v1/session')) failedRequests.push(`${request.method()} ${request.url()}`);
  });
  await setupPage.waitForSelector('#usernameInput');
  await setupPage.waitForFunction(() => !document.querySelector('body')?.hasAttribute('v-cloak'));
  if ((await setupPage.locator('body').innerText()).includes('<%-')) {
    throw new Error('The rendered welcome page contains an unresolved EJS template marker.');
  }
  const setupRouteResponse = await setupPage.goto(`${baseUrl}/steamshine/login`, { waitUntil: 'domcontentloaded' });
  if (setupRouteResponse?.status() !== 200) {
    throw new Error(`SteamShine credential setup returned ${setupRouteResponse?.status()}.`);
  }
  await setupPage.getByRole('heading', { name: 'Create shared credentials' }).waitFor({ timeout: 5000 });
  await setupPage.locator('#setup input[name="username"]').fill('web-e2e');
  await setupPage.locator('#setup input[name="password"]').fill('web-e2e-password');
  await setupPage.locator('#setup input[name="confirm_password"]').fill('web-e2e-password');
  await setupPage.locator('#setup button').click();
  await setupPage.getByRole('heading', { name: 'Sign in' }).waitFor({ timeout: 5000 });
  const defaultRouteResponse = await setupPage.goto(`${baseUrl}/`, { waitUntil: 'networkidle' });
  if (defaultRouteResponse?.status() !== 200 || !(await setupPage.getByRole('heading', { name: 'Sign in' }).isVisible())) {
    throw new Error('SteamShine was not served at the configured default Web UI route.');
  }
  await setupContext.tracing.stop({ path: traceFile });
  await setupContext.close();

  const authenticatedContext = await browser.newContext({
    ignoreHTTPSErrors: true,
    httpCredentials: { username: 'web-e2e', password: 'web-e2e-password' },
  });
  await authenticatedContext.route('https://api.github.com/repos/LizardByte/Sunshine/releases/latest', async (route) => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({ tag_name: 'v0.0.0', name: 'SteamShine browser test' }),
  }));
  await authenticatedContext.route('https://api.github.com/repos/LizardByte/Sunshine/releases', async (route) => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify([{ tag_name: 'v0.0.1', name: 'SteamShine browser test pre-release', prerelease: true }]),
  }));
  const authenticatedPage = await authenticatedContext.newPage();
  authenticatedPage.on('console', (message) => {
    if (message.type() === 'error') consoleErrors.push(message.text());
  });
  authenticatedPage.on('requestfailed', (request) => {
    if (request.url().startsWith(baseUrl)) failedRequests.push(`${request.method()} ${request.url()}`);
  });
  const rootResponse = await authenticatedPage.goto(`${baseUrl}/sunshine/`, { waitUntil: 'networkidle' });
  if (rootResponse?.status() !== 200) {
    throw new Error(`Authenticated upstream root returned ${rootResponse?.status()}.`);
  }
  if ((await authenticatedPage.locator('body').innerText()).includes('{{ $t(')) {
    throw new Error('The rendered authenticated page contains an unresolved translation marker.');
  }
  const pinResponse = await authenticatedPage.goto(`${baseUrl}/pin/`, { waitUntil: 'networkidle' });
  if (pinResponse?.status() !== 200) {
    throw new Error(`Authenticated upstream PIN page returned ${pinResponse?.status()}.`);
  }
  await authenticatedPage.waitForFunction(() => !document.querySelector('body')?.hasAttribute('v-cloak'));
  await authenticatedPage.locator('#pin-input').fill('123');
  const invalidPin = await authenticatedPage.locator('#pin-input').evaluate((input) => !input.checkValidity());
  if (!invalidPin) {
    throw new Error('The upstream PIN form accepted a value other than four digits.');
  }

  const steamshineContext = await browser.newContext({ ignoreHTTPSErrors: true });
  const steamshinePage = await steamshineContext.newPage();
  const steamshineResponse = await steamshinePage.goto(`${baseUrl}/`, { waitUntil: 'domcontentloaded' });
  if (steamshineResponse?.status() !== 200) {
    throw new Error(`SteamShine root returned ${steamshineResponse?.status()}.`);
  }
  await steamshinePage.locator('#login input[name="username"]').fill('web-e2e');
  await steamshinePage.locator('#login input[name="password"]').fill('web-e2e-password');
  await steamshinePage.locator('#login button').click();
  await steamshinePage.waitForURL(`${baseUrl}/steamshine/monitor`, { timeout: 5000 });
  await waitForMonitor(steamshinePage);
  for (const viewport of [
    { name: 'desktop', width: 1440, height: 900 },
    { name: 'tablet', width: 768, height: 1024 },
    { name: 'steam_deck', width: 800, height: 1280 },
  ]) {
    await steamshinePage.setViewportSize({ width: viewport.width, height: viewport.height });
    const response = await steamshinePage.goto(`${baseUrl}/steamshine/monitor`, { waitUntil: 'domcontentloaded' });
    await waitForMonitor(steamshinePage);
    const horizontalOverflow = await steamshinePage.evaluate(() => document.documentElement.scrollWidth > window.innerWidth);
    if (response?.status() !== 200 || horizontalOverflow) {
      throw new Error(`SteamShine Monitor is not responsive at ${viewport.name}.`);
    }
    responsiveViewports.push({ ...viewport, status: response.status(), horizontal_overflow: horizontalOverflow });
  }
  await steamshinePage.screenshot({ path: successScreenshotFile, fullPage: true });
  const concurrentUpstreamResponse = await authenticatedPage.reload({ waitUntil: 'networkidle' });
  if (concurrentUpstreamResponse?.status() !== 200) {
    throw new Error(`Upstream session did not remain valid with SteamShine open: ${concurrentUpstreamResponse?.status()}.`);
  }
  const concurrentSteamshineStatus = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (concurrentSteamshineStatus !== 200) throw new Error('SteamShine session did not remain valid with upstream open.');
  steamshinePage.on('console', (message) => {
    if (message.type() === 'error' && !/status of (400|401|429)/.test(message.text())) consoleErrors.push(message.text());
  });
  steamshinePage.on('requestfailed', (request) => {
    if (request.url().startsWith(baseUrl) && !request.url().includes('/api/steamshine/v1/session') && !request.url().includes('/api/steamshine/v1/pairing/pin') && !request.url().includes('/api/steamshine/v1/config/virtual-display') && !request.url().includes('/api/steamshine/v1/stream/profiles')) {
      failedRequests.push(`${request.method()} ${request.url()}`);
    }
  });
  const steamshineCookies = await steamshineContext.cookies(baseUrl);
  const sessionCookie = steamshineCookies.find((cookie) => cookie.name === 'steamshine_session');
  if (!sessionCookie?.secure || !sessionCookie.httpOnly || sessionCookie.sameSite !== 'Strict') {
    throw new Error('SteamShine session cookie is missing Secure, HttpOnly, or SameSite=Strict.');
  }
  const clientRoute = '**/api/steamshine/v1/clients';
  await steamshinePage.route(clientRoute, async (route) => route.fulfill({
    contentType: 'application/json',
    body: JSON.stringify({ named_certs: [{ uuid: 'xss-test', name: '<img src=x onerror="window.steamshineXss=true">' }] }),
  }));
  await steamshinePage.goto(`${baseUrl}/steamshine/clients`, { waitUntil: 'networkidle' });
  // The SteamShine shell legitimately renders a couple of <img> logo marks
  // (sidebar brand + mobile top bar) on every authenticated page, so an
  // absent onerror firing plus every real <img> pointing at our own known
  // asset path is what proves the hostile name was escaped, not merely
  // "there are zero <img> elements" (true before the redesign added a logo).
  securityResults.xss_escaped = await steamshinePage.evaluate(() => !window.steamshineXss
    && [...document.querySelectorAll('img')].every((img) => img.getAttribute('src')?.startsWith('/steamshine/images/')));
  await steamshinePage.unroute(clientRoute);
  if (!securityResults.xss_escaped) {
    throw new Error('SteamShine client rendering did not escape a hostile client name.');
  }
  await steamshinePage.goto(`${baseUrl}/steamshine/pairing`, { waitUntil: 'networkidle' });
  await steamshinePage.locator('#pairing input[name="pin"]').fill('123');
  const steamshineInvalidPin = await steamshinePage.locator('#pairing input[name="pin"]').evaluate((input) => !input.checkValidity());
  if (!steamshineInvalidPin) {
    throw new Error('The SteamShine PIN form accepted a value other than four digits.');
  }
  securityResults.missing_csrf_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/pairing/pin', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ pin: '1234', name: 'test-client' }),
  })).status);
  securityResults.invalid_csrf_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/pairing/pin', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': 'invalid' }, body: JSON.stringify({ pin: '1234', name: 'test-client' }),
  })).status);
  const csrfValue = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).json().then((value) => value.csrf_token));
  const streamResponse = await steamshinePage.goto(`${baseUrl}/steamshine/stream`, { waitUntil: 'domcontentloaded' });
  if (streamResponse?.status() !== 200) throw new Error(`Steam negotiation page returned ${streamResponse?.status()}.`);
  await steamshinePage.getByRole('heading', { name: 'Stream negotiation' }).waitFor({ timeout: 5000 });
  const streamState = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/status')).json().then((value) => value.stream_negotiation));
  if (streamState?.schema_version !== 1 || streamState?.poll_interval_ms !== 2000) {
    throw new Error(`Stream negotiation schema or polling bound changed: ${JSON.stringify(streamState)}`);
  }
  await steamshinePage.getByRole('heading', { name: 'Sender recording' }).waitFor({ timeout: 5000 });
  const initialRecordingState = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/stream/recordings')).json());
  if (initialRecordingState.capacity_mb !== 500 || initialRecordingState.state !== 'idle' || !Array.isArray(initialRecordingState.recordings)) {
    throw new Error(`Unexpected default sender recording state: ${JSON.stringify(initialRecordingState)}`);
  }
  securityResults.recording_capacity_status = await steamshinePage.evaluate(async (csrf) => {
    const response = await fetch('/api/steamshine/v1/stream/recordings/settings', {
      method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: JSON.stringify({ capacity_mb: 501 }),
    });
    await response.text();
    return response.status;
  }, csrfValue);
  const savedRecordingState = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/stream/recordings')).json());
  if (securityResults.recording_capacity_status !== 200 || savedRecordingState.capacity_mb !== 501) {
    throw new Error(`Sender recording capacity did not persist: ${JSON.stringify(savedRecordingState)}`);
  }
  const recordingToggleStatuses = [];
  for (const enabled of [true, false]) {
    recordingToggleStatuses.push(await steamshinePage.evaluate(async ({ csrf, value }) => {
      const response = await fetch('/api/steamshine/v1/stream/recordings/toggle', {
        method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: JSON.stringify({ enabled: value }),
      });
      await response.text();
      return response.status;
    }, { csrf: csrfValue, value: enabled }));
  }
  if (recordingToggleStatuses.some((status) => status !== 200)) throw new Error(`Sender recording toggle failed: ${recordingToggleStatuses.join(',')}`);
  const profilePayload = {
    client_id: 'browser-client', network_class: 'lan', capability_signature: 'v1-c0-d0-x0-h0', active: true,
    geometry_policy: 'fit', fps_policy: 'custom', fps_ceiling: 60, codec_policy: 'h264', hdr_policy: 'off',
    bitrate_ceiling_kbps: 15000, quality_preset: 'balanced', orientation: 'landscape', safe_area_percent: 5,
    learned_start_kbps: 12000,
  };
  securityResults.stream_profile_save_status = await steamshinePage.evaluate(async ({ csrf, profile }) => (await fetch('/api/steamshine/v1/stream/profiles', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: JSON.stringify(profile),
  })).status, { csrf: csrfValue, profile: profilePayload });
  const streamProfiles = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/stream/profiles')).json());
  if (securityResults.stream_profile_save_status !== 200 || streamProfiles.profiles?.length !== 1 || !streamProfiles.profiles[0].active) {
    throw new Error(`Stream profile API did not persist the active profile: ${JSON.stringify(streamProfiles)}`);
  }
  const fallbackHref = await steamshinePage.getByRole('link', { name: 'Sunshine fallback settings' }).getAttribute('href');
  if (fallbackHref !== '/sunshine/config') throw new Error(`Unexpected stream rollback route: ${fallbackHref}`);
  const statusRoute = '**/api/steamshine/v1/status';
  await steamshinePage.route(statusRoute, async (route) => route.fulfill({ status: 200, contentType: 'application/json', body: '{' }));
  await steamshinePage.goto(`${baseUrl}/steamshine/stream`, { waitUntil: 'domcontentloaded' });
  await steamshinePage.getByRole('heading', { name: 'Stream negotiation' }).waitFor({ timeout: 5000 });
  await steamshinePage.unroute(statusRoute);
  const upstreamAfterStreamUiFailure = await authenticatedPage.reload({ waitUntil: 'networkidle' });
  if (upstreamAfterStreamUiFailure?.status() !== 200) throw new Error('Stream UI status failure affected the upstream recovery UI.');
  securityResults.stream_profile_reset_status = await steamshinePage.evaluate(async ({ csrf, clientId, networkClass }) => (await fetch('/api/steamshine/v1/stream/profiles/reset', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf },
    body: JSON.stringify({ client_id: clientId, network_class: networkClass }),
  })).status, { csrf: csrfValue, clientId: profilePayload.client_id, networkClass: profilePayload.network_class });
  if (securityResults.stream_profile_reset_status !== 200) throw new Error('Stream profile reset failed.');
  await steamshinePage.goto(`${baseUrl}/steamshine/config`, { waitUntil: 'networkidle' });
  await steamshinePage.getByRole('heading', { name: 'Virtual Display' }).waitFor({ timeout: 5000 });
  await steamshinePage.locator('#virtual-display-config select[name="mode"]').waitFor({ timeout: 5000 });
  securityResults.virtual_display_config_status = await steamshinePage.evaluate(async (csrf) => (await fetch('/api/steamshine/v1/config/virtual-display', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: JSON.stringify({ enabled: true, mode: 'force' }),
  })).status, csrfValue);
  if (securityResults.virtual_display_config_status !== 200) {
    throw new Error(`SteamShine virtual display configuration returned ${securityResults.virtual_display_config_status}.`);
  }
  const cspResponse = await steamshinePage.request.get(`${baseUrl}/steamshine/monitor`);
  securityResults.csp_header = cspResponse.headers()['content-security-policy'] || '';
  if (!securityResults.csp_header.includes("default-src 'self'")) {
    throw new Error('SteamShine Monitor is missing its restrictive Content-Security-Policy header.');
  }
  if (!securityResults.csp_header.includes("style-src 'self'") || !securityResults.csp_header.includes("style-src-attr 'unsafe-inline'")) {
    throw new Error('SteamShine Monitor CSP does not narrowly permit required runtime style attributes.');
  }
  if (!securityResults.csp_header.includes("connect-src 'self' wss://127.0.0.1:48991")) {
    throw new Error('SteamShine Monitor CSP does not permit its same-host terminal WebSocket.');
  }
  const appAssetResponse = await steamshinePage.request.get(`${baseUrl}/steamshine/app.js`);
  securityResults.app_asset_cache_control = appAssetResponse.headers()['cache-control'] || '';
  if (securityResults.app_asset_cache_control !== 'no-store') {
    throw new Error('SteamShine application assets may retain stale login code in the browser cache.');
  }
  const appAsset = await appAssetResponse.text();
  securityResults.terminal_explanation_removed = !appAsset.includes('A real shell on the SteamShine host') && !appAsset.includes('The terminal connects over a separate port');
  if (!securityResults.terminal_explanation_removed) throw new Error('The Terminal page still contains the removed subtitle or framed explanation.');
  const appCssResponse = await steamshinePage.request.get(`${baseUrl}/steamshine/app.css`);
  const appCss = await appCssResponse.text();
  securityResults.monitor_fan_icon_sized = /\.metric-sub \.fan svg\s*\{[^}]*width:\s*0\.85rem;[^}]*height:\s*0\.85rem;[^}]*flex:\s*0 0 0\.85rem;/s.test(appCss);
  if (!securityResults.monitor_fan_icon_sized) throw new Error('The Monitor fan icon can obscure or clip the GPU RPM readout.');
  securityResults.malformed_json_status = await steamshinePage.evaluate(async (csrf) => (await fetch('/api/steamshine/v1/pairing/pin', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: '{',
  })).status, csrfValue);
  securityResults.oversized_body_status = await steamshinePage.evaluate(async (csrf) => (await fetch('/api/steamshine/v1/pairing/pin', {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': csrf }, body: 'x'.repeat(65537),
  })).status, csrfValue);
  const apiContext = await request.newContext({
    ignoreHTTPSErrors: true,
    extraHTTPHeaders: { Cookie: `steamshine_session=${sessionCookie.value}`, Origin: 'https://invalid.example' },
  });
  securityResults.origin_mismatch_status = (await apiContext.post(`${baseUrl}/api/steamshine/v1/pairing/pin`, {
    data: { pin: '1234', name: 'test-client' }, headers: { 'X-SteamShine-CSRF-Token': csrfValue },
  })).status();
  await apiContext.dispose();
  const hostMismatchContext = await request.newContext({
    ignoreHTTPSErrors: true,
    extraHTTPHeaders: { Cookie: `steamshine_session=${sessionCookie.value}`, Host: 'invalid.example', Origin: baseUrl },
  });
  securityResults.host_mismatch_status = (await hostMismatchContext.post(`${baseUrl}/api/steamshine/v1/pairing/pin`, {
    data: { pin: '1234', name: 'test-client' }, headers: { 'X-SteamShine-CSRF-Token': csrfValue },
  })).status();
  await hostMismatchContext.dispose();
  const unauthorizedRevokeContext = await request.newContext({
    ignoreHTTPSErrors: true,
    extraHTTPHeaders: { Origin: baseUrl, 'X-SteamShine-CSRF-Token': csrfValue },
  });
  securityResults.unauthorized_revoke_status = (await unauthorizedRevokeContext.delete(`${baseUrl}/api/steamshine/v1/clients/not-a-client`)).status();
  securityResults.static_path_traversal_status = (await unauthorizedRevokeContext.get(`${baseUrl}/steamshine/%2e%2e%2findex.html`)).status();
  await unauthorizedRevokeContext.dispose();
  const pinRateContext = await request.newContext({
    ignoreHTTPSErrors: true,
    extraHTTPHeaders: { Cookie: `steamshine_session=${sessionCookie.value}`, Origin: baseUrl, 'X-SteamShine-CSRF-Token': csrfValue },
  });
  const pinRateStatuses = [];
  for (let attempt = 0; attempt < 6; ++attempt) {
    pinRateStatuses.push((await pinRateContext.post(`${baseUrl}/api/steamshine/v1/pairing/pin`, { data: { pin: 'bad', name: 'test-client' } })).status());
  }
  await pinRateContext.dispose();
  securityResults.pin_rate_limit_status = pinRateStatuses.find((status) => status === 429) ?? 0;
  if (securityResults.pin_rate_limit_status !== 429 || pinRateStatuses.slice(pinRateStatuses.indexOf(429)).some((status) => status !== 429)) {
    throw new Error(`SteamShine PIN rate limit failed: ${pinRateStatuses.join(',')}`);
  }
  const csrfSecurityStatuses = [
    securityResults.missing_csrf_status,
    securityResults.invalid_csrf_status,
    securityResults.malformed_json_status,
    securityResults.oversized_body_status,
    securityResults.origin_mismatch_status,
    securityResults.host_mismatch_status,
  ];
  if (csrfSecurityStatuses.some((status) => status !== 400)) {
    throw new Error(`SteamShine security request was not rejected: ${JSON.stringify(securityResults)}`);
  }
  if (securityResults.unauthorized_revoke_status !== 401 || securityResults.static_path_traversal_status !== 404) {
    throw new Error(`SteamShine authorization or static traversal protection failed: ${JSON.stringify(securityResults)}`);
  }

  const upstreamCsrf = await authenticatedPage.evaluate(async () => (await fetch('/api/csrf-token')).json().then((value) => value.csrf_token));
  const credentialChange = await authenticatedPage.evaluate(async (csrf) => (await fetch('/api/password', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf },
    body: JSON.stringify({ currentUsername: 'web-e2e', currentPassword: 'web-e2e-password', newUsername: 'web-e2e', newPassword: 'web-e2e-password-2', confirmNewPassword: 'web-e2e-password-2' }),
  })).status, upstreamCsrf);
  if (credentialChange !== 200) throw new Error(`Credential change returned ${credentialChange}.`);
  securityResults.credential_change_session_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (securityResults.credential_change_session_status !== 401) throw new Error('Credential change did not invalidate the SteamShine session.');
  await steamshinePage.goto(`${baseUrl}/steamshine/login`, { waitUntil: 'networkidle' });
  await steamshinePage.locator('#login input[name="username"]').fill('web-e2e');
  await steamshinePage.locator('#login input[name="password"]').fill('web-e2e-password-2');
  await steamshinePage.locator('#login button').click();
  await waitForMonitor(steamshinePage);
  await steamshinePage.locator('#mobile-logout').click();
  await steamshinePage.getByRole('heading', { name: 'Sign in' }).waitFor({ timeout: 5000 });
  securityResults.logout_session_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (securityResults.logout_session_status !== 401) throw new Error('Logout did not invalidate the SteamShine session.');
  const loginRateContext = await request.newContext({ ignoreHTTPSErrors: true, extraHTTPHeaders: { Origin: baseUrl } });
  const successfulLoginStatuses = [];
  for (let attempt = 0; attempt < 6; ++attempt) {
    successfulLoginStatuses.push((await loginRateContext.post(`${baseUrl}/api/steamshine/v1/auth/login`, {
      data: { username: 'web-e2e', password: 'web-e2e-password-2' },
    })).status());
  }
  securityResults.successful_logins_not_rate_limited = successfulLoginStatuses.every((status) => status === 200);
  if (!securityResults.successful_logins_not_rate_limited) {
    throw new Error(`Successful SteamShine logins consumed the failure limit: ${successfulLoginStatuses.join(',')}`);
  }
  const loginRateStatuses = [];
  for (let attempt = 0; attempt < 6; ++attempt) {
    loginRateStatuses.push((await loginRateContext.post(`${baseUrl}/api/steamshine/v1/auth/login`, {
      data: { username: 'web-e2e', password: 'wrong-password' },
    })).status());
  }
  await loginRateContext.dispose();
  securityResults.login_rate_limit_status = loginRateStatuses.at(-1);
  if (!loginRateStatuses.slice(0, -1).every((status) => status === 401) || securityResults.login_rate_limit_status !== 429) {
    throw new Error(`SteamShine login rate limit failed: ${loginRateStatuses.join(',')}`);
  }
  await new Promise((resolve) => setTimeout(resolve, 100));
  const serviceLog = await readFile(logFile, 'utf8').catch(() => '');
  securityResults.secrets_absent_from_service_log = !['web-e2e-password', 'web-e2e-password-2', '1234', 'bad'].some((secret) => serviceLog.includes(secret));
  if (!securityResults.secrets_absent_from_service_log) {
    throw new Error('SteamShine service log exposed a browser credential or pairing PIN.');
  }
  await steamshineContext.close();
  if (consoleErrors.length || failedRequests.length) {
    throw new Error(`Browser errors: ${consoleErrors.join('; ')}; failed requests: ${failedRequests.join('; ')}`);
  }
  await authenticatedContext.close();
  await writeFile(join(reportDirectory, 'web-browser-e2e-report.json'), JSON.stringify({
    browser: 'chromium',
    browser_version: browserVersion,
    commit_sha: process.env.STEAMSHINE_COMMIT_SHA || process.env.GITHUB_SHA || 'local',
    artifact_sha256: process.env.STEAMSHINE_ARTIFACT_SHA256 || null,
    tested_url: baseUrl,
    welcome_status: 200,
    root_status: 200,
    steamshine_default_route_status: 200,
    upstream_compatibility_route_status: 200,
    pin_status: 200,
    setup: 'passed',
    login: 'passed',
    invalid_pin_rejected: true,
    steamshine_monitor_status: 200,
    steamshine_login: 'passed',
    steamshine_secure_session_cookie: true,
    steamshine_invalid_pin_rejected: true,
    steamshine_stream_negotiation_status: 200,
    stream_profile_save_reset: 'passed',
    stream_ui_failure_isolated: true,
    stream_poll_interval_ms: 2000,
    concurrent_upstream_and_steamshine_sessions: true,
    responsive_viewports: responsiveViewports,
    console_errors: consoleErrors,
    failed_requests: failedRequests,
    trace: traceFile,
    screenshots: [successScreenshotFile],
    service_logs: [logFile],
  }, null, 2) + '\n');
  await writeFile(join(reportDirectory, 'web-security-report.json'), JSON.stringify({
    browser: 'chromium', browser_version: browserVersion,
    commit_sha: process.env.STEAMSHINE_COMMIT_SHA || process.env.GITHUB_SHA || 'local',
    artifact_sha256: process.env.STEAMSHINE_ARTIFACT_SHA256 || null,
    tested_url: baseUrl, cookie_attributes: 'Secure; HttpOnly; SameSite=Strict', ...securityResults,
  }, null, 2) + '\n');
  await writeFile(join(reportDirectory, 'web-coexistence-report.json'), JSON.stringify({
    commit_sha: process.env.STEAMSHINE_COMMIT_SHA || process.env.GITHUB_SHA || 'local',
    artifact_sha256: process.env.STEAMSHINE_ARTIFACT_SHA256 || null,
    upstream_url: `${baseUrl}/`, steamshine_url: `${baseUrl}/steamshine/monitor`, shared_credential_login: true,
    simultaneous_routes: true,
    client_sync: 'not_exercised_without_a_mock_pairing_backend',
    service_mock_client_sync: 'covered_by_WebServicesTest.SharesPairingAndClientState',
    service_logs: [logFile],
  }, null, 2) + '\n');
} catch (error) {
  if (browser) {
    const pages = browser.contexts().flatMap((context) => context.pages());
    if (pages[0]) await pages[0].screenshot({ path: screenshotFile, fullPage: true }).catch(() => {});
  }
  await writeFile(join(reportDirectory, 'web-browser-e2e-report.json'), JSON.stringify({
    browser: 'chromium',
    tested_url: baseUrl,
    error: String(error),
    console_errors: consoleErrors,
    failed_requests: failedRequests,
    screenshot: screenshotFile,
  }, null, 2) + '\n');
  throw error;
} finally {
  await cleanup();
}
