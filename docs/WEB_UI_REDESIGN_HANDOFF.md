# SteamShine Web UI redesign — handoff

Status as of 2026-07-27. Work lives on branch `feat/steamshine-web-ui-redesign`
(PR #8, https://github.com/souten-yd/SteamShine/pull/8). **CI is currently red**
for one known reason described below.

## What was built

A full redesign of the SteamShine web UI (`/steamshine/*`) plus three new
backend modules. All of it is committed and pushed; nothing is uncommitted.

### Frontend — `src_assets/common/assets/steamshine/`
Build-less: plain `app.js` (template-string router) + `app.css`. No bundler.

- **Theme**: near-black shell, chrome/silver gradient accents matching the
  SteamShine mark. Status colors (emerald/amber/red/sky) kept for legibility.
- **Layout**: desktop left sidebar; below 860px it becomes a fixed bottom tab
  bar plus a sticky mobile top bar (`.mobile-topbar` — logo + wordmark +
  logout, since the sidebar's brand and logout are hidden on mobile).
- **Nav** (`NAV` array, `DEFAULT_PAGE = 'monitor'`): Monitor, Apps, GPU,
  Settings, Pin, Clients, Terminal. **Dashboard and Logs were deliberately
  removed** at the user's request; `renderDashboard`/`renderLogs` are deleted.
- **Vendored**: `vendor/xterm/` (xterm.js 5.5.0 + CSS + LICENSE),
  `images/logo-mark{,-32,-64,-180,-320}.{svg,png}`.

### Backend — new files, all wired into `cmake/compile_definitions/common.cmake`
- `src/steamshine_hwmonitor.{cpp,h}` — read-only CPU/RAM/AMD-GPU telemetry from
  procfs/sysfs. Serves `GET /api/steamshine/v1/system/metrics`.
- `src/steamshine_gpuctl.{cpp,h}` — AMD GPU/CPU performance profiles.
  Built-ins (Silent/Balanced/Performance/OC) are scaled from probed hardware
  bounds; custom profiles persist to config key `steamshine_gpu_profiles`
  (JSON string). Writes to root-owned sysfs by briefly raising
  `CAP_DAC_OVERRIDE` around each individual write and dropping it immediately
  (`with_capability()`), with an allow-list of absolute paths resolved once at
  detection time and per-field clamping. `postinst` was updated to grant that
  capability alongside the existing `CAP_SYS_ADMIN`/`CAP_SYS_NICE`.
- `src/steamshine_terminal.{cpp,h}` — single PTY-backed shell via `forkpty()`,
  running as the same unprivileged user as Sunshine (no capability raised).
  Output fans out to WebSocket subscribers with a 64 KiB replay backlog.
- `src/confighttp.cpp` — new `steamshine_*` handlers for apps/config/gpu/
  terminal/metrics (all behind the existing `steamshine_session` cookie +
  CSRF scheme), plus a Boost.Beast WSS server on port offset
  `PORT_STEAMSHINE_TERMINAL` (base+2) for the terminal stream.

### CI
`classify-changes` in `.github/workflows/build-steamos.yml` gained a
`web-assets` fast path: a PR touching only `src_assets/common/assets/*`,
`tests/web/*`, `package.json`, `package-lock.json`, or `vite.config.js` now
runs npm/vite + the asset validators instead of a ~20 minute full C++ build.
The C++ web-server files stay in the `web` category and still force a full run.

## THE ONE THING THAT IS BROKEN

`tests/web/upstream-browser-e2e.mjs` still assumes the Dashboard page exists.
Around **line 154-171** it does:

```js
await steamshinePage.goto(`${baseUrl}/steamshine/dashboard`, ...);
...
await steamshinePage.getByText('Welcome, web-e2e').waitFor({ timeout: 5000 });  // line 161 — FAILS HERE
```

`/steamshine/dashboard` no longer has a renderer, so it falls through to
`renderMonitor`, which never prints `Welcome, <username>`. The wait times out
and the job fails at the "Stage, package, and smoke test the same binary" step.

**Fix**: point the test at the Monitor page and assert on something Monitor
actually renders. Both the initial navigation (line 154) and the responsive
loop (line 168) should use `/steamshine/monitor`. For the post-login assertion,
wait for a Monitor-specific element instead — e.g. the page header
`getByText('Monitor')`, or better, wait for the first metric tile to populate
(`.metric-tile` exists and its `.metric-value` is not `—`), since Monitor
fetches `/system/metrics` before it has content. Note Monitor polls every 2s,
so `waitUntil: 'networkidle'` may need replacing with an explicit locator wait.

Everything else in that file already passes, including the XSS-escaping check
(fixed in `13a05668`) and the tablet/Steam-Deck overflow check (fixed in
`7a8c8051`).

## Also unfinished

**Logo/icon.** The current `images/logo-mark.svg` is a hand-drawn flat "S"
that the user explicitly called ugly and accepted only as a placeholder. The
user then supplied a proper render, most recently as a **green-screen version**
(bright green `#00e000`-ish background) specifically so the background can be
keyed out. That work was interrupted and is NOT committed. To finish:

1. Take the user's green-screen PNG, chroma-key the green to transparent —
   `ffmpeg -i in.png -vf "chromakey=0x00e000:0.3:0.1,format=rgba" -update 1 out.png`
   (tune similarity/blend; check the result composited over both a light and a
   dark background, since the mark has soft translucent smoke edges that will
   otherwise keep a green fringe).
2. Regenerate `logo-mark-{32,64,180,320}.png` from it and `steamshine.png`
   (512px, used by README) — replacing the SVG-derived ones.
3. Verify legibility at 32px; the earlier photorealistic render turned into an
   unreadable dark blob at favicon size, which is why the flat SVG exists.

**No live end-to-end verification on real hardware.** The C++ builds cleanly
(verified in an Arch container with Boost.Beast), and the frontend was checked
against a mock API server, but nothing was exercised against a running
SteamShine on the Deck. In particular the GPU profile *write* path has never
executed — this dev session had no root, so `activate_profile()` writing to
`power1_cap` / `scaling_max_freq` / `pp_od_clk_voltage` is code-reviewed only.
The Terminal WebSocket has likewise never carried a real session.

## Build environment notes

This Steam Deck has **no C++ toolchain** (`no cmake`, `no gcc`). A distrobox
container named `sunshine-build` (Arch, with base-devel/cmake/clang/npm/
playwright) was created for building:

```bash
distrobox enter sunshine-build -- bash -c 'cd /home/deck/SteamShine/cmake-build-distrobox && cmake --build . --target sunshine -j24'
```

Binaries built there **cannot run on the host** — the container's glibc/Qt are
newer, so `ldd` reports missing versions. Use the CI artifact for anything
that must actually run on the Deck (`./steamshine.sh install --pr 8`).

`clang-format` and `actionlint` are installed in that container and must both
pass; CI enforces clang-format on `src/confighttp.cpp`, `src/config.*`, and
friends (it failed on this once already, commit `bfce1bb0`).

## Hazard worth knowing

`sunshine --creds <user> <pass>` resolves its credentials file through
`platf::appdata()`, which hardcodes the directory name `sunshine` and did NOT
respect an overridden `HOME` in the one-shot `--creds` invocation. Running it
during testing overwrote the **live** service's
`~/.config/sunshine/sunshine_state.json`, changing the real Web UI login to
`testadmin` / `TestPass123`. Paired client certs survived (`save_user_creds`
merges rather than replaces), but be careful: do not run `--creds` on this
machine expecting isolation. The user should change that password.
