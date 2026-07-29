# SteamOS automatic virtual display implementation

The reproducible build, installation, transition matrix, evidence collection,
pass criteria, and failure-triage procedure are defined in
[`STEAMOS_TRANSITION_ACCEPTANCE_RUNBOOK.md`](STEAMOS_TRANSITION_ACCEPTANCE_RUNBOOK.md).
The runbook distinguishes attached stock Gamescope from a SteamShine-owned
private session and is the authoritative procedure for hardware acceptance.

The canonical repeated-launch route model and VRR-off scenario matrix are
documented in
[`STEAMOS_CONNECTION_ROUTE_REDESIGN.md`](STEAMOS_CONNECTION_ROUTE_REDESIGN.md).
Route policy is isolated in `select_session_route()`: automatic selection
prefers verified stock Gamescope, then a capturable physical Desktop, then a
compatible retained owned session, and finally a new owned session.
Source-specific preparation begins only after that common decision; owned
Gamescope option probing does not run for physical, attached, or retained
routes.

- Status: Implemented — final hardware acceptance in progress
- Last updated: 2026-07-29

## Current integration map

1. `src/main.cpp` initializes Sunshine services.
2. `src/nvhttp.cpp::launch()` parses Moonlight/GameStream launch parameters into `rtsp_stream::launch_session_t`; it includes width, height, FPS, bitrate-related protocol data and HDR intent.
3. `display_device::configure_display()` applies existing physical-display policy and `video::probe_encoders()` selects capture/encoder before `proc::proc.execute()` starts the requested application.
4. `rtsp_stream::launch_session_raise()` accepts the RTSP connection, then `stream::session::start()` creates video, audio, and input workers.
5. `stream::session::join()` stops connection workers, releases input/audio/video, reverts display configuration, and retains an owned virtual session for `/resume`; explicit cancel or service stop performs virtual-session cleanup.

Capture implementations are `src/platform/linux/pipewire.cpp` (PipeWire/DMA-BUF where negotiated), `wayland.cpp`, `kmsgrab.cpp`, and `x11grab.cpp`. The SteamOS Gamescope route uses `gamescopegrab.cpp` to consume one identity-verified PipeWire node. It queries DMA-BUF import formats through GBM/EGL on the session's verified DRM render node, not through the process's Desktop Wayland environment, and feeds those buffers to the Vulkan Video or VA-API encoder on the same node. Ordinary KWin and portal capture retain Desktop Wayland capability discovery. VA-API and Vulkan Video are `vaapi.cpp` and `vulkan_encode.cpp`. Application prep/undo is owned by `process.cpp` and configured in `config.cpp`. Linux service packaging is `packaging/linux/*.service.in`; tests are GTest under `tests/`.

## Implemented boundary

A systemd user service may start before Desktop Mode KWin. In that ordering,
the daemon initially has no `WAYLAND_DISPLAY`, and environment values imported
into the user manager later do not propagate into the existing process. Before
an idle Moonlight launch or resume, SteamShine therefore requires the live
`org.kde.KWin` D-Bus owner, reads the environment imported into the systemd
user manager, validates the ownership and type of its advertised Wayland
socket, and accepts only one unambiguous complete endpoint. It applies that
endpoint and re-probes KWin ScreenCast
before choosing physical Desktop or virtual Gamescope capture. When no trusted
KWin is live, stale Desktop capture capabilities are removed so Game Mode
cannot inherit a previous Desktop decision.

`steamos_virtual_display_enabled=false` preserves normal Sunshine behavior. The `steamos_virtual_display_mode` policy accepts `off`, `auto`, and `force`; invalid values are startup configuration errors. `force` never falls back to a physical display or desktop Wayland socket. `steamos_session_source=auto` first selects one uniquely verified current-user SteamOS Game Mode capture-output node, then falls back to a SteamShine-owned private Gamescope session. `existing_gamescope` fails closed unless that resident source is verified; `owned_private` skips discovery and requires the private session. Verification joins the PipeWire node to its client PID and UID, validates the live Gamescope executable and start time from `/proc`, and rejects ambiguity or a GPU mismatch. Stock SteamOS Game Mode is identified by the exact `gamescope-session.service` systemd cgroup component because its vendor launcher does not pass `--steam`; its resident Steam environment is accepted only from the unique current-user `steam` executable in the exact sibling `steam-launcher.service` component. Game subprocesses such as a `reaper` retained outside that service do not invalidate this endpoint, while another current-user `steam` executable still fails closed. Non-vendor launchers still require both a Steam command marker and Steam-related cgroup evidence. When an existing session is attached, SteamShine never sends it signals or removes its runtime. When an owned session is required, `steamos_virtual_session` normalizes client dimensions to 640x480–7680x4320 and FPS to 30–240, rejects any configured runtime base outside `XDG_RUNTIME_DIR`, creates an owned directory under `XDG_RUNTIME_DIR/steamshine`, starts a process group, and requires a real UNIX Wayland socket before continuing. It derives a Gamescope command from the installed binary's `--help` output; Gamescope 3.16 uses `--backend headless`, while older binaries are accepted only when they advertise a legacy `--headless` option. The command includes `--expose-wayland`, nested size/refresh, the `fit` policy when available, optional HDR, and an AMD Vulkan device preference. Owned sessions intentionally omit `--steam`: Steam-controlled focus rejects ordinary applications with no Steam AppID, producing a valid PipeWire node that never publishes an application frame. SteamShine's independent Steam-instance placement checks remain active.

The PipeWire capture backend discovers the Gamescope capture-output node from the host PipeWire registry. Node ownership is verified through its PipeWire Client identity: the client carries `application.process.id`, which must match the selected Gamescope process identity. The verified node ID, stable object serial, producer start time, and selected render node form one consumer descriptor. Stream configuration completes that verification before GPU capability discovery begins. SteamShine then opens the exact `/dev/dri/renderD*` node with `O_NOFOLLOW`, verifies that the descriptor still names a character device, creates GBM/EGL on it, and advertises only the importable formats and modifiers returned by that device. The host PipeWire descriptor is independently opened for each consumer and is closed on every pre-connect failure. This prevents selecting a desktop or another user's PipeWire node, prevents a stale Desktop `WAYLAND_DISPLAY` from affecting Game Mode capture, and keeps capture and encoding on the same approved GPU.

The resident user service treats the desktop system tray as optional. When SteamOS virtual-display support is enabled, it omits the tray because Qt's Wayland and X11 platform backends are tied to the current desktop compositor and can terminate their process when that compositor disappears. Normal tray behavior is unchanged with virtual-display support disabled. If another optional platform loop ends unexpectedly, only an explicit SteamShine shutdown request ends the main loop.

Once a virtual session is prepared, any missing or changed PipeWire identity or render node fails capture closed: the backend does not retry Desktop Wayland, KMS, X11, or another GPU. SteamShine records the Gamescope group-leader PID in its owner-only runtime and the hardware harness matches that PID's `/proc` environment to the marker-owned directory. Gamescope capture discovery accepts its current `Stream/Output/Video` PipeWire class and the compatible `Video/Source` spelling, then joins the node to a verified current-user Gamescope client. Gamescope may omit a render-node property; omission is accepted only after the configured game, capture, and encoder GPU resolve to the same node, while contradictory producer metadata is rejected. DMA-BUF negotiation also advertises a memory-buffer fallback from the same verified PipeWire stream when the selected GPU exposes no import modifiers; this preserves final video output without authorizing a different capture source or software encoder. Gamescope may briefly pause and resume the same stream while applying its private requested-size negotiation. SteamShine treats that transition as a capture and encoder reinitialization only while the original Gamescope PID and start-time identity remain current; it never falls back to another display source. After PipeWire format negotiation produces positive dimensions, the backend marks the virtual session capture-ready and increments an in-memory frame counter only while streaming. A Moonlight network disconnect changes `Streaming` to `Ready` and stops packet accounting. The default `steamos_keep_session_alive=true` retains an owned app, Gamescope process group, and runtime directory for `/resume`; setting it false stops only that owned session. On a replacement consumer, a source that previously produced frames has a two-second first-frame grace period. Expiry records the source as failed instead of emitting black duplicates indefinitely, allowing normal teardown to remove only the owned process group before a later launch creates a fresh source. Newly created sessions retain their longer Steam startup allowance. Attached Game Mode is detached but never terminated in either case. A terminal capture error records `Failed` without blocking the capture thread.

The Vulkan RGB-to-YUV conversion treats each PipeWire DMA-BUF as borrowed
storage. The compute submission signals an explicit fence, and `convert()`
waits for that fence before returning the image to the capture pool. Only then
may PipeWire recycle the producer buffer or may the next frame destroy the
imported Vulkan image and memory. The same completion point permits an
explicitly reset command buffer to be reused; elapsed-frame assumptions are
not a valid substitute because an idle or reconnecting producer can leave an
arbitrary interval between buffers. This synchronization was required after a
retained owned reconnect produced a `session::video` AMDGPU page fault,
graphics-ring timeout, full GPU reset, and consequent Gamescope device loss.

Application execution receives one generation-bound display endpoint only after SteamShine verifies the Gamescope PID/start time, environment-source PID/start time, dynamic Xwayland socket, Xauthority policy, and private Wayland sockets. The endpoint carries `XDG_RUNTIME_DIR`, optional `WAYLAND_DISPLAY`, `GAMESCOPE_WAYLAND_DISPLAY`, `DISPLAY`, optional `XAUTHORITY`, `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP`, host PipeWire/Pulse values, and a verified D-Bus address when available. Gamescope 3.16 does not create an Xauthority path for its auth-less private Xwayland, so an owned session creates an empty `0600` file inside its private runtime before Gamescope starts and passes that explicit path to both the bootstrap and later applications; it does not call `xhost` or change Gamescope's access policy. Stock SteamOS 3.8 Game Mode also omits `XAUTHORITY` and `WAYLAND_DISPLAY` from the resident Steam environment while publishing `XDG_SESSION_TYPE=x11`. Those omissions are accepted only for the exact vendor `gamescope-session.service` and `steam-launcher.service` pair after the Steam process identity and connectable current-user X11 socket are verified; application launches erase stale inherited Desktop values and reproduce the allow-listed resident session values. Non-vendor attached sessions still require a current-user regular authority file inside their runtime. SteamShine waits up to the configured startup deadline for the vendor Steam process and Xwayland socket to settle, covering a Moonlight request that arrives while Game Mode is still starting. The bootstrap writes only allow-listed values to an atomic `0600` JSON report and performs no frame or GPU work. Attached Game Mode instead reuses the unique verified resident Steam environment. Physical Desktop launches receive no session endpoint and retain every inherited host display variable. The dashboard reports endpoint origin, verification, display names, producer/environment identities, and generation without exposing Xauthority or D-Bus values.

The owned bootstrap also receives the daemon PID and Linux start time that created it. It rechecks that exact same-UID identity every 250 milliseconds and exits when the daemon disappears or the PID is reused. Gamescope's reaper then closes Gamescope, Xwayland, and their inherited descriptors even after an unexpected daemon crash. The force-mode hardware helper independently verifies its private owner marker, Gamescope PID, process group, session ID, command, and start time before reclaiming a test group; it also closes only KDE surfaces whose environment names that private runtime.

For the packaged commandless Desktop surface only, the canonical `plasmawindowed org.kde.plasma.folder` command is executed with `QT_QPA_PLATFORM=xcb`. Gamescope currently logs `NO CURSOR IMPL XDG` for a native XDG surface, while its XWayland path composes the cursor. This command-local selection leaves physical Desktop, attached Game Mode, Steam, and custom virtual desktop commands unchanged. Owned Desktop and Game Mode capture bind directly to a verified Gamescope PipeWire node; automatic physical capture prefers KWin ScreenCast, so neither path requires a Portal screen-sharing chooser.

Gamescope input uses the EIS socket derived from the verified `GAMESCOPE_WAYLAND_DISPLAY`. SteamShine checks containment, socket type, and UID, connects once, and compares the kernel-provided `SO_PEERCRED` PID with the verified Gamescope PID/start-time identity. That exact connected descriptor is transferred to libei, so verification and input use one connection without a probe disconnect or a path-replacement window. The resulting path, device, inode, and producer identity are cached only for that active session. This remains available when a file-capability Gamescope process protects `/proc/<pid>/fd` from same-user inspection, without weakening the fail-closed source binding.

If a verified Steam singleton is outside an owned private Gamescope session, Steam-referencing application, prep, and detached commands are rejected rather than creating a second instance; the dashboard asks the operator to migrate Steam explicitly and never stops it automatically. Failure is returned as GameStream 503 before application launch. Explicit `/cancel` or service stop kills only the owned process group and deletes only its owned directory. On SteamShine restart, the configured orphan base must first be inside the current `XDG_RUNTIME_DIR`; then an ownership marker plus an exact `XDG_RUNTIME_DIR` process-environment match is required before an orphan runtime or process is removed.

## Remaining hardware-gated work

The PipeWire Gamescope-node provider and client-PID ownership verification are implemented. On the SteamOS 3.8.16 RX 9070 XT host, the `4fadb824` local build has verified post-reboot monitorless Gamescope launch, the host PipeWire socket, Gamescope `Video/Source` discovery, matching producer PID, PipeWire streaming, DMA-BUF capture on `/dev/dri/renderD128`, Vulkan encoding, positive captured-frame/packet/byte/IDR counters, Moonlight video, and the private 1920x1080 Xwayland endpoint. A later physical-Desktop pass confirmed that the display helper can refresh Plasma's endpoint from the systemd user manager, switch `DP-1` to 1920x1080, and start KWin PipeWire capture even though the resident service predated Plasma. The Desktop-to-Game-Mode transition retained the same server PID with no systemd restart after the optional tray was removed. Subsequent Moonlight launches exposed four stock Game Mode topology differences: `/usr/lib/steamos/gamescope-session` omits `--steam`, Steam runs in the sibling `steam-launcher.service` rather than beneath Gamescope, its auth-less Xwayland supplies no `XAUTHORITY`, and the resident service retains the vanished Desktop `WAYLAND_DISPLAY=wayland-0`. Exact recognition of both vendor unit components and the vendor-only auth-less endpoint policy are implemented. Direct Gamescope PipeWire capture now obtains DMA-BUF capabilities from the verified render node, so the stale Desktop variable is outside the capture trust path. Similarly named units, mixed Steam placement, missing authority files in non-vendor sessions, changed PipeWire identities, and mismatched GPUs remain rejected. A hardware pass verified attached-session video and input, preservation of the non-owned stock shell at teardown, and a subsequent reattach to the same Gamescope PID and PipeWire source. Stock Game Mode producer starvation on a static menu remains a performance gate. Steam Big Picture display also remains a hardware gate. The current acceptance evidence is stored under `~/.local/state/steamshine/pipewire-runtime-diagnosis/`, `~/.local/state/steamshine/session-diagnostics/`, and the user journal.

Earlier local acceptance confirmed audio, touch/mouse control, retained-session behavior across a transient disconnect, and ten cable-disconnect/reconnect cycles. The latest endpoint build exposed the capability-protected EIS ownership check before input could be accepted, so mouse, touch, keyboard, and gamepad must all be repeated after the `SO_PEERCRED` change. The operator intentionally removed the retained session after the final earlier cycle; that is not evidence for explicit-stop cleanup. Post-reboot monitorless capture displayed the packaged folder-view surface, but Steam Big Picture did not become visible even though the Steam client started on the private 1920x1080 endpoint; Big Picture presentation remains failed, not accepted. At the default `minimum_fps_target=0`, duplicate output uses half the requested rate. Hardware comparison found a generic latency regression after full-rate deadline pacing was introduced: earlier physical KWin sessions reached capture-to-network in roughly 2.7--3.2 ms, while the paced owned-Gamescope session averaged 17.35 ms and held unique frames about 14.13 ms before encode. The same slowdown was visible in Desktop Mode, so unique frames now bypass the duplicate clock, capture handoffs retain only the latest pending frame, and immediate unique/IDR output re-anchors the next duplicate deadline. A retained owned-Gamescope reconnect then exposed a second independently phased 60 Hz wait in the generic PipeWire capture loop: despite producer callbacks at 60 FPS and queue depths of one, frames reached capture about 12.7 ms after callback. PipeWire now relies on its negotiated producer maximum and dequeues callback arrivals directly instead of sleeping on another client-rate phase. Audio retains its independent RTP clock, while input remains immediately injected instead of waiting on an A/V deadline; the first resulting latest video frame provides synchronized visual feedback without adding control latency. Session diagnostics report all deliberate replacements, duplicate output, IDR activity, and per-stage latency. Attached stock Game Mode is not owned by SteamShine, so application teardown preserves its resident Steam shell: a configured `steam://close/bigpicture` undo is skipped for that origin. Closing Big Picture there terminates the stock Gamescope session, starts a Game Mode-to-Desktop transition, and makes an immediate Moonlight reconnect race the disappearing source. Owned private and physical Desktop sessions retain their configured undo behavior. Hardware acceptance confirmed that the skip preserves the same stock Gamescope PID and PipeWire source across a later Moonlight launch. The remaining hardware-gated acceptance includes renewed physical KWin and owned/attached Gamescope latency passes, owned-session explicit-stop cleanup, and repeating the accepted scenario with the latest CI Artifact. A Game Mode session needs a separate acceptance pass: its resident Steam client can receive a `steam://open/bigpicture` request instead of a Steam client launched inside the owned virtual Gamescope session. The Desktop Mode Big Picture result must therefore not be used as proof of the Game Mode case. GitHub Actions validates format, ShellCheck, configure, build, lifecycle GTests, installer smoke, runtime linkage, ABI ceiling, packaging, checksum, and Artifact upload; it cannot establish live GPU or input acceptance. Use `./steamshine.sh hardware-test --interactive` on the target system.

### Force-mode hardware harness

`scripts/test-steamos-force-hardware.sh` is an explicit, temporary test
harness. It requires `STEAMSHINE_FORCE_HARDWARE_TEST=1`, copies rather than
modifies the configured Sunshine file, appends force-mode settings only to that
copy, and starts the binary with the copy without changing the installed user
service unit. The EXIT/INT/TERM trap terminates the temporary process, removes
only the `mktemp`-created runtime directory, and restores the original user
service when it was active before the test. Reports are written beneath
`~/.local/state/steamshine/force-mode-report` by default and redact common
credential keys. The operator must record actual Moonlight video, audio, and
input outcomes separately; pressing Enter is intentionally not treated as
streaming acceptance. Its temporary runtime uses a short `ss-fh.XXXXXX` name
so Qt/KIO can append worker socket names without exceeding Linux's UNIX-domain
socket path limit; this keeps the harness representative of the shorter
production runtime.

### GPU selector diagnosis

Run `scripts/diagnose-steamos-gpu.sh` before changing a SteamOS GPU selector.
It records the live DRM render-to-card mapping, PCI BDF, AMD driver and VRAM,
current-user and systemd-user-service device access, active service settings,
configured selectors, Vulkan summary, and recent non-sensitive GPU failures.
GPU selectors trim surrounding configuration whitespace and accept canonical
`/dev/dri/renderD*` paths, bare `renderD*` names, canonical `card*` paths or
names (resolved through their shared sysfs PCI device), and PCI BDFs. A blank
selector remains automatic selection and cannot be mistaken for a whitespace
only explicit selector.

### Web configuration

Both Web interfaces expose the persisted `off`, `auto`, and `force` policy,
the `auto`/`existing_gamescope`/`owned_private` source policy, and owned-session
retention. The dashboard reports the active origin, ownership flags, verified
source metadata, and Steam singleton location.
The Sunshine configuration UI has a **SteamOS Virtual Display** tab. The
SteamShine dashboard's **Display** page uses its authenticated, CSRF-protected
facade endpoint and validates the same canonical values before writing the
next-restart policy. Saving does not alter a live session; the interface makes
the restart requirement explicit. The current dashboard status continues to
show the running policy, owned private socket, Gamescope PID, and capture and
encoder counters so a persisted-but-not-yet-restarted policy is not mistaken
for a live one.

## Configuration

The added keys are `steamos_virtual_display_enabled`, `steamos_virtual_display_mode`, `steamos_session_source`, `steamos_keep_session_alive`, `steamos_existing_gamescope_pid`, `steamos_gamescope_path`, `steamos_runtime_directory`, `steamos_pipewire_runtime`, `steamos_pipewire_remote`, `steamos_pipewire_node_timeout_ms`, GPU preference keys, startup/shutdown timeout keys, default display values, and `steamos_cleanup_orphan_sessions`. `steamos_pipewire_runtime` defaults to the original login `XDG_RUNTIME_DIR`; `steamos_pipewire_remote` defaults to inherited `PIPEWIRE_REMOTE` or `pipewire-0`; `steamos_pipewire_node_timeout_ms` defaults to 10000. Both are passed explicitly to Gamescope and game children while their `XDG_RUNTIME_DIR` points to the private Wayland runtime. A configured PipeWire runtime must remain under the login runtime and the remote must be a socket name, so neither setting can redirect capture to an unrelated endpoint. A blank GPU preference selects the AMD render node with the largest dedicated VRAM (requiring at least 1 GiB) and refuses the usual UMA iGPU path. PCI BDF, card node, and render-node selectors are resolved through sysfs. Because Gamescope advertises a vendor/device selector rather than a PCI-BDF selector, SteamShine rejects a launch if two AMD render nodes share the requested identifier. The active virtual session feeds its render node into existing VA-API and Vulkan Video device resolution; capture and encoder overrides must resolve to the same node. The SteamShine dashboard exposes the current policy, lifecycle state, origin, ownership, verified source, Steam location, Gamescope PID, render node, and capture/encode counters without exposing credentials or session tokens. Hardware diagnostics on stock Gamescope 3.16.23.4 confirmed that a slow attached Game Mode stream received only 10 PipeWire buffers in 50.975 seconds while replacing none in the consumer; 1,384 of 1,393 encoded frames were duplicates. Gamescope's producer suppresses publication while the focus and override surface commit IDs are unchanged, even when an included Steam overlay or cursor changes. Consumer-side pacing or queue changes cannot repair this producer shortage; acceptance must distinguish an overlay-only Game Mode menu from a continuously committing game surface before selecting a compatibility fallback.

## Target client profiles

### Lenovo Legion Y700

The primary handheld target is 3040x1904 at 90 FPS. The target frame budget is 11.11 ms. Initial acceptance uses SDR. H.264 remains the bring-up codec; HEVC becomes the preferred production codec after live H.264 capture and encode are stable.

Recommended production target:

```yaml
y700_native_90:
  width: 3040
  height: 1904
  fps: 90
  codec: hevc
  bitrate_mbps: 60
  bitrate_min_mbps: 35
  bitrate_max_mbps: 75
  hdr: false
  frame_pool: auto
  max_queue_depth: 2
  drop_old_frames: true
  latency_priority: true
```

Fallback order for latency-priority mode:

1. 3040x1904 at 90 FPS.
2. 2560x1600 at 90 FPS.
3. 1920x1200 at 90 FPS.
4. Reduce FPS only after the configured resolution fallbacks are exhausted.

### 4K television

The television target is 3840x2160 at 59.94 or 60 FPS. HEVC is preferred; HEVC Main10 and HDR are later phases. H.264 remains a compatibility fallback.

Recommended production target:

```yaml
living_room_4k:
  width: 3840
  height: 2160
  fps: 59.94
  codec: hevc
  bitrate_mbps: 80
  bitrate_min_mbps: 50
  bitrate_max_mbps: 100
  hdr: auto
  max_queue_depth: 2
```

The implementation must preserve distinct 59.94 and 60.00 modes instead of rounding both to an integer refresh rate.

## Change ownership and upstream boundary

### Sunshine-core changes

The following work changes the capture/encode pipeline and therefore belongs in Sunshine-derived C++ code, preferably behind generic interfaces rather than SteamOS-only branches:

- DMA-BUF import cache and object lifetime management.
- Per-stage monotonic latency instrumentation.
- Detection and removal of unintended CPU/GPU blocking waits.
- Dynamic Vulkan hardware-frame pool sizing.
- Bounded queue depth and latest-frame preference.
- Encoder and capture watchdog hooks.
- `VK_ERROR_DEVICE_LOST` and encoder-recreation handling.
- Optional timeline semaphore synchronization.
- Optional compute/encode overlap when queue topology proves beneficial.
- HEVC Vulkan probe and later AV1 Vulkan support.

### SteamShine-specific changes

The following work remains in SteamShine management and SteamOS integration layers:

- Y700 and 4K television profiles.
- Gamescope resolution/refresh launch policy.
- Client-specific saved settings.
- User-service startup and bounded restart policy.
- Hardware-test orchestration and report collection.
- Same-GPU enforcement and SteamOS compatibility gates.
- Recovery escalation policy and diagnostic bundle creation.
- User-space-only installation, rollback, and uninstall.

### Protocol-facing changes

The following work requires codec capability negotiation and must remain compatible with Moonlight/GameStream behavior:

- HEVC and later AV1 codec selection.
- Main10/HDR signaling.
- Client-specific resolution and refresh selection.
- Runtime bitrate adaptation without unnecessary encoder recreation.
- Keyframe request rate limiting while allowing immediate IDR for a new client.

## Performance and stability plan

### Phase P0 — establish a live baseline

Goal: complete one real displayless Moonlight session before speculative optimization.

Tasks:

1. Connect Moonlight with H.264 SDR at 1920x1080 60 FPS.
2. Confirm Gamescope launch, verified PipeWire DMA-BUF frames, Vulkan Video output, audio, keyboard, mouse, and gamepad.
3. Record capture-to-network timing, dropped frames, queue depth, GPU identity, and write volume.
4. Repeat disconnect and reconnect ten times.

Acceptance:

- Video, audio, and touch/mouse control work; keyboard and gamepad remain explicit acceptance checks.
- Capture and encoder use the RX 9070 XT owned render node.
- No CPU software encoder fallback occurs.
- Every disconnect retains the owned process group and runtime path for `/resume`.
- After the final cycle, explicit cancel or service stop removes the owned process group and runtime path within five seconds.
- Ten reconnect cycles have passed without a replacement Gamescope session; repeat the check from the latest CI Artifact before release.

Rollback condition:

- If the optimized path cannot produce a live frame, retain the last successful Artifact and keep the PR Draft.

### Phase P1 — observability before optimization

Goal: identify actual bottlenecks and prevent unsupported performance claims.

Add monotonic timestamps and counters for:

- capture frame ready;
- DMA-BUF import start and completion;
- RGB-to-YUV compute submit and completion;
- encoder submit and bitstream ready;
- packet enqueue;
- frame drop reason;
- current and maximum queue depth;
- Vulkan object creation count;
- CPU waits, fence waits, `vkQueueWaitIdle`, and `vkDeviceWaitIdle` calls.

Report average, median, p95, p99, maximum, and sample count. Metrics must be disabled or sampled at low frequency in normal production mode to avoid excessive SSD writes.

Acceptance:

- A hardware-test report identifies each stage's latency.
- Normal streaming contains no `vkQueueWaitIdle` or `vkDeviceWaitIdle` calls.
- Metrics output remains bounded and does not generate per-frame persistent logs.

Expected benefit: no direct speed increase, but highest development value because all later gains become measurable.

### Phase P2 — DMA-BUF import cache

Goal: avoid recreating Vulkan resources when Gamescope reuses buffers.

Cache key must include:

- DRM device identity;
- DMA-BUF inode or stable buffer identity;
- width and height;
- DRM fourcc;
- modifier;
- plane count, offsets, and pitches.

Cached objects include `VkImage`, `VkDeviceMemory`, and `VkImageView`. File descriptors must not be retained beyond their ownership contract; duplicate only when required. Entries are retired only after associated GPU work is complete. The cache must be bounded and cleared on format change, resolution change, device loss, session stop, or encoder recreation.

Acceptance:

- Reused Gamescope buffers do not recreate Vulkan image/memory/view objects.
- No stale FD, image, or memory leak is observed during a 60-minute stream and ten reconnects.
- Cross-GPU buffers remain rejected.

Estimated benefit:

- approximately 0.2–2 ms per frame when repeated import currently occurs;
- lower driver overhead and fewer long-session allocation failures;
- strongest benefit at 3040x1904 90 FPS and 4K60.

### Phase P3 — queue depth and latest-frame policy

Goal: avoid accumulated latency when encode throughput temporarily falls behind capture.

Rules:

- normal target queue depth: 1;
- maximum queued frames: 2;
- when a third unencoded frame arrives, drop the oldest eligible frame and keep the newest;
- never drop a frame already submitted to GPU work;
- preserve keyframe and reference-frame correctness;
- expose dropped-frame reason and count.

Dynamic hardware-frame pool starting points:

- 60 FPS: 4 frames;
- 90 FPS: 5 frames;
- 120 FPS: 5–6 frames;
- higher rates: measured and capped at 8 frames.

Pool size must not be used as a substitute for unbounded queueing.

Acceptance:

- queue depth stays at 1–2 during steady-state Y700 and 4K tests;
- overload causes bounded frame drops rather than growing end-to-end delay;
- frame pool changes do not increase p99 latency.

Estimated benefit:

- 0–1 ms average when the current pool is already adequate;
- prevention of tens or hundreds of milliseconds of backlog during overload;
- major improvement to perceived responsiveness.

### Phase P4 — remove blocking synchronization

Goal: keep capture, conversion, encode, and network workers asynchronous.

Tasks:

- audit every Vulkan wait and FFmpeg frame acquisition path;
- remove normal-path queue/device idle waits;
- use nonblocking completion checks or bounded fence waits only when reuse requires them;
- ensure no CPU readback is introduced;
- record the wait source whenever a wait exceeds one millisecond.

Acceptance:

- zero queue/device idle calls during normal streaming;
- no synchronous CPU readback;
- Y700 host-side p99 capture-to-packet time remains within the 11.11 ms frame budget target where hardware permits;
- no regression in cleanup correctness.

Estimated benefit:

- approximately 0.5–5 ms average in a wait-bound implementation;
- approximately 5–30 ms reduction in p99 spikes when hidden synchronization exists.

### Phase P5 — staged watchdog and recovery

Goal: recover from component stalls without requiring a host reboot.

Track:

- last captured frame time;
- last encoded packet time;
- last network packet enqueue time;
- Gamescope process and socket state;
- active Moonlight session heartbeat.

Recovery order:

1. Capture reattach, maximum two attempts.
2. Encoder recreation, maximum two attempts.
3. Owned virtual-session recreation, maximum one attempt.
4. Exit SteamShine with failure and let the existing bounded systemd restart policy act.

Use backoff of approximately 1, 2, and 5 seconds. Do not loop indefinitely. Every escalation creates a bounded diagnostic event without per-frame log spam.

Acceptance:

- simulated capture stall recovers without restarting the entire service when possible;
- simulated encoder stall recreates only the encoder first;
- repeated unrecoverable failure stops after bounded attempts;
- unrelated desktop or Gamescope processes are never terminated.

Estimated benefit:

- no normal-path latency gain;
- recovery reduced from manual intervention or minutes to roughly 0.5–5 seconds for recoverable failures.

### Phase P6 — Vulkan device-loss recovery

Goal: safely recover from GPU reset or `VK_ERROR_DEVICE_LOST`.

Rules:

- never reuse a device or context after device loss;
- detach capture and destroy encoder/frame/pipeline resources;
- stop the owned Gamescope group;
- wait for the configured backoff;
- rediscover the same approved AMD render node;
- recreate the virtual session and encoder;
- stop safely if the node is unavailable or repeatedly fails.

Acceptance:

- injected device-loss handling leaves no live Vulkan objects or owned processes;
- no fallback to another GPU or CPU encoder occurs;
- a recoverable device reset can return to Idle or accept a new session without rebooting SteamOS.

Estimated benefit: no normal-path speed gain, but very high long-running stability value.

### Phase P7 — HEVC production path

Goal: make HEVC the preferred high-resolution codec after H.264 is proven stable.

Tasks:

1. Add selected-device `hevc_vulkan` probe mirroring the H.264 probe.
2. Validate 8-bit NV12 at 3040x1904 90 FPS and 3840x2160 60 FPS.
3. Confirm Moonlight capability negotiation and fallback to H.264.
4. Confirm IDR, VPS/SPS/PPS, packetization, reconnect, and bitrate changes.
5. Add Main10/P010 only after 8-bit HEVC passes.

Acceptance:

- Y700: 3040x1904 90 FPS HEVC SDR with stable frame pacing;
- television: 3840x2160 59.94/60 FPS HEVC SDR;
- fallback to H.264 is explicit and logged when HEVC is unavailable;
- no encoder recreation for a bitrate-only update unless FFmpeg requires it and the interruption is measured.

### Phase P8 — network adaptation and IDR control

Goal: reduce latency spikes on Wi-Fi, Tailscale, and variable networks.

Inputs:

- RTT;
- packet loss;
- retransmission or NACK rate;
- send queue depth;
- encode queue depth;
- client decoder latency when available.

Policy:

- reduce bitrate by approximately 10–15% after sustained congestion;
- increase by approximately 5% after 5–10 seconds of stability;
- apply hysteresis and minimum dwell time;
- do not rebuild the virtual display for bitrate-only changes;
- rate-limit repeated IDR requests to approximately 500–1000 ms, while allowing immediate IDR for a newly connected client.

Acceptance:

- no bitrate oscillation during a stable LAN test;
- induced packet loss reduces bitrate before send queues grow without bound;
- repeated IDR requests do not create continuous bitrate spikes.

Estimated benefit:

- small on stable wired LAN;
- potentially 10–100 ms or greater reduction in latency spikes on unstable Wi-Fi/WAN paths.

### Phase P9 — synchronization and queue-topology optimization

Goal: add more complex Vulkan scheduling only when P1 metrics prove it is needed.

Candidate work:

- timeline semaphores for per-frame completion tracking;
- separate compute and encode queues within the same queue family when available;
- overlap frame N encode with frame N+1 conversion;
- retain a single-queue path as the safe default.

Do not enable cross-family ownership transfers unless benchmarks show a clear improvement.

Acceptance:

- feature is capability-gated and can be disabled;
- p95/p99 improves without increasing device loss, hangs, or cleanup time;
- single-queue fallback remains tested.

Estimated benefit:

- timeline semaphore: usually 0–1 ms average, larger stability benefit at high FPS;
- compute/encode overlap: approximately 0.2–2 ms where queue topology supports true overlap.

### Phase P10 — AV1 and HDR

AV1 is not part of the initial acceptance path. Add it only after H.264 and HEVC live streaming, reconnect, and recovery are stable.

Tasks:

- detect `av1_vulkan` and Vulkan AV1 encode profiles;
- validate selected-device AV1 codec context and actual encoded keyframe;
- add Moonlight AV1 capability negotiation and fallback;
- test NV12 SDR first;
- test P010/HDR only after SDR AV1 is stable.

AV1 must not delay the initial release for Y700 or 4K television usage.

## Client-profile behavior

Profiles must be keyed by stable client identity, not only display name. A profile may specify:

- resolution and refresh;
- codec preference order;
- bitrate minimum, target, and maximum;
- HDR policy;
- latency-priority or quality-priority behavior;
- resolution fallback order;
- frame-drop policy.

The Web UI may expose profile editing, but invalid combinations must be rejected server-side. Client-provided values remain untrusted and must pass the existing dimension/FPS normalization and codec capability checks.

## Hardware acceptance matrix

| Scenario | Codec | Required result |
| --- | --- | --- |
| 1920x1080 60 FPS SDR | H.264 | Bring-up baseline; video/audio/input and ten reconnects |
| 3040x1904 60 FPS SDR | H.264 | Intermediate Y700 validation |
| 3040x1904 90 FPS SDR | H.264 | High-resolution pipeline stress baseline |
| 3040x1904 90 FPS SDR | HEVC | Y700 production target |
| 3840x2160 59.94 FPS SDR | HEVC | Television production target |
| 3840x2160 60 FPS SDR | HEVC | Television alternate refresh target |
| 3840x2160 60 FPS HDR | HEVC Main10 | Later HDR target |
| 3040x1904 90 FPS SDR | AV1 | Later optional target |

For every required scenario collect:

- capture, conversion, encode, and packet latency average/p95/p99/max;
- dropped frames and reason;
- queue depth average/max;
- encoder and render-node identity;
- CPU and GPU utilization;
- network bitrate, RTT, loss, and send queue;
- SteamShine persistent write volume;
- cleanup duration;
- reconnect result.

## Performance acceptance targets

These are engineering targets, not claims until measured on the RX 9070 XT host.

### Y700 3040x1904 at 90 FPS

- frame budget: 11.11 ms;
- host capture-to-packet target: 8 ms average or better;
- p99 target: at or below approximately one frame budget where practical;
- normal queue depth: 1;
- maximum queue depth: 2;
- long-session dropped-frame target: below 0.1%, excluding deliberate latest-frame drops during induced overload;
- no normal-path CPU readback or queue/device idle waits.

### 4K television at 59.94/60 FPS

- frame budget: approximately 16.68/16.67 ms;
- stable frame pacing without periodic 59.94/60 mismatch judder;
- normal queue depth: 1–2;
- no sustained bitrate or packet queue growth on the validated LAN profile;
- HEVC SDR acceptance before Main10/HDR.

## Test and CI requirements

Unit and integration tests must cover:

- DMA-BUF cache hit, miss, eviction, and session teardown;
- mismatched device identity rejection;
- queue-depth limit and oldest-frame drop policy;
- dynamic frame-pool calculation;
- watchdog escalation and retry limits;
- device-loss cleanup state machine;
- client-profile validation;
- HEVC probe success/failure and H.264 fallback;
- IDR rate limiting;
- bitrate controller hysteresis;
- all existing SteamOS virtual-session lifecycle cases.

GitHub Actions cannot claim live GPU performance. CI verifies logic, build, ABI, packaging, installer behavior, and simulated lifecycle. Real acceptance remains a SteamOS hardware-test requirement.

## Implementation order

1. Complete H.264 1080p60 live Moonlight acceptance.
2. Add P1 instrumentation and establish the baseline report.
3. Implement DMA-BUF cache.
4. Implement bounded queue depth, latest-frame policy, and dynamic frame pool.
5. Remove measured blocking waits.
6. Validate H.264 at 3040x1904 60 and 90 FPS.
7. Implement staged watchdog and device-loss recovery.
8. Complete HEVC 8-bit at Y700 native 90 FPS.
9. Complete HEVC 4K 59.94/60 FPS.
10. Add adaptive bitrate and IDR control.
11. Consider timeline semaphores and queue overlap only when metrics justify them.
12. Add Main10/HDR.
13. Add AV1 SDR, then AV1 HDR if still required.

## Non-goals and safeguards

- Do not introduce CPU software-encoder fallback for owned SteamOS virtual sessions.
- Do not silently choose another GPU.
- Do not add per-frame disk logging.
- Do not run SteamShine as root.
- Do not use `pacman`, `sudo`, `steamos-readonly`, or write `/usr` or `/etc` in the normal install or test path.
- Do not merge complex scheduling optimizations without before/after hardware evidence.
- Do not claim AV1, HDR, or performance targets before their corresponding live hardware validation.
