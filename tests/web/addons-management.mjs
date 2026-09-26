/** @file Test Addon workflows and administrator dialogs with simulated APIs; no host mutations. */
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir } from 'node:fs/promises';
import { resolve, extname } from 'node:path';
import { chromium } from '@playwright/test';

const assets = resolve('src_assets/common/assets/steamshine');
const uuid = '19d3483c-a24c-4c26-a7fc-e5d622399d1d';
const libraryId = '0123456789abcdef01234567';
let authorized = false;
let mounted = false;
let configured = false;
let authorizations = 0;
let restored = 0;
let updated = 0;
let applied = 0;
let homeCombo = { enabled: false, inputs: [], hold_ms: 1000 };
const homeComboSaves = [];
const errors = [];
const server = createServer(async (request, response) => {
  try {
    const suffix = request.url.slice('/steamshine/'.length);
    const file = suffix.includes('.') ? resolve(assets, suffix) : resolve(assets, 'index.html');
    if (!file.startsWith(assets + '/')) throw new Error('Invalid asset');
    response.setHeader('Content-Type', { '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html', '.png': 'image/png' }[extname(file)] || 'application/octet-stream');
    response.end(await readFile(file));
  } catch { response.writeHead(404).end(); }
});
await new Promise((ready) => server.listen(0, '127.0.0.1', ready));
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({ executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE || '/usr/bin/chromium', headless: true, args: ['--no-sandbox'] });
try {
  const page = await browser.newPage({ viewport: { width: 320, height: 800 } });
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route('**/api/steamshine/v1/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace('/api/steamshine/v1', '');
    const body = request.method() === 'POST' ? request.postDataJSON() : {};
    const reply = (payload, status = 200) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(payload) });
    if (request.method() === 'POST') assert.equal(request.headers()['x-steamshine-csrf-token'], 'fixture-csrf');
    if (path === '/setup/status') return reply({ configured: true });
    if (path === '/session') return reply({ username: 'fixture', csrf_token: 'fixture-csrf' });
    if (path === '/input/home-combo') {
      if (request.method() === 'POST') {
        homeComboSaves.push(body);
        homeCombo = { enabled: body.inputs.length > 0, inputs: body.inputs, hold_ms: body.hold_ms };
      }
      return reply({ ...homeCombo, available_inputs: ['START', 'BACK', 'A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT'], min_hold_ms: 200, max_hold_ms: 10000, max_inputs: 4 });
    }
    if (path === '/addons/decky') return reply({ installed: true, version: 'fixture', management_available: authorized });
    if (path === '/addons/steam-cache') return reply({ home_free_bytes: 300 * 1024 ** 3, libraries: [{
      id: libraryId, path: '/run/media/deck/Samsung2TB/SteamLibrary',
      shadercache: { state: configured ? 'internal' : 'library', configured, can_configure: !configured, reason: configured ? 'Already configured' : 'Ready to configure', target: '/home/deck/.local/share/Steam/InternalCache/shadercache' },
      compatdata: { target: '/home/deck/.local/share/Steam/InternalCache/compatdata' },
    }] });
    if (path === '/addons/steam-cache/configure') {
      assert.deepEqual(body, { library_id: libraryId });
      configured = true;
      return reply({ success: true, message: 'Cache configured', backup_path: '/saved/cache-backup' });
    }
    if (path === '/addons/storage') return reply({ saved_settings: mounted, volumes: [{ uuid, label: 'NVME2TB <safe>', size_bytes: 2 * 1024 ** 4, target: '/run/media/deck/Samsung2TB', origin: 'SteamOS backup', mounted, persistent: mounted, connected: true, can_restore: !mounted, reason: 'Saved mount available' }] });
    if (path === '/system/authorize') {
      authorizations++;
      if (body.password !== 'fixture-pass') return reply({ message: 'Authentication failed' }, 403);
      authorized = true;
      return reply({ success: true });
    }
    if (path === '/addons/storage/action' || path === '/addons/decky/action' || path.endsWith('/activate')) {
      if (!authorized) return reply({ code: 'admin_authorization_required' }, 403);
      if (path === '/addons/storage/action') { assert.equal(body.action, 'restore'); assert.equal(body.uuid, uuid); mounted = true; restored++; }
      if (path === '/addons/decky/action') { assert.equal(body.action, 'update'); updated++; }
      if (path.endsWith('/activate')) applied++;
      return reply({ success: true, message: 'Completed', applied: ['power_cap'], skipped: [] });
    }
    if (path === '/gpu/capabilities') return reply({ gpu_name: 'Fixture GPU', runtime_write_authorized: authorized, cpu_governors: ['performance'] });
    if (path === '/gpu/profiles') return reply({ profiles: [{ name: 'Balanced', builtin: true, description: 'Balanced profile', power_cap_watts: 200, cpu_governor: 'performance' }], active: '' });
    if (path === '/system/metrics') return reply({ gpu: {} });
    return reply({});
  });
  await page.goto(`${base}/steamshine/addons`);
  await page.getByRole('heading', { name: 'Storage recovery', exact: true }).waitFor();
  // Home button: choose Start + Back held for three seconds, then turn it off.
  const homeCard = page.locator('#home-combo');
  await homeCard.getByRole('button', { name: 'Start', exact: true }).click();
  await homeCard.getByRole('button', { name: 'Back / Select', exact: true }).click();
  await homeCard.getByLabel('Hold time (seconds)').fill('3');
  await homeCard.getByText('Hold Start + Back / Select for 3.0 s to press Home.').waitFor();
  await homeCard.getByLabel('Hold time (seconds)').fill('20');
  assert.equal(await homeCard.getByRole('button', { name: 'Save', exact: true }).isDisabled(), true);
  await homeCard.getByLabel('Hold time (seconds)').fill('3');
  await homeCard.getByRole('button', { name: 'Save', exact: true }).click();
  await page.locator('#home-combo [data-combo-input="START"][aria-pressed="true"]').waitFor();
  assert.deepEqual(homeComboSaves.at(-1), { inputs: ['START', 'BACK'], hold_ms: 3000 });
  for (const name of ['A', 'B']) await page.locator('#home-combo').getByRole('button', { name, exact: true }).click();
  assert.equal(await page.locator('#home-combo').getByRole('button', { name: 'X', exact: true }).isDisabled(), true);
  await page.locator('#home-combo').getByRole('button', { name: 'Turn off', exact: true }).click();
  await page.locator('#home-combo').getByText('Off. Controllers with a Guide button can still use it.').waitFor();
  assert.deepEqual(homeComboSaves.at(-1).inputs, []);

  await page.getByRole('button', { name: 'Use internal storage' }).click();
  await page.locator('#cache-result').filter({ hasText: '/saved/cache-backup' }).waitFor();
  assert.equal(await page.getByRole('button', { name: 'Use internal storage' }).count(), 0);

  await page.getByRole('button', { name: 'Restore saved mount' }).click();
  await page.getByRole('button', { name: 'Restore mount', exact: true }).click();
  await page.getByRole('dialog').waitFor();
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  assert.equal(restored, 0);
  assert.equal(authorizations, 0);
  await page.getByRole('button', { name: 'Restore saved mount' }).click();
  await page.getByRole('button', { name: 'Restore mount', exact: true }).click();
  await page.getByLabel('Administrator password', { exact: true }).fill('fixture-pass');
  await page.getByRole('button', { name: 'Authorize', exact: true }).click();
  await page.getByText('Mounted · automatic mount enabled', { exact: true }).waitFor();
  assert.equal(restored, 1);
  assert.equal(await page.getByLabel('Administrator password', { exact: true }).count(), 0);
  assert.equal(await page.evaluate(() => localStorage.length + sessionStorage.length), 0);

  authorized = false;
  await page.getByRole('button', { name: 'Update / repair stable' }).click();
  await page.getByRole('button', { name: 'Continue', exact: true }).click();
  await page.getByLabel('Administrator password', { exact: true }).fill('wrong-fixture');
  await page.getByRole('button', { name: 'Authorize', exact: true }).click();
  await page.getByRole('status').filter({ hasText: 'Authentication failed' }).waitFor();
  assert.equal(await page.getByLabel('Administrator password', { exact: true }).inputValue(), '');
  await page.getByLabel('Administrator password', { exact: true }).fill('fixture-pass');
  await page.getByRole('button', { name: 'Authorize', exact: true }).click();
  await page.getByRole('button', { name: 'Update / repair stable' }).waitFor({ state: 'visible' });
  await page.waitForFunction(() => !document.querySelector('[data-decky-action="update"]').disabled);
  assert.equal(updated, 1);
  const attempts = authorizations;
  await page.getByRole('button', { name: 'Update / repair stable' }).click();
  await page.getByRole('button', { name: 'Continue', exact: true }).click();
  await page.waitForFunction(() => !document.querySelector('[data-decky-action="update"]').disabled);
  assert.equal(updated, 2);
  assert.equal(authorizations, attempts);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);

  authorized = false;
  await page.goto(`${base}/steamshine/gpu`);
  await page.locator('[data-activate="Balanced"]').click();
  await page.getByLabel('Administrator password', { exact: true }).fill('fixture-pass');
  await page.getByRole('button', { name: 'Authorize', exact: true }).click();
  await page.getByText('Runtime writes are authorized', { exact: false }).waitFor();
  assert.equal(applied, 1);
  assert.deepEqual(errors, []);
  await mkdir('dist/addons-browser', { recursive: true });
  await page.goto(`${base}/steamshine/addons`);
  await page.getByRole('heading', { name: 'Storage recovery', exact: true }).waitFor();
  await page.screenshot({ path: 'dist/addons-browser/addons-320.png', fullPage: true });
  console.log('PASS: Home combination, cache setup, storage restore/cancel, password retry/clearing, Decky reuse, GPU authentication, and 320px layout.');
} finally {
  await browser.close();
  await new Promise((done) => server.close(done));
}
