# SteamShine dual Web UI coexistence and validation plan

## 1. Purpose

SteamShine will restore the upstream Sunshine Web UI and introduce a SteamShine-specific Web UI in parallel. Both UIs must remain usable against one shared authentication, pairing, client, application, and configuration backend while migration proceeds in controlled stages.

This plan also defines mandatory Web build, packaging, HTTP, browser, authentication, pairing, coexistence, security, installation, and SteamOS hardware validation. A delivery Artifact is not accepted merely because the binary starts or `--version` succeeds.

## 2. Current problem statement

The SteamOS Artifact has rendered unresolved frontend template expressions such as:

```text
<%- header %>
{{ $t('welcome.greeting') }}
```

This indicates that the generated upstream frontend assets, template expansion, static asset root, or packaging layout is incorrect. In that state, credential setup, login, PIN pairing, and the Moonlight acceptance test cannot be trusted.

## 3. Non-negotiable requirements

- Restore the upstream Sunshine UI before depending on the new SteamShine UI.
- Ship both UIs concurrently during migration.
- Do not duplicate password, PIN, pairing, client-revocation, application, or configuration persistence logic.
- Both UIs use the same user credentials, pairing records, client list, application definitions, and configuration model.
- Keep upstream UI source in the fork even when future packaging options exclude it.
- A failure in either UI must not terminate or block an already-running stream.
- Do not add `sudo`, `pacman`, `steamos-readonly`, `/usr`, or `/etc` writes to the SteamOS host workflow.
- Do not add arbitrary command execution endpoints.
- Do not log credentials, session secrets, pairing PINs, CSRF tokens, or authentication cookies.
- Preserve rollback to the upstream UI without migrating or deleting shared state.

## 4. Target architecture

```text
Upstream Sunshine UI ─┐
                      ├─ HTTP routing and shared authentication boundary
SteamShine UI ────────┘
                              │
                              ▼
                    Shared application services
                    ├─ CredentialService
                    ├─ SessionService
                    ├─ PairingService
                    ├─ ClientService
                    ├─ ApplicationService
                    ├─ ConfigurationService
                    ├─ StatusSnapshotService
                    └─ DiagnosticService
                              │
                              ▼
                 Existing Sunshine/SteamShine core
                 ├─ NVHTTP / RTSP
                 ├─ capture / encode
                 ├─ audio / input
                 └─ SteamOS virtual session
```

The SteamShine frontend must not directly manipulate configuration files or pairing storage. It uses a stable facade under `/api/steamshine/v1/`, implemented through shared services that reuse existing Sunshine logic.

## 5. URL and coexistence strategy

### 5.1 Preferred initial routing

Preserve upstream compatibility first:

```text
https://host:47990/             -> upstream Sunshine UI
https://host:47990/steamshine/  -> SteamShine UI
```

This avoids breaking existing absolute asset paths and bookmarks while the new UI is experimental.

### 5.2 Later routing option

After both UIs pass coexistence and browser tests:

```text
https://host:47990/             -> SteamShine UI
https://host:47990/sunshine/    -> upstream Sunshine UI
```

The route switch must be configuration-driven and reversible without changing credentials, clients, applications, or stream state.

### 5.3 Configuration switches

```ini
steamshine_web_ui_enabled = true
steamshine_web_ui_default = false
upstream_web_ui_enabled = true
upstream_web_ui_visible = true
```

No stage may make both UIs unavailable.

## 6. Phased implementation

### Phase 0: Restore upstream Sunshine UI

1. Trace the authoritative upstream frontend build process from source and lockfiles.
2. Identify generated HTML, JavaScript, CSS, locale, icon, and manifest output paths.
3. Verify `SUNSHINE_ASSETS_DIR` and `SUNSHINE_ASSETS_DIR_DEF` against the installed Artifact layout.
4. Build the frontend in CI using the upstream-selected package manager and lockfile.
5. Package generated runtime assets, not raw templates alone.
6. Add package-time rejection when unresolved template markers or required bundles are missing.
7. Validate credential creation, login, PIN registration, logout, and re-login on SteamOS.

Phase 0 is complete only when the upstream UI works in a real browser from both `localhost` and another LAN device.

### Phase 1: Shared backend boundary

Introduce shared services without changing protocol behavior.

Minimum facade API:

```text
GET    /api/steamshine/v1/setup/status
POST   /api/steamshine/v1/setup/credentials
POST   /api/steamshine/v1/auth/login
POST   /api/steamshine/v1/auth/logout
GET    /api/steamshine/v1/status
GET    /api/steamshine/v1/session
POST   /api/steamshine/v1/pairing/pin
GET    /api/steamshine/v1/clients
DELETE /api/steamshine/v1/clients/{id}
GET    /api/steamshine/v1/logs/recent
```

Requirements:

- Reuse existing credential verification and pairing functions.
- Apply schema validation before calling core services.
- Return stable error codes without exposing secrets or internal file paths.
- Do not write pairing/configuration state from HTTP handlers directly.
- Add unit tests around the service boundary before frontend integration.

### Phase 2: Minimal SteamShine UI

Required pages:

```text
/steamshine/setup
/steamshine/login
/steamshine/dashboard
/steamshine/pairing
/steamshine/clients
/steamshine/logs
```

Required functions:

- Initial credential creation.
- Login and logout.
- Four-digit Moonlight PIN submission with client name.
- Pairing success/failure display.
- Paired-client list and revocation.
- Service, active-stream, Gamescope, virtual-display, GPU, encoder, and latest-error status.
- Link to the upstream Sunshine UI.

Use the ControlDeck visual language for design tokens, cards, navigation, forms, dialogs, status badges, responsive layout, and dark theme. Do not import ControlDeck-specific LLM, workflow, terminal, or server-job logic.

Use the approved SteamShine logo for setup, login, navigation, About, favicon, and application icons. Generate optimized transparent variants for the required sizes rather than serving the large source image for every request.

### Phase 3: Concurrent operation

- Both UIs are built and packaged by default.
- Both use the same credential and pairing records.
- Each UI contains a link to the other.
- Failure of one static asset tree must not prevent the other route from working.
- Cookie names, paths, SameSite behavior, CSRF tokens, and route guards must not conflict.
- Concurrent browser tabs must not overwrite each other's valid state.

### Phase 4: Move required management functions

Priority A:

- setup, login, logout
- PIN pairing
- client list and revoke
- status and logs

Priority B:

- application list/create/edit/delete
- video/audio/input basics
- virtual display `off/auto/force`
- game/capture/encoder GPU selection
- resolution and FPS
- service restart
- diagnose and hardware-test entry points

Priority C, after the first stable release:

- Steam Deck profile
- Y700 `3040x1904 @ 90 fps` profile
- 4K TV profile
- bitrate and codec controls
- latency, frame-pacing, drop, and reconnect telemetry
- report viewer and comparison tools

### Phase 5: Make SteamShine UI the default

Only after:

- all Priority A and B functions are complete,
- security review is complete,
- browser/E2E/coexistence tests pass,
- at least two releases have no critical Web UI regression,
- rollback has been tested on SteamOS.

The upstream UI remains enabled as a compatibility and recovery path.

### Phase 6: Evaluate hiding or excluding upstream assets

Do not remove upstream source. Packaging exclusion may be considered only for an experimental channel after SteamShine UI alone supports setup, recovery, pairing, application management, configuration, logs, and rollback.

## 7. Security requirements

- HTTPS remains mandatory for authenticated routes.
- Authentication cookies use `Secure`, `HttpOnly`, and an appropriate `SameSite` policy.
- Implement CSRF protection for every state-changing browser request.
- Validate `Origin`, `Host`, and request content type.
- Apply login and PIN rate limits.
- Expire server-side sessions on logout and credential change.
- Restrict initial setup to localhost or explicitly allowed LAN access.
- Never place credentials or PINs in URL query strings.
- Use bounded request bodies and timeouts.
- Escape all user-controlled display values.
- Apply a Content Security Policy compatible with both built frontends.
- Prevent path traversal and arbitrary static-file reads.
- Audit authentication, pairing success/failure, client revocation, configuration changes, and service actions without recording secrets.

## 8. Build and packaging requirements

Add explicit options:

```cmake
STEAMSHINE_BUILD_WEB_UI=ON
SUNSHINE_BUILD_UPSTREAM_WEB_UI=ON
```

Default during migration: both `ON`.

The delivery package must contain:

- generated upstream index and bundles,
- generated SteamShine index and bundles,
- locale resources,
- icons/logo/favicon,
- asset manifest with content hashes,
- no unresolved source-template-only entry page.

Package generation fails if:

- either enabled UI lacks an index,
- referenced JS/CSS assets are absent,
- locale assets are absent,
- unresolved `<%-` or known frontend interpolation markers remain in delivered entry pages,
- asset paths escape the package root.

## 9. Web validation strategy

Validation is layered. No single check is sufficient.

### 9.1 Static artifact validation

Run before packaging completes:

- indexes exist and are non-empty,
- JS/CSS bundles exist and are non-empty,
- asset manifest matches files,
- unresolved template markers are absent from generated entry pages,
- MIME extension expectations are valid,
- every local `src` and `href` reference resolves within the asset tree,
- both UI route trees are present when enabled.

### 9.2 HTTP smoke validation

Install the Artifact into a temporary HOME, start SteamShine on an isolated test port, and perform HTTP checks against the actual binary.

Required checks:

- TLS endpoint starts and becomes ready within a bounded timeout.
- Upstream welcome/login route returns HTTP 200.
- SteamShine setup/login route returns HTTP 200.
- HTML does not contain unresolved `<%- header %>` or translation expressions.
- Every referenced JS and CSS resource returns HTTP 200.
- JavaScript uses a JavaScript MIME type; CSS uses `text/css`.
- Unknown static paths return 404, not a sensitive file.
- Unauthenticated protected API calls return 401/403.
- Health/status endpoint does not leak secrets.
- Both routes remain reachable concurrently.

The smoke test must capture server stdout/stderr and terminate the process reliably on pass or failure.

### 9.3 Browser automation

Use Playwright or an equivalent real-browser framework. Test Chromium at minimum; Firefox should be added when practical.

Required upstream UI scenarios:

1. Open first-run welcome page.
2. Verify visible labels are rendered, not raw templates.
3. Create test credentials.
4. Log in.
5. Open PIN page.
6. Submit a mocked valid pairing PIN through the shared PairingService.
7. Verify success and client list update.
8. Log out and log back in.

Required SteamShine UI scenarios:

1. Open setup or login page.
2. Authenticate with the same credentials created through the upstream UI.
3. Verify dashboard status cards render.
4. Open pairing page.
5. Validate bad PIN format locally and server-side.
6. Submit a mocked valid PIN.
7. Verify the same client is visible in both UIs.
8. Revoke the client from one UI and verify disappearance in the other.
9. Open both UIs in separate tabs and confirm sessions remain valid.
10. Verify route refresh and deep links do not return 404.
11. Verify responsive layout at desktop, tablet, and Steam Deck-size viewports.
12. Verify no uncaught console errors and no failed static requests.

### 9.4 API and security tests

- valid/invalid login
- expired session
- logout invalidation
- CSRF missing/invalid/valid
- Origin mismatch
- PIN rate limit
- login rate limit
- malformed JSON
- oversized body
- unauthorized client revoke
- concurrent configuration update conflict
- path traversal attempts
- XSS payload display escaping
- cookie attributes
- CSP headers

### 9.5 Coexistence tests

- credentials created in either UI work in the other,
- pairing created in either UI appears in the other,
- client revoke propagates to both,
- configuration updates are visible in both,
- upstream UI failure does not break SteamShine UI,
- SteamShine UI failure does not break upstream UI,
- disabling one UI leaves the other operational,
- changing the default route is reversible without data migration,
- simultaneous requests do not corrupt configuration or client state.

### 9.6 SteamOS hardware Web acceptance

On the SteamOS 3.8.16 RX 9070 XT host, using the installed delivery Artifact:

1. Open the upstream UI from `https://localhost:47990/`.
2. Open it from another LAN device using the host IP.
3. Complete credential setup/login.
4. Open the SteamShine UI route in both locations.
5. Submit the Moonlight PIN in the SteamShine UI.
6. Confirm the client appears in both UIs.
7. Start `hardware-test --interactive`.
8. Start an actual Moonlight Desktop/game stream.
9. Confirm both UIs remain responsive while streaming.
10. Confirm UI polling does not measurably regress encode latency, frame pacing, drops, or SSD writes.
11. Disconnect, revoke/re-pair the client, and repeat one connection.
12. Save browser console logs, network failures, service logs, and screenshots in the hardware-test report.

UI performance acceptance:

- No synchronous encoder/capture calls from HTTP request threads.
- UI snapshot update normally limited to 1-2 Hz.
- No per-frame disk writes for Web telemetry.
- With both dashboards open, encode latency delta must remain within the existing acceptance threshold (`<= +0.3 ms` or `+3%`, whichever is larger), with no statistically significant frame-time regression and dropped-frame delta `<= 0.1` percentage points.

## 10. CI workflow design

### Fast Web checks

Run for frontend, Web API, asset, or Web-test changes:

- formatter/linter/type check,
- unit tests,
- static artifact validation,
- API tests,
- mocked browser tests where no full C++ binary is required.

### Full delivery validation

Run for ready-for-review, manual full validation, master, and release:

- clean frontend builds for both UIs,
- clean C++ build,
- package both generated asset trees,
- install Artifact into temporary HOME,
- launch actual packaged binary,
- HTTP smoke tests,
- browser E2E tests,
- ABI/package/installer checks,
- Artifact upload.

The exact binary and asset tree that pass validation must be the files uploaded in the delivery Artifact. Do not rebuild a different frontend during release publication.

## 11. Test evidence and reporting

Produce machine-readable reports:

```text
web-static-report.json
web-http-smoke-report.json
web-browser-e2e-report.json
web-security-report.json
web-coexistence-report.json
web-hardware-report.json
```

Each report records:

- commit SHA and Artifact SHA-256,
- UI build versions and asset manifest hashes,
- tested routes and HTTP status,
- failed resource URLs,
- browser name/version and viewport,
- console errors,
- authentication/pairing scenario results,
- coexistence results,
- start/end timestamps and durations.

Screenshots and traces are retained on failure. Hardware evidence is retained for successful acceptance as well.

## 12. Acceptance gates

### Gate A: Upstream UI restored

- Real browser renders correctly.
- Credential creation/login/PIN works.
- No unresolved template markers.
- Static, HTTP, and browser tests pass.

### Gate B: Minimal SteamShine UI usable

- Shared login and PIN work.
- Clients and status work.
- Security/API tests pass.
- Upstream fallback remains available.

### Gate C: Coexistence complete

- Shared state is consistent.
- Both UIs work simultaneously.
- Failure and disable scenarios pass.
- SteamOS hardware Web acceptance passes.

### Gate D: SteamShine default eligible

- Priority A and B features complete.
- Two stable releases without critical Web regressions.
- Security review and rollback drill complete.

## 13. Rollback

Rollback must be possible by configuration only:

```ini
steamshine_web_ui_enabled = false
steamshine_web_ui_default = false
upstream_web_ui_enabled = true
upstream_web_ui_visible = true
```

Credentials, clients, applications, and configuration are preserved. If an Artifact rollback is required, use the existing immutable version/rollback mechanism and verify both UI routes after rollback.

## 14. Required commit separation

```text
fix(web): restore upstream Sunshine frontend build and packaging
test(web): add upstream static and HTTP smoke validation
feat(api): add shared Web application services and facade
feat(web): add minimal SteamShine setup login and PIN UI
feat(web): serve upstream and SteamShine UIs concurrently
test(web): add browser security and coexistence suites
docs(web): document staged migration validation and rollback
```

Do not mix these commits with Vulkan, virtual-display lifecycle, hardware-test bug fixes, or CI acceleration refactors unless a dependency is unavoidable and documented.

## 15. Completion report

The implementation report must include:

1. Root cause of the unresolved upstream UI templates.
2. Authoritative upstream frontend build command and output layout.
3. Final installed asset trees and manifests for both UIs.
4. URL and route configuration.
5. Shared credential, session, pairing, client, and configuration architecture.
6. Security controls.
7. Static, HTTP, browser, API, security, and coexistence results.
8. SteamOS localhost and LAN browser results.
9. Actual Moonlight PIN and pairing result without exposing the PIN.
10. Streaming-time UI performance comparison.
11. Artifact name, SHA-256, CI run, and commit.
12. Rollback drill result.
13. Remaining functions not yet migrated.
14. Current migration stage and next gate.
