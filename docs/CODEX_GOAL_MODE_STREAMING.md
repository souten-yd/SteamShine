# Codex Goal Mode — staged Moonlight streaming implementation

Use this file as the execution contract for the independent feature PRs that
follow PR #12. It is not an instruction to implement every feature at once.

## Mission and baseline

Extend the existing Sunshine-derived GameStream, session, capture, encoder,
packetization, audio, input, application, configuration, and Web facade paths so
Moonlight/Artemis requests become an explicit
`requested -> selected -> active -> observed` transaction.

Repository: `souten-yd/SteamShine`

Baseline: `master` at or after merged PR #12. The merge and validation evidence
used to prepare this plan is:

```text
PR #11 merge: 578b3e7c2bab64a1752e42433fc9a26aae8a7fc5
PR #11 head / recorded rollback: 45d36f1cbf20e1c466f7c6adfec64eeb3d16cf35
PR #12 merge / baseline master: 8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a
PR #12 validated head: c0d5d61657a3895b586892d10756a7d89715af23
PR #12 successful SteamOS Runtime Build: 30459891199
PR #12 matching Artifact:
  steamshine-steamos-x86_64-c0d5d61657a3895b586892d10756a7d89715af23
```

Create every implementation branch from current `master`, which must contain
PR #12 merge `8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a` or a later descendant.

## Mandatory workflow for every PR

Before editing:

```bash
git status
git fetch --all --prune
git switch master
git pull --ff-only
git log --oneline --decorate -30
gh pr list --state open --repo souten-yd/SteamShine
gh run list --repo souten-yd/SteamShine --limit 30
```

Rules:

1. Work on exactly one feature PR at a time and rebase it on current `master`
   before implementation and final acceptance.
2. Trace existing ownership before adding any type, manager, queue, endpoint,
   config key, encoder abstraction, or persistence store. No code before trace.
3. Do not push while relevant CI is queued or in progress. Batch related fixes.
4. Test hardware only with the Artifact whose `BUILD_INFO.json` matches the
   reviewed head. Without a hardware result, write `not tested`, never accepted.
5. Preserve the SteamShine systemd service, immutable install/rollback, shared
   Sunshine backend, and upstream Sunshine recovery UI.
6. A SteamShine UI/API failure must not interrupt an active stream.
7. Never start a second Steam, kill or reconfigure attached Gamescope, weaken
   ownership checks, or allow game/capture/encode to drift to different GPUs.
8. No automatic software encoder fallback, unbounded queue, per-frame disk
   write, per-frame external CLI, or normal-path `vkQueueWaitIdle` /
   `vkDeviceWaitIdle`.
9. Initial implementation keeps resolution, FPS, codec, and HDR/SDR fixed during
   a stream. Only bitrate may change when the active backend supports it.
10. Do not create a parallel protocol server, encoder stack, session manager,
    telemetry collector, pairing store, or configuration system.

Every PR body contains:

```text
Base and reviewed head SHA
Existing code traced and reused
Files changed and files explicitly not replaced
Requested/selected/active/observed impact
Unit and integration results
CI run and matching Artifact
Hardware cases: passed / failed / not tested
Rollback Artifact and feature rollback
Remaining risks and external dependencies
```

## PR A — canonical negotiation foundation

```text
Branch: feat/stream-negotiation-state
Title: refactor(stream): add canonical Moonlight negotiation state
```

Mission:

- trace NVHTTP launch fields and later RTSP ANNOUNCE fields;
- add one session-owned generation snapshot with immutable requested data and
  separate selected, active, and observed data;
- preserve rational FPS, codec mask, bitrate envelope, color state, and stable
  fallback reasons;
- expose the snapshot through the existing status API and final session JSON.

Files likely to change:

```text
src/rtsp.h
src/rtsp.cpp
src/nvhttp.cpp
src/stream.cpp
src/stream.h
src/video.cpp
src/video.h
src/web_services.cpp
tests/unit/
```

Files/systems not to replace: RTSP/NVHTTP servers, `launch_session_t` ownership,
`stream::session`, status facade, or diagnostics collector.

Unit tests: requested values survive fallback; launch and RTSP values populate
the same generation; rational 59.94 differs from 60; selection/active mismatch
is visible; fallback reasons are stable; old integer FPS remains compatible.

Integration tests: launch then ANNOUNCE population, resume/reconnect generation,
status JSON schema, bounded final report, and unchanged 1080p60 SDR startup.

Hardware gate: matching Artifact starts one 1080p60 H.264 SDR session and reports
all four stages; source/encode FPS and queue diagnostics remain valid.

Exit: one authoritative snapshot exists with no duplicate global manager and no
change to current route or encoder policy.

Rollback: disable only the new status exposure if necessary, then install the
PR #12 verified Artifact. Do not weaken ownership or diagnostics bounds.

Expected commits:

```text
refactor(stream): add canonical negotiation snapshot
test(stream): cover staged negotiation ownership
docs: record negotiation foundation acceptance
```

## PR B — arbitrary safe geometry and refresh

```text
Branch: feat/arbitrary-geometry-refresh
Title: feat(display): honor safe arbitrary client geometry and refresh
```

Mission: validate and select source-aware geometry and 30–240 FPS, including
pixel-rate limits, alignment, retained-session compatibility, fit/letterbox
content rectangles, and absolute input mapping.

Files likely to change:

```text
src/steamos_virtual_session.cpp
src/steamos_virtual_session_core.cpp
src/steamos_virtual_session_core.h
src/platform/linux/gamescopegrab.cpp
src/platform/linux/pipewire.cpp
src/platform/linux/vulkan_encode.cpp
src/video.cpp
scripts/configure-steamos-client-display.py
tests/unit/
tests/integration/
```

Files/systems not to replace: existing source router, KScreen/display-device
ownership, Gamescope source discovery, PipeWire capture, Vulkan conversion, or
input subsystem.

Required policy:

- dimensions remain within 640–7680 and 480–4320 but are also bounded by coded
  extent, pixel rate, GPU/decoder capability, VRAM/buffer budget, and admin cap;
- `auto` minimally aligns and records a reason; `require_exact` rejects;
- owned canvas follows the selected request and arbitrary aspect ratio;
- retained compatibility includes width, height, rational refresh, HDR intent,
  GPU/render node, source identity, and capture pixel-format requirements;
- attached Gamescope is untouched; source canvas, PipeWire size, encode size,
  and content rectangle remain separate;
- physical selection tries exact dimensions with nearest safe refresh, permitted
  lower refresh, owned fallback, then rejection.

Unit tests: overflow/pixel-rate/alignment; 30, 50, 59.94, 60, 75, 90, 100,
120, 144, 165, 240; retained key; exact/lower/virtual/reject physical decisions;
content rectangle and margin input policy.

Integration tests: custom aspect launch, requested_size, fixed resident canvas
to fitted encode output, physical restore, incompatible retained replacement,
and no attached-process mutation.

Hardware gate: 1080p60 plus custom/portrait or ultrawide and one 90+ FPS case;
requested/selected/active geometry, input mapping, first frame, and restore are
recorded from a matching Artifact.

Exit: every safe request is selected or rejected with reason; source motion FPS
and encoded output FPS are never described as interpolation.

Rollback: fixed owned-private 1080p60, then PR A/PR #12 Artifact. Do not use a
different physical aspect ratio as silent fallback.

Expected commits:

```text
feat(display): validate source-aware geometry and rational refresh
fix(input): map fitted stream content coordinates
test(display): cover arbitrary geometry and retained compatibility
docs: record geometry acceptance
```

## PR C — probed SDR codec policy

```text
Branch: feat/probed-codec-policy
Title: feat(video): select H.264 HEVC and AV1 from probed capabilities
```

Mission: select H.264 8-bit, HEVC Main/Main10 SDR, and AV1 Main 8/10-bit SDR
only from the intersection of client advertisement, host open probe, request,
latency/power budget, previous success, and administrator policy.

Files likely to change: `src/nvhttp.cpp`, `src/rtsp.cpp`, `src/video.*`, existing
encoder backend hooks, status facade, and codec tests.

Files/systems not to replace: codec packetization, FFmpeg profiles, existing
probe/open path, Vulkan/VA-API encoder stacks, or GameStream advertisement.

Policy: `auto/h264/hevc/av1`; quality preference may use AV1→HEVC→H.264;
recovery uses H.264→HEVC→AV1. Manual policy is strict or reasoned fallback.
Codec is fixed mid-stream and software fallback is opt-in diagnostics only.

Unit tests: client/host matrix, profile/bit-depth constraints, H.264 recovery,
manual strict rejection, reasoned fallback, and no unconditional AV1 default.

Integration tests: real encoder probe fixtures, codec open/failure, packet
headers/keyframes, reconnect, and no encoder recreation loop.

Hardware gate: H.264 SDR and HEVC SDR required; AV1 SDR is accepted only on a
verified client with bitrate, encode/decode latency, pacing, and power evidence.

Exit: selected/active codec, profile, bit depth, backend, render node, and reason
are visible; unsupported candidates cannot be advertised as active.

Rollback: force H.264 SDR, then install PR B/PR #12 Artifact.

Expected commits:

```text
feat(video): select codecs from client and host probes
test(video): cover codec profile and recovery policy
docs: record SDR codec acceptance
```

## PR D — HDR10 transaction

```text
Branch: feat/hdr10-negotiation
Title: feat(hdr): validate end-to-end HDR10 streaming
```

Mission: implement `off/auto/require` across client request, source/display,
10-bit PipeWire capture, color metadata, Vulkan import, RGB-to-P010 conversion,
HEVC Main10 or AV1 Main10, GameStream signaling, and client rendering.

Files likely to change: existing negotiation snapshot, display/session policy,
`pipewire.cpp`, `vulkan_encode.cpp`, `video.*`, `stream.cpp`, status/API, and HDR
tests. A Gamescope producer patch belongs in a separate repository/Artifact.

Files/systems not to replace: display-device HDR ownership, Gamescope source
router, color conversion framework, FFmpeg encoder, or GameStream HDR control.

Unit tests: each gate for off/auto/require, metadata consistency, Main10 SDR vs
HDR, attached SDR `require` rejection, and pre-stream SDR fallback.

Integration tests: xBGR_210LE/XBGR2101010 metadata, BT.2020/PQ, P010, HEVC/AV1
Main10 probe, signaling, renegotiation failure, and no washed-out hybrid path.

Hardware gate: HEVC Main10 SDR first, then a known-good HDR client/display. AV1
Main10 HDR remains `not tested` until its own matching hardware result exists.

Exit: `HDR active` means all gates passed; `auto` never sends inconsistent HDR;
`require` rejects with a stable reason.

Rollback: HDR off, then H.264 SDR, then install PR C/PR #12 Artifact.

Expected commits:

```text
feat(hdr): add end-to-end HDR transaction gates
test(hdr): cover color metadata and fallback consistency
docs: record HDR dependency and acceptance
```

## PR E — VBR envelope and Adaptive Bitrate

```text
Branch: feat/adaptive-bitrate
Title: feat(stream): add bounded adaptive bitrate control
```

Mission: formalize minimum/initial/target/maximum/peak/VBV VBR state, add a
backend runtime-rate adapter, and adjust bitrate—not geometry—from bounded
network feedback.

Files likely to change: `video.*`, existing encoder hooks, `stream.*`,
`network.cpp`, diagnostics, status/API, config, and controller tests.

Files/systems not to replace: encoder stack, network sender, packetization,
existing fixed-memory diagnostics, or session report writer.

Controller: 500 ms samples, 2 s decision window, 15–25% persistent-congestion
reduction, about 10% repeated-isolated-loss reduction, at most 3–5% clean-window
increase, hysteresis, IDR/reconnect cooldown, and states unknown/clean/warning/
congested/recovering. Clamp to all client/admin/encoder/pixel-rate/profile/
learned ceilings. Keep audio/header/encryption/FEC/recovery outside video target
but inside total network budget.

Unit tests: envelope clamp, VBR vs adaptation, clean/loss/RTT/queue traces,
hysteresis, cooldown, unsupported update, failed update, and learned next start.

Integration tests: frame-boundary update with typed driver result, bounded sender
under controlled congestion, no recreation/retry loop, and one final disk report.

Hardware gate: Ethernet, 5 GHz Wi-Fi, 6 GHz if available, controlled loss/RTT/
bandwidth reduction and recovery; record target/actual bitrate, loss, RTT,
queues, latency percentiles, FPS, power, and SSD writes.

Exit: congestion lowers bitrate before geometry, queues remain bounded, and
recovery does not oscillate. Unsupported backends retain the stream and learn
only the next-session start.

Rollback: disable adaptation, use fixed VBR/CBR, force H.264 SDR if needed, then
install PR D/PR #12 Artifact.

Expected commits:

```text
refactor(video): expose bounded runtime bitrate updates
feat(stream): add adaptive bitrate controller
test(stream): replay congestion and recovery traces
docs: record adaptive bitrate acceptance
```

## PR F — profiles and Stream Negotiation UI

```text
Branch: feat/stream-profiles-ui
Title: feat(web): expose stream negotiation and client profiles
```

Mission: show requested/selected/active/observed state and add per-client/network
defaults without overriding actual client capabilities.

Files likely to change: existing SteamShine facade/status services, existing Web
assets, `en` localization only, config/profile persistence, and Web/API tests.

Files/systems not to replace: Web server, status aggregation, shared config,
pairing identity, encoder settings, or upstream Sunshine recovery route.

UI sections and controls follow
`STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md`: geometry exact/fit/virtual fallback,
FPS auto/custom, codec policy, HDR policy, automatic/custom bitrate ceiling,
quality preset, learned network class, and reset/rollback controls.

Unit tests: profile priority, capability over marketing name, network separation,
orientation/safe area, bounded schema, reset, and invalid policy rejection.

Integration tests: authenticated API and browser state, shared config with the
upstream UI, low bounded polling, UI failure during a stream, and rollback.

Hardware gate: localhost/LAN UI, active 1080p60 stream, live four-stage state,
profile selection explanation, reset, and upstream UI recovery.

Exit: UI accurately reflects core state and cannot stop or own the media path.

Rollback: disable the negotiation page/profile policy, retain the shared core,
use the upstream Sunshine route, then install PR E/PR #12 Artifact.

Expected commits:

```text
feat(web): expose stream negotiation state
feat(profile): persist bounded client network profiles
test(web): cover negotiation UI and shared-backend rollback
docs: record profiles and UI acceptance
```

## Final release acceptance

After PR F, run the complete geometry/FPS/codec/color/source/network matrix in
`STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md`. Required invariants include first
frame within one second, stable bounded queues, input within one or two frames in
the deterministic test, no encoder recreation loop, no false HDR, correct
ultrawide/letterbox mapping, no AMDGPU fault/reset, `NRestarts=0`, no high-rate
SSD writes, and exact Artifact/BUILD_INFO provenance.

## Stop conditions

Stop and report instead of weakening safety when the request would require a
second Steam, attached Gamescope mutation, CPU full-frame fallback, inconsistent
codec/HDR metadata, a nonmatching Artifact, unavailable mandatory hardware, or a
new subsystem that duplicates existing ownership. Record unavailable evidence as
`not tested` or `unavailable`, never passed.
