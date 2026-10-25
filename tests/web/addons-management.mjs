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
let remembered = 0;
let updated = 0;
let applied = 0;
let shortcutList = [];
const shortcutSaves = [];
const shortcutDeletes = [];
let nextShortcutId = 1;
let turboState = { enabled: false, modifier: 'START', hz: 10 };
const turboSaves = [];
let x52Connected = false;
let x52LastButton = -1;
const x52Actions = [];
const recommendedX52 = { name: 'Recommended', enabled: false, buttons: {}, lighting: { brightness: 64, blink: false },
  display: { brightness: 80, shift: false, lines: ['SteamShine X52', 'Ready', ''], hours_24: true, date_format: 'YYMMDD', clock_offsets_minutes: [0, 0] } };
let x52Profiles = [recommendedX52];
let x52Active = 'Recommended';
let x52InputEnabled = true;
const errors = [];
/** Return the simulated controller-shortcut API payload. */
function shortcutPayload() {
  const inputs = ['START', 'BACK', 'A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT'];
  return { shortcuts: shortcutList, available_inputs: inputs, available_outputs: ['HOME', ...inputs], min_hold_ms: 200, max_hold_ms: 10000, max_inputs: 4, max_shortcuts: 8, max_name_length: 40 };
}
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
    if (path === '/input/turbo') {
      if (request.method() === 'POST') { turboSaves.push(body); turboState = body; }
      const buttons = ['START', 'BACK', 'A', 'B', 'X', 'Y', 'LB', 'RB', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT'];
      return reply({ ...turboState, available_buttons: buttons, min_hz: 1, max_hz: 30 });
    }
    if (path === '/addons/x52') {
      if (request.method() === 'POST') {
        assert.equal(x52Connected, true);
        x52Actions.push(body);
        if (body.action === 'save') {
          x52Profiles = [...x52Profiles.filter((profile) => profile.name !== body.profile.name), body.profile];
          x52Active = body.profile.name;
        } else if (body.action === 'activate') x52Active = body.name;
        else if (body.action === 'delete') {
          x52Profiles = x52Profiles.filter((profile) => profile.name !== body.name);
          if (x52Active === body.name) x52Active = 'Recommended';
        } else if (body.action === 'enable') {
          x52Profiles = x52Profiles.map((profile) => profile.name === x52Active ? { ...profile, enabled: body.enabled } : profile);
        } else if (body.action === 'input') x52InputEnabled = body.enabled;
      }
      return reply({ connected: x52Connected, keyboard_ready: true, last_button: x52LastButton, active: x52Active, input_enabled: x52InputEnabled, profiles: x52Profiles,
        capabilities: { individual_led_colors: false, mfd_lines: 3, mfd_characters: 16, brightness_max: 128 } });
    }
    if (path.startsWith('/input/shortcuts')) {
      if (request.method() === 'POST') {
        shortcutSaves.push(body);
        const saved = { ...body, id: body.id || `s${nextShortcutId++}` };
        shortcutList = body.id ? shortcutList.map((item) => (item.id === body.id ? saved : item)) : [...shortcutList, saved];
        return reply({ ...shortcutPayload(), saved });
      }
      if (request.method() === 'DELETE') {
        const id = path.split('/').pop();
        shortcutDeletes.push(id);
        shortcutList = shortcutList.filter((item) => item.id !== id);
      }
      return reply(shortcutPayload());
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
      if (path === '/addons/storage/action') {
        if (body.action === 'remember') remembered++;
        else { assert.equal(body.action, 'restore'); assert.equal(body.uuid, uuid); mounted = true; restored++; }
      }
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
  await page.getByRole('heading', { name: 'Decky Loader', exact: true }).waitFor();
  assert.equal(await page.getByRole('heading', { name: 'Steam shader cache', exact: true }).count(), 0);
  assert.equal(await page.getByRole('heading', { name: 'Storage recovery', exact: true }).count(), 0);
  // Create Home (Start + Back, 3 s) and Quick Access (Start + Back + A, 1.5 s), then toggle and delete.
  const shortcutsCard = page.locator('#controller-shortcuts');
  await shortcutsCard.getByText('No shortcuts yet.').waitFor();
  await shortcutsCard.getByRole('button', { name: 'Add shortcut', exact: true }).click();
  const editor = page.locator('#shortcut-editor');
  const holdGroup = editor.getByRole('group', { name: 'Buttons to hold' });
  await holdGroup.getByRole('button', { name: 'Start', exact: true }).click();
  await holdGroup.getByRole('button', { name: 'Back / Select', exact: true }).click();
  await editor.getByLabel('Hold time (seconds)').fill('3');
  await editor.getByText('Hold Start + Back / Select for 3.0 s to send Home.').waitFor();
  await editor.getByLabel('Hold time (seconds)').fill('20');
  assert.equal(await editor.getByRole('button', { name: 'Save shortcut', exact: true }).isDisabled(), true);
  await editor.getByLabel('Hold time (seconds)').fill('3');
  await editor.getByRole('button', { name: 'Save shortcut', exact: true }).click();
  await page.locator('[data-shortcut-id="s1"]').waitFor();
  assert.deepEqual(shortcutSaves.at(-1), { id: '', name: '', inputs: ['START', 'BACK'], hold_ms: 3000, output: ['HOME'], enabled: true });

  await page.locator('#controller-shortcuts').getByRole('button', { name: 'Add shortcut', exact: true }).click();
  const quick = page.locator('#shortcut-editor');
  await quick.getByLabel('Name (optional)').fill('Decky');
  for (const name of ['Start', 'Back / Select', 'A']) await quick.getByRole('group', { name: 'Buttons to hold' }).getByRole('button', { name, exact: true }).click();
  await quick.getByLabel('Hold time (seconds)').fill('1.5');
  await quick.getByRole('button', { name: 'Quick Access (Home + A)', exact: true }).click();
  await page.locator('#shortcut-editor').getByText('Hold Start + Back / Select + A for 1.5 s to send Home, then A.').waitFor();
  const sendGroup = page.locator('#shortcut-editor').getByRole('group', { name: 'Keys to send' });
  assert.equal(await sendGroup.locator('[aria-pressed="true"]').count(), 2);
  await page.locator('#shortcut-editor').getByRole('button', { name: 'Save shortcut', exact: true }).click();
  await page.locator('[data-shortcut-id="s2"]').getByRole('heading', { name: 'Decky', exact: true }).waitFor();
  assert.deepEqual(shortcutSaves.at(-1), { id: '', name: 'Decky', inputs: ['START', 'BACK', 'A'], hold_ms: 1500, output: ['HOME', 'A'], enabled: true });

  // Edit keeps the identifier and the send order chosen by tapping.
  await page.locator('[data-shortcut-id="s2"]').getByRole('button', { name: 'Edit', exact: true }).click();
  const edit = page.locator('#shortcut-editor');
  await edit.getByRole('group', { name: 'Keys to send' }).getByRole('button', { name: /Home$/ }).click();
  await page.locator('#shortcut-editor').getByRole('group', { name: 'Keys to send' }).getByRole('button', { name: /Home$/ }).click();
  await page.locator('#shortcut-editor').getByRole('button', { name: 'Save shortcut', exact: true }).click();
  await page.locator('[data-shortcut-id="s2"]').getByText('to send A, then Home.', { exact: false }).waitFor();
  assert.deepEqual(shortcutSaves.at(-1).output, ['A', 'HOME']);
  assert.equal(shortcutSaves.at(-1).id, 's2');

  await page.locator('[data-shortcut-id="s1"] [data-shortcut-toggle]').uncheck();
  await page.waitForFunction(() => document.querySelector('[data-shortcut-id="s1"] [data-shortcut-toggle]')?.checked === false && !document.querySelector('[data-shortcut-id="s1"] [data-shortcut-toggle]').disabled);
  assert.equal(shortcutSaves.at(-1).enabled, false);
  assert.equal(shortcutSaves.at(-1).id, 's1');

  await page.locator('[data-shortcut-id="s1"]').getByRole('button', { name: 'Delete', exact: true }).click();
  await page.getByRole('alertdialog').getByRole('button', { name: 'Delete', exact: true }).click();
  await page.locator('[data-shortcut-id="s1"]').waitFor({ state: 'detached' });
  assert.deepEqual(shortcutDeletes, ['s1']);
  assert.equal(shortcutList.length, 1);

  // Turbo: the switch saves at once; the combination button and speed save with Save.
  const turboCard = page.locator('#controller-turbo');
  await turboCard.getByText('Off: hold Start and press a button to toggle its turbo at 10 presses per second.').waitFor();
  await turboCard.getByLabel('Enable turbo').check();
  await page.waitForFunction(() => !document.querySelector('#turbo-enabled').disabled);
  assert.deepEqual(turboSaves.at(-1), { enabled: true, modifier: 'START', hz: 10 });
  await turboCard.getByRole('group', { name: 'Combination button' }).getByRole('button', { name: 'Back / Select', exact: true }).click();
  assert.equal(await turboCard.locator('[data-turbo-modifier][aria-pressed="true"]').count(), 1);
  await turboCard.getByLabel('Presses per second').fill('40');
  assert.equal(await turboCard.getByRole('button', { name: 'Save', exact: true }).isDisabled(), true);
  await turboCard.getByLabel('Presses per second').fill('12');
  await turboCard.getByText('On: hold Back / Select and press a button to toggle its turbo at 12 presses per second.').waitFor();
  await turboCard.getByRole('button', { name: 'Save', exact: true }).click();
  await page.waitForFunction(() => !document.querySelector('#turbo-save').disabled);
  assert.deepEqual(turboSaves.at(-1), { enabled: true, modifier: 'BACK', hz: 12 });
  await turboCard.getByLabel('Enable turbo').uncheck();
  await page.waitForFunction(() => !document.querySelector('#turbo-enabled').disabled);
  assert.deepEqual(turboSaves.at(-1), { enabled: false, modifier: 'BACK', hz: 12 });

  // X52 controls are gated by connection; edits and button assignments persist in named profiles.
  const x52Card = page.locator('#x52-addon');
  await x52Card.getByText('Disconnected', { exact: true }).waitFor();
  assert.equal(await x52Card.locator('#x52-enabled').isDisabled(), true);
  assert.equal(await x52Card.locator('#x52-input-enabled').isDisabled(), true);
  assert.equal(await x52Card.locator('#x52-settings').evaluate((element) => element.open), false);
  x52Connected = true;
  await page.locator('#refresh-addons').click();
  await x52Card.getByText('Connected', { exact: true }).waitFor();
  assert.equal(await x52Card.locator('#x52-input-enabled').isChecked(), true);
  await x52Card.locator('#x52-input-enabled').uncheck();
  await page.waitForFunction(() => document.querySelector('#x52-input-enabled')?.checked === false);
  assert.deepEqual(x52Actions.at(-1), { action: 'input', enabled: false });
  await page.locator('#refresh-addons').click();
  assert.equal(await x52Card.locator('#x52-input-enabled').isChecked(), false);
  await x52Card.locator('#x52-settings summary').click();
  assert.equal(await x52Card.locator('#x52-settings').evaluate((element) => element.open), true);
  x52LastButton = 0;
  await x52Card.getByText('Last pressed: Trigger (1)').waitFor({ timeout: 5000 });
  await x52Card.locator('#x52-enabled').check();
  await page.waitForFunction(() => document.querySelector('#x52-enabled')?.checked === true && !document.querySelector('#x52-save')?.disabled);
  assert.deepEqual(x52Actions.at(-1), { action: 'enable', enabled: true });
  await x52Card.locator('#x52-name').fill('Flight');
  await x52Card.locator('#x52-led-brightness').fill('90');
  await x52Card.locator('[data-x52-button="0"]').selectOption('SPACE');
  await x52Card.locator('#x52-save').click();
  await page.waitForFunction(() => document.querySelector('#x52-profile')?.value === 'Flight');
  assert.equal(x52Actions.at(-1).profile.lighting.brightness, 90);
  assert.equal(x52Actions.at(-1).profile.buttons['0'], 'SPACE');
  await x52Card.locator('#x52-profile').selectOption('Recommended');
  await page.waitForFunction(() => document.querySelector('#x52-profile')?.value === 'Recommended');
  await x52Card.locator('#x52-profile').selectOption('Flight');
  await page.waitForFunction(() => document.querySelector('#x52-profile')?.value === 'Flight');
  await x52Card.locator('#x52-delete').click();
  await page.getByRole('alertdialog').getByRole('button', { name: 'Delete', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('#x52-profile')?.value === 'Recommended');
  assert.equal(x52Profiles.length, 1);
  x52Connected = false;
  await page.locator('#refresh-addons').click();
  await x52Card.getByText('Disconnected', { exact: true }).waitFor();
  assert.equal(await x52Card.locator('#x52-save').isDisabled(), true);

  await page.goto(`${base}/steamshine/settings`);
  await page.getByRole('heading', { name: 'Steam shader cache', exact: true }).waitFor();
  await page.getByRole('heading', { name: 'Storage recovery', exact: true }).waitFor();
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
  await page.getByRole('button', { name: 'Save mount settings' }).click();
  await page.waitForFunction(() => !document.querySelector('#save-mount-settings')?.disabled);
  assert.equal(remembered, 1);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);

  authorized = false;
  await page.goto(`${base}/steamshine/addons`);
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
  await page.getByRole('heading', { name: 'Decky Loader', exact: true }).waitFor();
  await page.screenshot({ path: 'dist/addons-browser/addons-320.png', fullPage: true });
  console.log('PASS: controller shortcuts, turbo, X52 connection/profile/button settings, storage, Decky, GPU, and 320px layout.');
} finally {
  await browser.close();
  await new Promise((done) => server.close(done));
}
