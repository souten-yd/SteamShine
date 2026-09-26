/**
 * Run upstream Sunshine Web UI setup, login, and PIN-page browser validation.
 */
import { chromium, request } from '@playwright/test';
import { execFile as execFileCallback, spawn } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';

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
const websocketErrors = [];
const securityResults = {};
const responsiveViewports = [];
const execFile = promisify(execFileCallback);
let server;
let browser;
let browserVersion = '';

/** Environment isolated from any tmux server hosting the developer's Web Terminal. */
const isolatedServerEnvironment = { ...process.env };
delete isolatedServerEnvironment.TMUX;
delete isolatedServerEnvironment.TMUX_TMPDIR;

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
  if (server && server.exitCode === null && !server.killed) {
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
    `steamshine_gpu_profiles = ${JSON.stringify([{ name: 'Alternative', power_cap_watts: 260, cpu_governor: 'powersave', cpu_max_freq_mhz: 3600 }])}`,
    '',
  ].join('\n'));
  const logHandle = await import('node:fs').then(({ createWriteStream }) => createWriteStream(logFile));
  server = spawn(binary, [configFile], {
    env: {
      ...isolatedServerEnvironment,
      HOME: homeDirectory,
      TMUX_TMPDIR: join(homeDirectory, 'run'),
      XDG_RUNTIME_DIR: join(homeDirectory, 'run'),
    },
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
  steamshinePage.on('console', (message) => {
    if (message.type() === 'error' && !/status of (400|401|429)/.test(message.text()) && !(/status of 403/.test(message.text()) && message.location().url.includes('/gpu/profiles/Alternative/activate'))) consoleErrors.push(message.text());
  });
  steamshinePage.on('requestfailed', (request) => {
    if (request.url().startsWith(baseUrl) && !request.url().includes('/api/steamshine/v1/session') && !request.url().includes('/api/steamshine/v1/pairing/pin') && !request.url().includes('/api/steamshine/v1/config/virtual-display') && !request.url().includes('/api/steamshine/v1/stream/profiles')) {
      failedRequests.push(`${request.method()} ${request.url()}`);
    }
  });
  steamshinePage.on('websocket', (socket) => {
    socket.on('socketerror', (error) => websocketErrors.push(`${socket.url()}: ${error}`));
  });
  const steamshineResponse = await steamshinePage.goto(`${baseUrl}/`, { waitUntil: 'domcontentloaded' });
  if (steamshineResponse?.status() !== 200) {
    throw new Error(`SteamShine root returned ${steamshineResponse?.status()}.`);
  }
  await steamshinePage.locator('#login input[name="username"]').fill('web-e2e');
  await steamshinePage.locator('#login input[name="password"]').fill('web-e2e-password');
  await steamshinePage.locator('#login button').click();
  await steamshinePage.waitForURL(`${baseUrl}/steamshine/monitor`, { timeout: 5000 });
  await waitForMonitor(steamshinePage);

  // Existing custom profiles must survive a process start and appear on the GPU page.
  await steamshinePage.goto(`${baseUrl}/steamshine/gpu`, { waitUntil: 'domcontentloaded' });
  const customProfile = steamshinePage.locator('[data-activate="Alternative"]');
  await customProfile.waitFor({ state: 'visible', timeout: 5000 });
  if (!(await customProfile.innerText()).includes('260W')) {
    throw new Error('The saved Alternative GPU profile lost its power limit.');
  }
  const savedGpuProfiles = await steamshinePage.evaluate(async () => {
    const response = await fetch('/api/steamshine/v1/gpu/profiles');
    return response.json();
  });
  const savedAlternative = savedGpuProfiles.profiles.find((profile) => profile.name === 'Alternative');
  if (savedAlternative?.power_cap_watts !== 260 || savedAlternative?.cpu_max_freq_mhz !== 3600 || savedAlternative?.cpu_governor !== 'powersave') {
    throw new Error('The GPU API did not preserve the existing custom profile.');
  }
  // Administrator provisioning requires CSRF and throttles rejected credentials before sudo.
  const adminSession = await steamshinePage.evaluate(async () => (await (await fetch('/api/steamshine/v1/session')).json()));
  const adminUrl = `${baseUrl}/api/steamshine/v1/system/authorize`;
  const adminNoCsrf = await steamshineContext.request.post(adminUrl, { headers: { Origin: baseUrl }, data: { password: '' } });
  if (adminNoCsrf.status() !== 400) throw new Error('Administrator authorization accepted a request without CSRF.');
  for (let attempt = 0; attempt < 6; attempt++) {
    const denied = await steamshineContext.request.post(adminUrl, { headers: { Origin: baseUrl, 'X-SteamShine-CSRF-Token': adminSession.csrf_token }, data: { password: '' } });
    if (denied.status() !== (attempt < 5 ? 403 : 429)) throw new Error('Administrator authentication did not reject or throttle invalid input.');
  }
  // Runtime-suspended GPUs must not erase disabled profile fields or claim a failed apply.
  const gpuCapsRoute = '**/api/steamshine/v1/gpu/capabilities';
  const unavailableCaps = { gpu_present: true, gpu_name: 'Test AMD GPU', runtime_write_authorized: false,
    power_cap_supported: false, power_cap_min_watts: 0, power_cap_max_watts: 0, power_cap_default_watts: 0,
    cpu_freq_supported: false, cpu_governors: ['powersave'], od_clk_voltage_supported: false };
  await steamshinePage.route(gpuCapsRoute, (route) => route.fulfill({ json: unavailableCaps }));
  await steamshinePage.reload({ waitUntil: 'domcontentloaded' });
  await steamshinePage.locator('[data-edit="Alternative"]').click();
  await steamshinePage.locator('#profile-form input[name="description"]').fill('Preserved while GPU sleeps');
  const disabledSave = steamshinePage.waitForResponse((response) => response.url().endsWith('/gpu/profiles') && response.request().method() === 'POST');
  await steamshinePage.locator('#profile-form button.btn-primary').click();
  await (await disabledSave).finished();
  await steamshinePage.locator('#profile-form').waitFor({ state: 'detached' });
  const preserved = await steamshinePage.evaluate(async () => (await (await fetch('/api/steamshine/v1/gpu/profiles')).json()).profiles.find((p) => p.name === 'Alternative'));
  if (preserved.power_cap_watts !== 260 || preserved.cpu_max_freq_mhz !== 3600) throw new Error('Disabled GPU controls erased saved limits.');
  await steamshinePage.locator('#add-profile').click();
  if (await steamshinePage.locator('#profile-form').count()) throw new Error('An unavailable power range allowed a zero-limit profile.');
  await steamshinePage.locator('#authorize-gpu').click();
  await steamshinePage.locator('#administrator-form').waitFor();
  await steamshinePage.locator('[data-admin-cancel]').click();
  await steamshinePage.route('**/api/steamshine/v1/gpu/profiles/Alternative/activate', (route) => route.fulfill({ status: 403, json: { code: 'admin_authorization_required' } }));
  await steamshinePage.locator('[data-activate="Alternative"] h4').click();
  await steamshinePage.locator('#administrator-form').waitFor();
  await steamshinePage.locator('[data-admin-cancel]').click();
  await steamshinePage.unroute('**/api/steamshine/v1/gpu/profiles/Alternative/activate');
  // Successful authorization clears the password field and refreshes detected bounds.
  await steamshinePage.route('**/api/steamshine/v1/system/authorize', (route) => route.fulfill({ json: { success: true } }));
  await steamshinePage.locator('#authorize-gpu').click();
  await steamshinePage.locator('[name="administrator_password"]').fill('test-only-password');
  await steamshinePage.locator('#administrator-form [type="submit"]').click();
  await steamshinePage.locator('#administrator-form').waitFor({ state: 'detached' });
  await steamshinePage.unroute('**/api/steamshine/v1/system/authorize');
  await steamshinePage.unroute(gpuCapsRoute);
  await steamshinePage.reload({ waitUntil: 'domcontentloaded' });
  await steamshinePage.locator('[data-edit="Alternative"]').waitFor();
  const availableCaps = { ...unavailableCaps, runtime_write_authorized: true, power_cap_supported: true,
    power_cap_min_watts: 231, power_cap_max_watts: 340, power_cap_default_watts: 330,
    cpu_freq_supported: true, cpu_min_freq_mhz: 400, cpu_max_freq_mhz: 3600 };
  await steamshinePage.route(gpuCapsRoute, (route) => route.fulfill({ json: availableCaps }));
  await steamshinePage.reload({ waitUntil: 'domcontentloaded' });
  await steamshinePage.locator('#add-profile').click();
  await steamshinePage.locator('#profile-form input[name="name"]').fill('Browser-created profile');
  if (await steamshinePage.locator('#profile-form [name="power_cap_watts"]').inputValue() !== '330') throw new Error('New profile does not use the detected default power limit.');
  const newSave = steamshinePage.waitForResponse((response) => response.url().endsWith('/gpu/profiles') && response.request().method() === 'POST');
  await steamshinePage.locator('#profile-form button.btn-primary').click();
  await (await newSave).finished();
  await steamshinePage.locator('#profile-form').waitFor({ state: 'detached' });
  await steamshinePage.reload({ waitUntil: 'domcontentloaded' });
  const createdCard = steamshinePage.locator('[data-activate="Browser-created profile"]');
  await createdCard.waitFor();
  if (!(await createdCard.innerText()).includes('330W')) throw new Error('New profile power limit changed after reopening the page.');
  await steamshinePage.unroute(gpuCapsRoute);
  // Saving upstream settings from an older tab must retain profiles added later.
  const profilePersistence = await steamshinePage.evaluate(async () => {
    const session = await (await fetch('/api/steamshine/v1/session')).json();
    const headers = { 'Content-Type': 'application/json', 'X-SteamShine-CSRF-Token': session.csrf_token };
    const created = await fetch('/api/steamshine/v1/gpu/profiles', {
      method: 'POST', headers, body: JSON.stringify({ name: 'Second profile', power_cap_watts: 250, cpu_governor: 'powersave', cpu_max_freq_mhz: 2040 }),
    });
    await created.json();
    const upstreamHeaders = { 'Content-Type': 'application/json', Authorization: `Basic ${btoa('web-e2e:web-e2e-password')}` };
    const config = await (await fetch('/api/config', { headers: upstreamHeaders })).json();
    delete config.status;
    config.steamshine_gpu_profiles = '[]';
    const saved = await fetch('/api/config', { method: 'POST', headers: upstreamHeaders, body: JSON.stringify(config) });
    await saved.json();
    return { created: created.status, saved: saved.status };
  });
  if (profilePersistence.created !== 200 || profilePersistence.saved !== 200) {
    throw new Error(`GPU profile persistence requests failed: ${JSON.stringify(profilePersistence)}`);
  }
  const persistedGpuConfig = await readFile(configFile, 'utf8');
  const persistedGpuLine = persistedGpuConfig.split('\n').find((line) => line.startsWith('steamshine_gpu_profiles = '));
  const persistedGpuNames = JSON.parse(persistedGpuLine.slice(persistedGpuLine.indexOf('=') + 1)).map((profile) => profile.name);
  if (!persistedGpuNames.includes('Alternative') || !persistedGpuNames.includes('Second profile')) {
    throw new Error('Saving upstream settings discarded custom GPU profiles on disk.');
  }
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

  const addonsResponse = await steamshinePage.goto(`${baseUrl}/steamshine/addons`, { waitUntil: 'networkidle' });
  await steamshinePage.getByRole('heading', { name: 'Addons', exact: true }).waitFor({ timeout: 5000 });
  const deckyStatus = await steamshinePage.evaluate(async () => {
    const response = await fetch('/api/steamshine/v1/addons/decky');
    return { status: response.status, body: await response.json() };
  });
  if (addonsResponse?.status() !== 200
    || deckyStatus.status !== 200
    || typeof deckyStatus.body.installed !== 'boolean'
    || typeof deckyStatus.body.service_active !== 'boolean'
    || typeof deckyStatus.body.management_available !== 'boolean') {
    throw new Error(`SteamShine Addon status is invalid: ${JSON.stringify(deckyStatus)}`);
  }
  securityResults.addons_status = deckyStatus.status;

  /** Keep destructive and recovery controls discoverable at phone width. */
  await steamshinePage.setViewportSize({ width: 320, height: 700 });
  const diagnosticsMobileResponse = await steamshinePage.goto(`${baseUrl}/steamshine/diagnostics`, { waitUntil: 'domcontentloaded' });
  const diagnosticsReset = steamshinePage.getByRole('button', { name: 'Reset history' });
  const mobileRestart = steamshinePage.getByRole('button', { name: 'Restart SteamShine' });
  await diagnosticsReset.waitFor({ state: 'visible', timeout: 5000 });
  await mobileRestart.waitFor({ state: 'visible', timeout: 5000 });
  const mobileControls = await steamshinePage.evaluate(() => {
    const reset = document.querySelector('#reset-diagnostics')?.getBoundingClientRect();
    const restart = document.querySelector('#mobile-restart')?.getBoundingClientRect();
    return {
      reset: reset ? { width: reset.width, height: reset.height } : null,
      restart: restart ? { width: restart.width, height: restart.height, label: document.querySelector('#mobile-restart')?.textContent?.trim() } : null,
      horizontalOverflow: document.documentElement.scrollWidth > window.innerWidth,
    };
  });
  if (diagnosticsMobileResponse?.status() !== 200
    || !mobileControls.reset
    || !mobileControls.restart
    || mobileControls.restart.label !== 'Restart'
    || mobileControls.restart.height < 44
    || mobileControls.horizontalOverflow) {
    throw new Error(`SteamShine mobile recovery controls failed: ${JSON.stringify(mobileControls)}`);
  }
  securityResults.diagnostics_reset_mobile = true;
  securityResults.mobile_lifecycle_controls = mobileControls;

  /** Exercise the real PTY transport, session tabs, replay, and mobile geometry. */
  await steamshinePage.setViewportSize({ width: 1280, height: 800 });
  const terminalResponse = await steamshinePage.goto(`${baseUrl}/steamshine/terminal`, { waitUntil: 'domcontentloaded' });
  if (terminalResponse?.status() !== 200) throw new Error(`SteamShine Terminal returned ${terminalResponse?.status()}.`);
  await steamshinePage.getByRole('heading', { name: 'Terminal', exact: true }).waitFor({ timeout: 5000 });
  try {
    await steamshinePage.waitForFunction(() => document.querySelector('#term-connection')?.dataset.state === 'open', undefined, { timeout: 10000 });
  } catch (error) {
    const diagnostics = await steamshinePage.evaluate(async () => ({
      connectionState: document.querySelector('#term-connection')?.dataset.state,
      connectionLabel: document.querySelector('#term-connection')?.textContent?.trim(),
      terminalStatus: await fetch('/api/steamshine/v1/terminal/status').then((response) => response.json()),
    }));
    throw new Error(`Terminal WebSocket did not become ready: ${JSON.stringify({ diagnostics, websocketErrors })}`, { cause: error });
  }
  const firstTerminalId = await steamshinePage.locator('.terminal-tab.active [data-terminal-session]').getAttribute('data-terminal-session');
  if (!firstTerminalId) throw new Error('Terminal did not create its initial session tab.');
  const terminalInput = steamshinePage.locator('.xterm-helper-textarea');
  await terminalInput.pressSequentially("printf 'STEAMSHINE_BROWSER_TERMINAL_OK:%s\\n' \"$PWD\"", { delay: 1 });
  await terminalInput.press('Enter');
  await steamshinePage.waitForFunction((expectedHome) => document.querySelector('.xterm-rows')?.textContent?.includes(`STEAMSHINE_BROWSER_TERMINAL_OK:${expectedHome}`), homeDirectory, { timeout: 5000 });

  // Queue terminal capability requests while this session is not displayed.
  // On replay xterm.js answers them, but those stale replies must never be
  // forwarded to the current shell prompt as strings such as `0;276;0c`.
  await terminalInput.pressSequentially("(sleep 3; printf '\\033[c\\033[>c') &", { delay: 1 });
  await terminalInput.press('Enter');

  await steamshinePage.locator('#term-new').click();
  await steamshinePage.waitForFunction(() => document.querySelectorAll('.terminal-tab').length === 2, undefined, { timeout: 5000 });
  await steamshinePage.waitForFunction(() => document.querySelector('#term-connection')?.dataset.state === 'open', undefined, { timeout: 10000 });
  await steamshinePage.waitForTimeout(3500);
  await steamshinePage.locator(`[data-terminal-session="${firstTerminalId}"]`).click();
  await steamshinePage.waitForFunction(() => document.querySelector('#term-connection')?.dataset.state === 'open', undefined, { timeout: 10000 });
  await steamshinePage.waitForFunction((expectedHome) => document.querySelector('.xterm-rows')?.textContent?.includes(`STEAMSHINE_BROWSER_TERMINAL_OK:${expectedHome}`), homeDirectory, { timeout: 5000 });
  await steamshinePage.waitForTimeout(250);
  const replayProofFile = join(homeDirectory, 'terminal-replay-input-ok');
  await terminalInput.pressSequentially("printf 'STEAMSHINE_TERMINAL_REPLAY_INPUT_OK\\n' > \"$HOME/terminal-replay-input-ok\"", { delay: 1 });
  await terminalInput.press('Enter');
  let replayProof = '';
  for (let attempt = 0; attempt < 50 && replayProof.trim() !== 'STEAMSHINE_TERMINAL_REPLAY_INPUT_OK'; ++attempt) {
    try { replayProof = await readFile(replayProofFile, 'utf8'); } catch { await new Promise((resolve) => setTimeout(resolve, 100)); }
  }
  if (replayProof.trim() !== 'STEAMSHINE_TERMINAL_REPLAY_INPUT_OK') {
    throw new Error('Terminal replay responses contaminated or blocked the next shell command.');
  }
  await terminalInput.pressSequentially('seq 1 240', { delay: 1 });
  await terminalInput.press('Enter');
  await steamshinePage.waitForFunction(() => document.querySelector('.xterm-rows')?.textContent?.includes('240'), undefined, { timeout: 5000 });
  await terminalInput.pressSequentially("printf '\\033[?1000h'", { delay: 1 });
  await terminalInput.press('Enter');
  const terminalStatus = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/terminal/status')).json());
  const activeTerminal = terminalStatus.sessions?.find((session) => session.id === firstTerminalId);
  if (!activeTerminal) throw new Error('Active Terminal session disappeared before the touch-scroll test.');

  await steamshinePage.setViewportSize({ width: 320, height: 700 });
  await steamshinePage.waitForFunction(() => {
    const root = document.querySelector('#terminal-root');
    const viewportBottom = (window.visualViewport?.offsetTop ?? 0) + (window.visualViewport?.height ?? window.innerHeight);
    return root !== null && root.getBoundingClientRect().bottom <= viewportBottom + 1;
  }, undefined, { timeout: 5000 });
  const terminalMobileGeometry = await steamshinePage.evaluate(() => ({
    horizontalOverflow: document.documentElement.scrollWidth > window.innerWidth,
    helperTargets: [...document.querySelectorAll('.terminal-keybar button')].map((button) => {
      const rect = button.getBoundingClientRect();
      return { width: rect.width, height: rect.height };
    }),
    rootBottom: document.querySelector('#terminal-root')?.getBoundingClientRect().bottom ?? Number.POSITIVE_INFINITY,
    viewportBottom: (window.visualViewport?.offsetTop ?? 0) + (window.visualViewport?.height ?? window.innerHeight),
  }));
  if (terminalMobileGeometry.horizontalOverflow
    || terminalMobileGeometry.helperTargets.some(({ width, height }) => width < 44 || height < 44)
    || terminalMobileGeometry.rootBottom > terminalMobileGeometry.viewportBottom + 1) {
    throw new Error(`SteamShine Terminal mobile geometry failed: ${JSON.stringify(terminalMobileGeometry)}`);
  }
  const terminalTouchScroll = await steamshinePage.evaluate(async () => {
    const target = document.querySelector('.terminal-host .xterm-screen');
    const viewport = document.querySelector('.terminal-host .xterm-viewport');
    if (!target || !viewport) return { error: 'terminal touch target is missing' };
    const dispatchTouch = (type, clientY, active) => {
      const event = new Event(type, { bubbles: true, cancelable: true });
      const touch = { identifier: 7, clientY, pageY: clientY };
      Object.defineProperties(event, {
        touches: { value: active ? [touch] : [] },
        changedTouches: { value: [touch] },
      });
      target.dispatchEvent(event);
    };
    const before = viewport.scrollTop;
    dispatchTouch('touchstart', 120, true);
    dispatchTouch('touchmove', 190, true);
    dispatchTouch('touchmove', 260, true);
    const synchronous = viewport.scrollTop;
    await new Promise((resolve) => requestAnimationFrame(resolve));
    await new Promise((resolve) => requestAnimationFrame(resolve));
    const framed = viewport.scrollTop;
    dispatchTouch('touchend', 260, false);
    await new Promise((resolve) => setTimeout(resolve, 250));
    const glided = viewport.scrollTop;
    return { before, synchronous, framed, glided };
  });
  let remoteScrollPosition = 0;
  if (activeTerminal.persistent) {
    const result = await execFile('tmux', ['display-message', '-p', '-t', activeTerminal.name, '#{scroll_position}'], {
      env: { ...isolatedServerEnvironment, TMUX_TMPDIR: join(homeDirectory, 'run') },
    });
    remoteScrollPosition = Number.parseInt(result.stdout.trim(), 10);
  }
  const localScrollFailed = !activeTerminal.persistent && (
    terminalTouchScroll.before <= 0
    || terminalTouchScroll.synchronous !== terminalTouchScroll.before
    || terminalTouchScroll.framed > terminalTouchScroll.before - 190
    || terminalTouchScroll.glided > terminalTouchScroll.framed - 50
  );
  const remoteScrollFailed = activeTerminal.persistent && (
    !Number.isFinite(remoteScrollPosition)
    || remoteScrollPosition <= 0
    || terminalTouchScroll.framed !== terminalTouchScroll.before
  );
  if (terminalTouchScroll.error || localScrollFailed || remoteScrollFailed) {
    throw new Error(`SteamShine Terminal touch scrolling failed: ${JSON.stringify(terminalTouchScroll)}`);
  }
  terminalTouchScroll.backend = activeTerminal.persistent ? 'tmux' : 'xterm';
  terminalTouchScroll.remoteScrollPosition = remoteScrollPosition;
  securityResults.terminal_multiple_sessions = true;
  securityResults.terminal_history_replay = true;
  securityResults.terminal_replay_input_isolation = true;
  securityResults.terminal_mobile_geometry = terminalMobileGeometry;
  securityResults.terminal_mobile_touch_scroll = terminalTouchScroll;

  await terminalInput.pressSequentially("printf '\\033[?1000l'", { delay: 1 });
  await terminalInput.press('Enter');

  await steamshinePage.locator('#term-end').click();
  await steamshinePage.getByRole('alertdialog').getByRole('button', { name: 'End session' }).click();
  await steamshinePage.waitForFunction(() => document.querySelectorAll('.terminal-tab').length === 1, undefined, { timeout: 5000 });
  await steamshinePage.setViewportSize({ width: 800, height: 1280 });

  const concurrentUpstreamResponse = await authenticatedPage.reload({ waitUntil: 'networkidle' });
  if (concurrentUpstreamResponse?.status() !== 200) {
    throw new Error(`Upstream session did not remain valid with SteamShine open: ${concurrentUpstreamResponse?.status()}.`);
  }
  const concurrentSteamshineStatus = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (concurrentSteamshineStatus !== 200) throw new Error('SteamShine session did not remain valid with upstream open.');
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
  await steamshinePage.getByRole('heading', { name: 'Stream', exact: true }).waitFor({ timeout: 5000 });
  const streamState = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/status')).json().then((value) => value.stream_negotiation));
  if (streamState?.schema_version !== 1 || streamState?.poll_interval_ms !== 2000) {
    throw new Error(`Stream negotiation schema or polling bound changed: ${JSON.stringify(streamState)}`);
  }
  await steamshinePage.getByRole('heading', { name: 'Recording', exact: true }).waitFor({ timeout: 5000 });
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
  const fallbackHref = await steamshinePage.getByRole('link', { name: 'Sunshine settings' }).first().getAttribute('href');
  if (fallbackHref !== '/sunshine/config') throw new Error(`Unexpected stream rollback route: ${fallbackHref}`);
  await steamshinePage.locator('#open-stream-profiles').click();
  const profileDialog = steamshinePage.getByRole('dialog', { name: 'Client profiles' });
  await profileDialog.waitFor({ timeout: 5000 });
  await profileDialog.locator(`.profile-row:has-text("${profilePayload.client_id}")`).click();
  await profileDialog.locator('#profile-policy select[name="codec_policy"]').waitFor({ timeout: 5000 });
  const editorNetworkClass = await profileDialog.locator('#profile-policy input[name="network_class"]').inputValue();
  if (editorNetworkClass !== profilePayload.network_class) {
    throw new Error(`Client profile editor opened the wrong entry: ${editorNetworkClass}`);
  }
  const editorCodec = await profileDialog.locator('#profile-policy select[name="codec_policy"]').inputValue();
  if (editorCodec !== profilePayload.codec_policy) {
    throw new Error(`Client profile editor did not prefill the saved codec policy: ${editorCodec}`);
  }
  const statusRoute = '**/api/steamshine/v1/status';
  await steamshinePage.route(statusRoute, async (route) => route.fulfill({ status: 200, contentType: 'application/json', body: '{' }));
  await steamshinePage.goto(`${baseUrl}/steamshine/stream`, { waitUntil: 'domcontentloaded' });
  await steamshinePage.getByRole('heading', { name: 'Stream', exact: true }).waitFor({ timeout: 5000 });
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
  if (!securityResults.csp_header.includes("style-src 'self' 'unsafe-inline'") || !securityResults.csp_header.includes("style-src-attr 'unsafe-inline'")) {
    throw new Error('SteamShine Monitor CSP does not permit the terminal renderer runtime styles.');
  }
  if (!securityResults.csp_header.includes(`connect-src 'self' wss://127.0.0.1:${basePort + 2}`)) {
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
  securityResults.terminal_same_origin_websocket = appAsset.includes('`wss://${location.host}${terminalStreamPath}`')
    && appAsset.includes('standardHttpsOrigin');
  if (!securityResults.terminal_same_origin_websocket) throw new Error('The Terminal does not prefer the same-origin secure WebSocket route for HTTPS proxies.');
  securityResults.diagnostics_status = await steamshinePage.evaluate(async () => {
    const response = await fetch('/api/steamshine/v1/diagnostics');
    await response.text();
    return response.status;
  });
  if (securityResults.diagnostics_status !== 200) throw new Error('SteamShine structured diagnostics endpoint is unavailable.');
  await steamshinePage.goto(`${baseUrl}/steamshine/diagnostics`, { waitUntil: 'networkidle' });
  await steamshinePage.getByRole('heading', { name: 'Diagnostics' }).waitFor({ timeout: 5000 });
  await steamshinePage.getByRole('heading', { name: 'Service log' }).waitFor({ timeout: 5000 });
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
  // Stop PIN-page polling before deliberately invalidating its Basic credentials.
  // The isolated request context still exercises the real credential-change endpoint.
  const upstreamRequest = authenticatedPage.context().request;
  await authenticatedPage.close();
  const credentialChange = (await upstreamRequest.post(`${baseUrl}/api/password`, {
    headers: { 'X-CSRF-Token': upstreamCsrf, Origin: baseUrl },
    data: { currentUsername: 'web-e2e', currentPassword: 'web-e2e-password', newUsername: 'web-e2e', newPassword: 'web-e2e-password-2', confirmNewPassword: 'web-e2e-password-2' },
  })).status();
  if (credentialChange !== 200) throw new Error(`Credential change returned ${credentialChange}.`);
  securityResults.credential_change_session_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (securityResults.credential_change_session_status !== 401) throw new Error('Credential change did not invalidate the SteamShine session.');
  await steamshinePage.goto(`${baseUrl}/steamshine/login`, { waitUntil: 'networkidle' });
  await steamshinePage.locator('#login input[name="username"]').fill('web-e2e');
  await steamshinePage.locator('#login input[name="password"]').fill('web-e2e-password-2');
  await steamshinePage.locator('#login button').click();
  await waitForMonitor(steamshinePage);
  await steamshinePage.locator('#mobile-restart').click();
  await steamshinePage.getByRole('alertdialog').getByRole('button', { name: 'Cancel' }).click();
  await steamshinePage.locator('#mobile-quit').click();
  await steamshinePage.getByRole('alertdialog').getByRole('button', { name: 'Cancel' }).click();
  await steamshinePage.locator('#mobile-logout').click();
  await steamshinePage.getByRole('heading', { name: 'Sign in' }).waitFor({ timeout: 5000 });
  securityResults.logout_session_status = await steamshinePage.evaluate(async () => (await fetch('/api/steamshine/v1/session')).status);
  if (securityResults.logout_session_status !== 401) throw new Error('Logout did not invalidate the SteamShine session.');
  const loginRateContext = await request.newContext({ ignoreHTTPSErrors: true, extraHTTPHeaders: { Origin: baseUrl } });
  const successfulLoginStatuses = [];
  let lifecycleCookie = '';
  let lifecycleCsrf = '';
  for (let attempt = 0; attempt < 6; ++attempt) {
    const response = await loginRateContext.post(`${baseUrl}/api/steamshine/v1/auth/login`, {
      data: { username: 'web-e2e', password: 'web-e2e-password-2' },
    });
    successfulLoginStatuses.push(response.status());
    const payload = await response.json();
    if (!lifecycleCookie) {
      lifecycleCookie = (response.headers()['set-cookie'] || '').match(/steamshine_session=([^;]+)/)?.[1] || '';
      lifecycleCsrf = payload.csrf_token || '';
    }
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
  await steamshineContext.close();
  if (consoleErrors.length || failedRequests.length) {
    throw new Error(`Browser errors: ${consoleErrors.join('; ')}; failed requests: ${failedRequests.join('; ')}`);
  }
  await authenticatedContext.close();
  if (!lifecycleCookie || !lifecycleCsrf) throw new Error('Could not retain an authenticated session for the quit lifecycle test.');
  const serverExit = new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('SteamShine did not exit after the authenticated quit request.')), 10000);
    server.once('exit', (code, signal) => {
      clearTimeout(timeout);
      resolve({ code, signal });
    });
  });
  const lifecycleContext = await request.newContext({
    ignoreHTTPSErrors: true,
    extraHTTPHeaders: { Cookie: `steamshine_session=${lifecycleCookie}`, Origin: baseUrl, 'X-SteamShine-CSRF-Token': lifecycleCsrf },
  });
  const quitResponse = await lifecycleContext.post(`${baseUrl}/api/steamshine/v1/system/quit`, { data: {} });
  securityResults.quit_status = quitResponse.status();
  await quitResponse.text();
  await lifecycleContext.dispose();
  const quitResult = await serverExit;
  securityResults.quit_exit_code = quitResult.code;
  if (securityResults.quit_status !== 200 || quitResult.code !== 0) {
    throw new Error(`Authenticated quit failed: HTTP ${securityResults.quit_status}, exit ${quitResult.code}, signal ${quitResult.signal}`);
  }
  const serviceLog = await readFile(logFile, 'utf8').catch(() => '');
  securityResults.secrets_absent_from_service_log = !['web-e2e-password', 'web-e2e-password-2', '1234', 'bad'].some((secret) => serviceLog.includes(secret));
  if (!securityResults.secrets_absent_from_service_log) {
    throw new Error('SteamShine service log exposed a browser credential or pairing PIN.');
  }
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
    steamshine_saved_gpu_profile: 'passed',
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
    if (pages.at(-1)) await pages.at(-1).screenshot({ path: screenshotFile, fullPage: true }).catch(() => {});
  }
  await writeFile(join(reportDirectory, 'web-browser-e2e-report.json'), JSON.stringify({
    browser: 'chromium',
    tested_url: baseUrl,
    error: String(error),
    console_errors: consoleErrors,
    failed_requests: failedRequests,
    websocket_errors: websocketErrors,
    screenshot: screenshotFile,
  }, null, 2) + '\n');
  throw error;
} finally {
  await cleanup();
}
