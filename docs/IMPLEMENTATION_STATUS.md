# Implementation status

- Baseline `master` / PR #12 merge:
  `8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a`.
- PR #13 documentation baseline:
  `47dd88f2cbd29da83454fd3624172574d105d52d`.
- PR #11: merged as `578b3e7c2bab64a1752e42433fc9a26aae8a7fc5`;
  validated head `45d36f1cbf20e1c466f7c6adfec64eeb3d16cf35`.
- PR #12 validated head:
  `c0d5d61657a3895b586892d10756a7d89715af23`.
- Latest matching validation: SteamOS Runtime Build run `30459891199`, Artifact
  `steamshine-steamos-x86_64-c0d5d61657a3895b586892d10756a7d89715af23`.
- Recorded rollback install: `45d36f1cbf20e1c466f7c6adfec64eeb3d16cf35`.
- Future-work sources of truth: `PROJECT_ROADMAP.md`,
  `STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md`, and
  `CODEX_GOAL_MODE_STREAMING.md`.
- PR C working branch: `feat/probed-codec-policy` from the PR #13 planning
  baseline. Client/host codec intersection, strict/manual H.264 recovery,
  hardware-only default, optional codec advertisement filtering, and bounded
  codec status are implemented; matching hardware acceptance is pending.

Only these status values are used below:

```text
Implemented and hardware accepted
Implemented, hardware acceptance pending
Partially implemented
Detailed design only
Blocked by external dependency
Not implemented
```

Earlier hardware success is evidence about the implementation, but it does not
become acceptance for a new head. A row says hardware accepted only when its
scope has matching Artifact/BUILD_INFO evidence; narrower acceptance is stated
in the evidence column.

| Item | Status | Evidence and remaining gate |
| --- | --- | --- |
| Service/Artifact lifecycle | Implemented and hardware accepted | Immutable checksum-verified installation, `BUILD_INFO.json`, systemd user autostart, versioned `current`/rollback, and installer CI exist. PR #12 Artifact was installed locally with matching binary SHA, preserved configuration, active service, and `NRestarts=0`. |
| Source routing | Implemented, hardware acceptance pending | Pure routing distinguishes physical Desktop, verified attached stock Gamescope, retained owned-private, new owned-private, and rejection. The PR #12 automatic order remains authoritative for ordinary applications; canonical Big Picture launch commands now prefer owned Gamescope after verified stock Game Mode so Desktop-launched Big Picture cannot be captured from the physical connector accidentally. Re-run the complete three-source matrix and the Big Picture exception on the matching Artifact. |
| Owned private Gamescope | Implemented, hardware acceptance pending | Private runtime ownership, headless Gamescope, Steam singleton protection, application endpoint, retain/reconnect, and owned-only cleanup exist. Prior heads ran on hardware; matching post-merge baseline acceptance remains required. |
| Attached stock Gamescope | Implemented, hardware acceptance pending | Verified producer/Steam environment, PID/UID/start-time identity, non-ownership, detach-without-kill, and same-GPU capture selection exist. Repeat live video/input/reconnect/service-stop on the matching post-merge baseline. |
| Physical Desktop | Implemented, hardware acceptance pending | KWin ScreenCast and exact KScreen mode/restore have prior hardware evidence. Exact-size physical selection now accepts the requested refresh or nearest lower safe refresh, never a higher rate; an incompatible mode returns a bounded protocol result and automatic policy retries once on an owned canvas after physical state restoration. Re-run exact, lower-refresh, fallback, and restore cases on the matching feature Artifact. |
| PipeWire DMA-BUF | Implemented, hardware acceptance pending | Verified render-node modifier discovery/import and Gamescope/KWin DMA-BUF paths exist. PR #12 startup probe and physical sessions succeeded; repeat owned and attached DMA-BUF cases on the matching final baseline. |
| Latest-frame-wins | Implemented and hardware accepted | PipeWire pending DMA-BUF capacity is one, replacement is counted, capture/encoder/network queues are bounded, and PR #12 session diagnostics showed network queue maximum one without growth. |
| Frame pacing | Implemented and hardware accepted | Rational FPS helpers, monotonic deadline scheduling, bounded duplicate output, source/encode FPS, and interarrival diagnostics exist. PR #12 physical 60 FPS session observed approximately 59 FPS with p99 encode interarrival near 18 ms. Wider FPS matrix remains future work. |
| DMA-BUF lifetime synchronization | Implemented, hardware acceptance pending | RAII PipeWire buffer lease and Vulkan completion fence prevent buffer recycle while compute reads external storage. Unit/integration/CI pass; repeat owned/attached reconnect stress on the matching final baseline. |
| Requested-size negotiation | Implemented, hardware acceptance pending | Gamescope private `requested_size` is sent from selected encode dimensions and tested in CI. Requested, selected, active PipeWire producer, and encoded content-rectangle dimensions are now reported separately. Full producer-size hardware acceptance remains pending. |
| Vulkan scaling/content rectangle | Implemented, hardware acceptance pending | Vulkan conversion and absolute input use one centered, even-aligned, aspect-preserving fit model. Letterbox/pillarbox input explicitly clamps or rejects by policy, and the active content rectangle is exposed in status. Custom-aspect hardware acceptance remains pending. |
| Input routing/visualizer | Implemented, hardware acceptance pending | Desktop input, verified Gamescope EIS isolation, bounded/coalesced input diagnostics, and packaged Input Latency Visualizer exist. Custom-aspect touch/pointer mapping and complete keyboard/gamepad matrix need matching-Artifact acceptance. |
| Session diagnostics JSON | Implemented and hardware accepted | One bounded final report records Artifact commit, source, geometry, codec, bitrate, source/encode FPS, queue/age distributions, IDR and input counters. The PR A implementation also serializes the session-owned requested/selected/active/observed negotiation snapshot; matching-Artifact hardware inspection remains pending for those new fields. |
| Canonical negotiation state | Implemented, hardware acceptance pending | NVHTTP launch and RTSP ANNOUNCE append immutable request facts to one generation, the existing stream session owns separate selected/active/observed stages, and the existing status API and final report expose the same bounded snapshot. Automated tests cover rational 59.94 versus 60, request preservation, stage mismatch, stable fallback reasons, schema, and the 60-second bounded-input CI gate; a matching Artifact must still prove 1080p60 H.264 SDR on hardware. |
| Arbitrary resolution/FPS | Implemented, hardware acceptance pending | Requests cover 640–7680 by 480–4320 and exact rational 30–240 Hz selection, with explicit minimal-alignment or exact-reject policy. Coded extent, pixel rate, overflow, conservative frame-buffer budget, retained canvas/color/GPU/source/format identity, physical exact/lower/fallback policy, and the required FPS matrix are automatically tested. Active codec capability intersection is completed by PR C; custom, portrait, ultrawide, and 90+ Hz matching-Artifact hardware acceptance remains pending. |
| H.264 | Implemented, hardware acceptance pending | Existing advertisement, hardware probe/open, packetization, IDR, and Vulkan encode path remain authoritative. PR C preserves H.264 as explicit recovery only when the configured manual target is unusable and the client requests the advertised recovery path. Matching Artifact acceptance remains pending. |
| HEVC | Implemented, hardware acceptance pending | Existing Main/Main10 advertisement, probe/open, packetization, and Vulkan path remain authoritative. PR C filters advertisement and stream startup through client selection, host open, hardware, geometry, bit-depth, latency, power, history, and administrator gates with visible reasons. The complete SDR hardware matrix is pending. |
| AV1 | Implemented, hardware acceptance pending | Existing AV1 advertisement, open probe, Vulkan encode, and packetization remain authoritative. PR C prevents unconditional AV1 selection and rejects unprobed or policy-incompatible requests. Verified client decode, latency/power, high-FPS, reconnect, and recovery acceptance remains pending. |
| HDR | PR D implemented and locally tested; hardware dependency remains | `feat/hdr10-negotiation` adds explicit `off`, `auto`, and `require` policy, preserves requested HDR separately from selected bit depth, evaluates client/source/display/capture/metadata/conversion/encoder/signaling gates, fails closed after an unsafe late fallback, and reports requested/selected/active color state. Stock Gamescope 3.16.23.4 exports only 8-bit SDR PipeWire frames even with `--hdr-enabled`; the tracked SteamOS producer patch adds xBGR210, BT.2020/PQ metadata, and HDR export color management without replacing `/usr/bin/gamescope`. Its build succeeds, but matching live producer/client acceptance remains required; Main10 alone is not HDR. |
| VBR | Implemented, hardware acceptance pending | Existing encoder VBR modes now feed a formal video-only minimum/initial/target/maximum/peak/VBV envelope and active diagnostics. Cross-backend hardware measurements remain pending. |
| Adaptive Bitrate | Implemented, hardware acceptance pending | PR E adds a fixed-memory 500 ms sample/2 s decision controller, client-loss and sender-queue feedback, bounded 20%/10% reduction and 5% recovery, hysteresis, IDR/reconnect cooldown, a typed FFmpeg runtime adapter, and next-session learning without encoder recreation or retries. Ethernet/Wi-Fi controlled-loss runs and backend-specific runtime acceptance remain external hardware gates. |
| Per-client profiles | Implemented, hardware acceptance pending | PR F adds an owner-private, schema-versioned, atomic, 64-entry profile store keyed by paired-client ID and network class. The user explicitly activates one network class per client; IP addresses are never used to guess LAN, Wi-Fi, or overlay state. RTSP supplies a signature from current request facts, applies only safe FPS/bitrate/HDR-off bounds, records codec/HDR conflicts that yield to the client, and persists the final bounded learned start rate into the same exact active profile. Geometry/quality/orientation/safe-area selections remain visible for final hardware acceptance and later source/backend-specific application. |
| Negotiation Web UI | Implemented, hardware acceptance pending | PR F adds an authenticated Stream page with requested/selected/active/observed sections, two-second bounded polling, exact profile selection explanation, profile save/reset, and a direct Sunshine fallback-settings route. Live localhost/LAN browser acceptance on the integrated Artifact remains pending. |
| Hardware matrix | Partially implemented | Service installation, physical 1080p60, encoder probes, input diagnostics, and earlier owned/attached cases have evidence. The canonical geometry/FPS/codec/color/source/network matrix must be run independently for each matching feature Artifact; unavailable cases remain `not tested`. |
