# Codex Goal Mode — Moonlight negotiation, HDR, codec, and quality

Use this file as the execution contract for the next implementation PR.

## Mission

Extend the current SteamShine implementation so Moonlight/Artemis requests for safe arbitrary resolution, aspect ratio, FPS, bitrate, codec capability, and HDR intent are negotiated correctly. Then improve frame pacing, communication quality, bitrate efficiency, AV1/HEVC quality, and processing quality without duplicating the inherited Sunshine pipeline.

SteamShine is the primary server and Web UI. The upstream Sunshine UI and compatible inherited paths remain the backup and recovery route.

## Repository and starting point

```text
Repository: souten-yd/SteamShine
Base branch: master
Planning source of truth:
  docs/PROJECT_ROADMAP.md
  docs/STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md
Status source of truth:
  docs/IMPLEMENTATION_STATUS.md
```

Before work:

```bash
git status
git fetch --all --prune
git switch master
git pull --ff-only
git log --oneline -20
gh pr list --state open
gh run list --repo souten-yd/SteamShine --limit 30
```

Create one draft umbrella PR:

```text
Branch: feat/moonlight-negotiation-hdr-quality
Title: feat(stream): negotiate Moonlight display HDR and quality
```

Do not build on a stale PR branch. Do not mix unrelated UI redesign, GPU-control, terminal, or local-presenter work into this PR.

## Global rules

1. Search and trace existing code before adding a file, class, manager, queue, config key, endpoint, or persistence store.
2. Extend `launch_session_t`, display-device, SteamOS virtual-session, video, stream, and Web status paths rather than creating a parallel server or encoder stack.
3. Keep H.264, HEVC, AV1, HDR, capture, audio, input, pairing, applications, and config owned by existing core implementations.
4. Preserve DMA-BUF and same-GPU Vulkan Video on the SteamOS AMD path.
5. Never start a second Steam instance automatically.
6. Never kill an attached existing Game Mode Gamescope.
7. Never silently change aspect ratio, HDR state, codec, or source without recording the fallback reason.
8. No unbounded queue, per-frame SSD write, per-frame external CLI, or normal-path `vkQueueWaitIdle`/`vkDeviceWaitIdle`.
9. Keep the backup Sunshine UI operational against the shared backend.
10. Do not merge with red CI or without a matching delivery Artifact and required hardware evidence.

## Goal G0 — audit and baseline

Tasks:

- Read the three canonical documents above.
- Trace request parsing from NVHTTP through RTSP launch, display preparation, application launch, capture, encoder, and packet sender.
- Record the exact current fields and units for width, height, FPS/refresh, bitrate, codec capability, HDR, and client identity.
- Identify every existing codec probe, rate-control setting, HDR setting, resolution/refresh setting, mode remapping path, queue metric, and Web status field.
- Run the current tests and preserve a 1080p60 SDR baseline.
- Record current master SHA, latest successful CI run, Artifact SHA-256, and rollback Artifact.

Required output in PR description:

```text
Existing code reused
Existing gaps
Files expected to change
Files explicitly not replaced
Baseline measurements
Rollback reference
```

Exit:

- no code added before the trace is complete;
- baseline is reproducible;
- the PR is Draft.

## Goal G1 — canonical requested/selected/active/observed snapshot

Tasks:

- Add one canonical negotiation snapshot using existing session ownership.
- Preserve requested values unchanged.
- Add selected, active, and observed values separately.
- Add millihertz or rational refresh representation while retaining compatibility with current integer FPS fields.
- Add codec/profile/bit-depth/chroma/color-space fields.
- Add bitrate envelope and fallback reasons.
- Expose the snapshot through existing SteamShine status/API services.

Do not:

- add a second global session manager;
- add a second Web status polling loop;
- persist high-rate samples.

Tests:

- requested value is never overwritten by fallback;
- selected and active mismatch is visible;
- integer FPS compatibility remains;
- 59.94 and 60.00 remain distinct when the protocol supplies enough precision.

Exit:

- current 1080p60 behavior is unchanged;
- status shows all four stages.

## Goal G2 — arbitrary safe resolution and FPS

Tasks:

### Owned private Gamescope

- Use normalized Moonlight dimensions and refresh for Gamescope canvas creation.
- Reuse existing `normalize_display_request()` and `gamescope_arguments()` after extending them safely.
- Add overflow, pixel-rate, encoder-limit, and alignment validation.
- Preserve arbitrary aspect ratios.
- Define retained-session compatibility by geometry, refresh, HDR intent, GPU, and source identity.
- Do not reuse an incompatible retained canvas silently.

### Physical KDE desktop

- Reuse existing display-device automatic resolution/refresh concepts and KScreen helper.
- Refactor toward one pure exact-mode selection function rather than two independent decision implementations.
- Use exact requested width/height and nearest safe refresh.
- Restore exact previous mode and scale.
- When exact physical dimensions are unavailable, select an owned virtual fallback if policy permits; otherwise fail with a stable reason.
- Do not substitute a different aspect ratio silently.

### Existing Game Mode Gamescope

- Do not restart or reconfigure the resident Gamescope.
- Keep its source canvas visible in status.
- Use the existing scaling/color-conversion path for requested encode geometry.
- Default to fit/letterbox.
- Compute the visible content rectangle.
- Map touch and absolute pointer input to that rectangle.

Tests:

- minimum/maximum/bad/overflow requests;
- odd dimension normalization and reported reason;
- ultrawide, 16:10, phone-native, and custom dimensions;
- 30/60/90/120/144/165/240 FPS requests;
- physical exact mode;
- physical no-mode -> virtual fallback;
- fixed Game Mode canvas -> requested encode geometry;
- content rectangle and touch mapping;
- retained-session compatibility.

Hardware gate:

- 1080p60 baseline;
- at least one custom aspect ratio;
- at least one 90+ FPS request;
- requested, selected, and active values match the report.

Exit:

- arbitrary safe request works on owned virtual display;
- unsupported requests fail or fall back explicitly.

## Goal G3 — end-to-end HDR

Tasks:

- Reuse client `enable_hdr`, `config::video.dd.hdr_option`, Gamescope `--hdr-enabled`, existing video color-space code, and 10-bit encoder profiles.
- Add `off`, `auto`, and `require` policy without creating synonymous config keys.
- Implement gates for client capability, source, display/Gamescope, capture metadata, conversion, encoder, GameStream signaling, and client acceptance.
- Record codec, profile, bit depth, primaries, transfer, matrix, range, chroma, and HDR-active state independently.
- For `auto`, fall back to SDR before sending bad signaling.
- For `require`, reject with a stable error.
- Do not mutate an unowned resident Game Mode session to force HDR.

Tests:

- HDR off;
- auto success;
- auto SDR fallback at each failed gate;
- require failure at each gate;
- HEVC Main10 SDR distinct from HDR;
- capture/encoder/signaling metadata mismatch.

Hardware gate:

- first verify HEVC Main10 SDR;
- then verify one known-good HDR display/client pair;
- record visual result and client overlay/status.

Exit:

- no washed-out or falsely signaled output is accepted;
- SDR baseline remains stable.

## Goal G4 — codec and processing quality

Tasks:

- Reuse existing H.264, HEVC, and AV1 probes and encoders.
- Add policy/scoring around existing candidates.
- Consider client support, host open result, bit depth, HDR, resolution/FPS, measured encode/decode latency, power, and prior success.
- Keep H.264 as recovery.
- Prefer AV1 only after probe and measurement.
- Keep codec fixed during a stream initially.
- Reuse existing Vulkan/VA-API rate-control, tune, QP, scaling, and color-conversion controls.
- Add selected codec reason to status.

Counterargument to address in PR:

```text
AV1 compresses better, but it may raise encode/decode latency, power, or incompatibility.
Therefore it is a measured preference, not an unconditional default.
```

Tests:

- candidate matrix;
- AV1 unavailable/slow fallback;
- HDR codec/profile requirements;
- no repeated encoder recreation;
- software encoder not selected unexpectedly.

Hardware gate:

- H.264 SDR;
- HEVC SDR;
- AV1 SDR on a verified client;
- compare bitrate, encode latency, frame pacing, and power.

Exit:

- automatic policy chooses a valid measured codec;
- manual codec requests fail safely when unsupported.

## Goal G5 — adaptive bitrate and communication quality

Tasks:

- Extend the existing bounded diagnostics and queues; do not add a parallel telemetry collector.
- Sample every 500 ms and decide over 2 seconds.
- Use loss, RTT growth, socket output queue, network queue age/depth, capture/encode age, frame drops, FEC/recovery, and IDR reasons.
- Add congestion states and stable reason codes.
- Reduce target quickly for persistent congestion.
- Increase gradually after clean intervals.
- Add hysteresis and cooldown.
- Clamp to client/admin/encoder/profile ceilings.
- Account for audio, headers, encryption, FEC, and recovery outside the encoder target.
- Probe runtime rate-control update support.
- If unsupported, learn the next-session initial bitrate instead of recreating the encoder repeatedly.
- Keep geometry fixed mid-stream initially.

Tests:

- trace replay for clean, loss, queue growth, RTT growth, recovery, oscillation, IDR cooldown;
- bounded target and queue;
- unsupported runtime rate update;
- learned next-session bitrate;
- no high-rate disk writes.

Hardware gate:

```text
Ethernet
5/6 GHz Wi-Fi
remote/VPN if available
static scene
continuous motion
60-second continuous input
10-second recovery after input stops
```

Record:

- target/actual bitrate;
- loss/RTT;
- queue age/depth;
- p50/p95/p99 capture/encode/network latency;
- rendered FPS and client queue when available;
- visible stutter;
- power and SSD writes.

Exit:

- no long-term latency accumulation;
- stable frame pacing;
- congestion lowers bitrate before geometry;
- recovery does not oscillate.

## Goal G6 — client profiles and persistence

Tasks:

- Implement profiles as defaults/envelopes over the actual request.
- Reuse the canonical snapshot and adaptive controller.
- Add generic, 4K, Y700, iPhone 16 Plus, Deck LCD, Deck OLED, and custom profiles.
- Selection priority: explicit override, exact request, previous success, capability signature, device-name hint, generic.
- Add per-client/orientation safe-area and content-rectangle calibration.
- Store only final aggregate results and low-rate profile state.
- Use atomic replace, schema version, size bounds, and owner-only permissions.

Tests:

- capability beats name;
- Y700 geometries remain distinct;
- Deck LCD/OLED distinction;
- phone safe area and rotation;
- network class separation;
- reset learned state.

Exit:

- profiles never override a valid explicit request silently;
- UI explains profile selection.

## Goal G7 — SteamShine-primary UI and backup Sunshine route

Tasks:

- Make SteamShine the default management route after streaming gates pass.
- Keep upstream Sunshine enabled and visible at the compatibility route.
- Verify shared credentials, clients, applications, and configuration.
- Add Stream Negotiation UI using the existing facade.
- Keep polling at a low bounded rate.
- Verify either UI can fail without stopping the stream.
- Perform configuration-only rollback to upstream default.

Exit:

- SteamShine is primary;
- Sunshine route is a working backup;
- one service and one shared backend remain.

## Goal G8 — final acceptance and merge

Run the available hardware matrix from `PROJECT_ROADMAP.md`.

Before each push:

```bash
gh run list \
  --repo souten-yd/SteamShine \
  --branch feat/moonlight-negotiation-hdr-quality \
  --limit 30
```

Do not push while the latest relevant run is queued or in progress. Batch related changes into one push.

Before merge:

- rebase/merge current master safely;
- full CI green on the reviewed head;
- delivery Artifact exists and matches commit/BUILD_INFO;
- install the Artifact through `steamshine.sh` and systemd user service;
- perform required hardware gates;
- update `docs/IMPLEMENTATION_STATUS.md` with measured facts only;
- update PR body with Artifact SHA, test matrix, remaining `not tested` hardware, and rollback;
- remove Draft only after required gates;
- merge with the expected head SHA.

## Required commit sequence

Use small, bisectable commits in this order:

```text
refactor(stream): add canonical negotiation snapshot
fix(steamos): honor arbitrary client geometry and refresh
feat(hdr): validate end-to-end HDR negotiation
feat(video): select probed codec and quality policy
feat(stream): add adaptive bitrate controller
feat(profile): persist per-client stream profiles
feat(web): expose stream negotiation and quality controls
test(stream): cover negotiation HDR codec and congestion
docs: record hardware acceptance and rollback
```

A goal may require more than one commit, but do not collapse all work into one generated commit.

## Stop conditions

Stop and report instead of weakening safety when:

- requested behavior requires a second Steam;
- attached Game Mode would need to be killed or mutated without consent;
- only CPU full-frame copy can make the path work;
- codec/HDR metadata cannot be made consistent;
- CI Artifact differs from tested code;
- a hardware gate cannot be executed;
- a proposed new subsystem duplicates an existing implementation.

Mark unavailable hardware or telemetry as `not tested` or `unavailable`, never as passed.
