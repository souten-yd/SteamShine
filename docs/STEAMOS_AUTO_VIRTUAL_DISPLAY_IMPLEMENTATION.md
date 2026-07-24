# SteamOS automatic virtual display implementation

- Status: Implementing — hardware validation required
- Last updated: 2026-07-24

## Current integration map

1. `src/main.cpp` initializes Sunshine services.
2. `src/nvhttp.cpp::launch()` parses Moonlight/GameStream launch parameters into `rtsp_stream::launch_session_t`; it includes width, height, FPS, bitrate-related protocol data and HDR intent.
3. `display_device::configure_display()` applies existing physical-display policy and `video::probe_encoders()` selects capture/encoder before `proc::proc.execute()` starts the requested application.
4. `rtsp_stream::launch_session_raise()` accepts the RTSP connection, then `stream::session::start()` creates video, audio, and input workers.
5. `stream::session::join()` stops workers, releases input/audio/video, reverts display configuration, and now requests virtual-session cleanup.

Capture implementations are `src/platform/linux/pipewire.cpp` (PipeWire/DMA-BUF where negotiated), `wayland.cpp`, `kmsgrab.cpp`, and `x11grab.cpp`. The SteamOS virtual route uses `wayland.cpp`/`wlgrab.cpp` and accepts Vulkan Video capture memory so DMA-BUF frames can be imported by `vulkan_encode.cpp` without CPU readback. VA-API and Vulkan Video are `vaapi.cpp` and `vulkan_encode.cpp`. Application prep/undo is owned by `process.cpp` and configured in `config.cpp`. Linux service packaging is `packaging/linux/*.service.in`; tests are GTest under `tests/`.

## Implemented boundary

`steamos_virtual_display_enabled=false` preserves normal Sunshine behavior. When enabled, `steamos_virtual_session` normalizes client dimensions to 640x480–7680x4320 and FPS to 30–240, creates an owned directory under `XDG_RUNTIME_DIR/steamshine`, starts a process group, and requires a real UNIX Wayland socket before continuing. It derives a Gamescope command from the installed binary's `--help` output; Gamescope 3.16 uses `--backend headless`, while older binaries are accepted only when they advertise a legacy `--headless` option. The command includes `--expose-wayland`, nested size/refresh, the `fit` policy when available, optional HDR, and an AMD Vulkan device preference.

The existing Wayland DMA-BUF backend now opens the owned socket by file descriptor instead of changing Sunshine's process-wide `XDG_RUNTIME_DIR`. After the backend has verified its Wayland capture interfaces, it marks the virtual session capture-ready. Existing application execution receives only the owned `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`, so prep/undo semantics remain under the existing process manager. Failure is returned as GameStream 503 before application launch. Cleanup kills only the owned process group and deletes only its owned directory. On SteamShine restart, an ownership marker plus an exact `XDG_RUNTIME_DIR` process-environment match is required before an orphan runtime or process is removed.

## Remaining hardware-gated work

The dedicated PipeWire-node provider has not been implemented; the current virtual path uses the existing Wayland DMA-BUF capture backend, which is the low-copy display source exposed by Gamescope. PipeWire node ownership/disappearance tests and monitorless Moonlight acceptance remain hardware-gated. GitHub Actions run 30069571926 exercised configure, build, targeted GTest, installer smoke, runtime linkage, packaging, and Artifact upload. On the SteamOS 3.8.16 RX 9070 XT host, the final Artifact installed in user space, initialized its packaged assets, started its user service, and passed the Vulkan H.264 encoder-open probe. A direct headless Gamescope probe also confirmed that this Gamescope version owns `gamescope-0`, so SteamShine clears inherited desktop display variables and waits for that private socket. It did not yet stream a live Moonlight frame. Use `./steamshine.sh hardware-test --interactive` on the target system.

## Configuration

The added keys are `steamos_virtual_display_enabled`, `steamos_virtual_display_mode`, `steamos_gamescope_path`, `steamos_runtime_directory`, GPU preference keys, startup/shutdown timeout keys, default display values, and `steamos_cleanup_orphan_sessions`. A blank GPU preference selects the AMD render node with the largest dedicated VRAM (requiring at least 1 GiB) and refuses the usual UMA iGPU path. PCI BDF, card node, and render-node selectors are resolved through sysfs. Because Gamescope advertises a vendor/device selector rather than a PCI-BDF selector, SteamShine rejects a launch if two AMD render nodes share the requested identifier. The active virtual session feeds its render node into existing VA-API and Vulkan Video device resolution; capture and encoder overrides must resolve to the same node.

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
2. Confirm Gamescope launch, Wayland DMA-BUF frames, Vulkan Video output, audio, keyboard, mouse, and gamepad.
3. Record capture-to-network timing, dropped frames, queue depth, GPU identity, and write volume.
4. Repeat disconnect and reconnect ten times.

Acceptance:

- Video, audio, and input all work.
- Capture and encoder use the RX 9070 XT owned render node.
- No CPU software encoder fallback occurs.
- Every disconnect removes the owned process group and runtime path within five seconds.
- Ten reconnect cycles pass without manual cleanup.

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
- Do not claim AV1, HDR, PipeWire-node ownership, or performance targets before live hardware validation.
