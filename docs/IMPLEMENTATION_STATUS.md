# Implementation status

- Baseline `master` / PR #12 merge:
  `8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a`.
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
| Source routing | Implemented, hardware acceptance pending | Pure routing distinguishes physical Desktop, verified attached stock Gamescope, retained owned-private, new owned-private, and rejection. Re-run the complete three-source matrix on the final merged PR #12 Artifact. |
| Owned private Gamescope | Implemented, hardware acceptance pending | Private runtime ownership, headless Gamescope, Steam singleton protection, application endpoint, retain/reconnect, and owned-only cleanup exist. Prior heads ran on hardware; matching post-merge baseline acceptance remains required. |
| Attached stock Gamescope | Implemented, hardware acceptance pending | Verified producer/Steam environment, PID/UID/start-time identity, non-ownership, detach-without-kill, and same-GPU capture selection exist. Repeat live video/input/reconnect/service-stop on the matching post-merge baseline. |
| Physical Desktop | Implemented and hardware accepted | KWin ScreenCast and exact KScreen mode/restore have hardware evidence. The installed PR #12 Artifact produced 1920×1080@60 sessions with approximately 59 observed source/encode FPS and bounded queues. Unsupported-mode owned fallback remains future geometry policy. |
| PipeWire DMA-BUF | Implemented, hardware acceptance pending | Verified render-node modifier discovery/import and Gamescope/KWin DMA-BUF paths exist. PR #12 startup probe and physical sessions succeeded; repeat owned and attached DMA-BUF cases on the matching final baseline. |
| Latest-frame-wins | Implemented and hardware accepted | PipeWire pending DMA-BUF capacity is one, replacement is counted, capture/encoder/network queues are bounded, and PR #12 session diagnostics showed network queue maximum one without growth. |
| Frame pacing | Implemented and hardware accepted | Rational FPS helpers, monotonic deadline scheduling, bounded duplicate output, source/encode FPS, and interarrival diagnostics exist. PR #12 physical 60 FPS session observed approximately 59 FPS with p99 encode interarrival near 18 ms. Wider FPS matrix remains future work. |
| DMA-BUF lifetime synchronization | Implemented, hardware acceptance pending | RAII PipeWire buffer lease and Vulkan completion fence prevent buffer recycle while compute reads external storage. Unit/integration/CI pass; repeat owned/attached reconnect stress on the matching final baseline. |
| Requested-size negotiation | Implemented, hardware acceptance pending | Gamescope private `requested_size` is sent from requested encode dimensions and tested in CI. Source/PipeWire/encode active size separation and full hardware matrix remain pending. |
| Vulkan scaling/content rectangle | Partially implemented | Vulkan conversion performs centered aspect-preserving fit/letterbox and cursor scaling. The canonical content rectangle, explicit upscale policy, margin input behavior, and status model are not complete. |
| Input routing/visualizer | Implemented, hardware acceptance pending | Desktop input, verified Gamescope EIS isolation, bounded/coalesced input diagnostics, and packaged Input Latency Visualizer exist. Custom-aspect touch/pointer mapping and complete keyboard/gamepad matrix need matching-Artifact acceptance. |
| Session diagnostics JSON | Implemented and hardware accepted | One bounded final report records Artifact commit, source, geometry, codec, bitrate, source/encode FPS, queue/age distributions, IDR and input counters. PR #12 reports were generated and inspected; canonical four-stage/color/congestion fields remain future work. |
| Canonical negotiation state | Not implemented | NVHTTP launch and RTSP ANNOUNCE currently populate different existing objects. There is no single session-owned requested/selected/active/observed snapshot or complete fallback reason chain. |
| Arbitrary resolution/FPS | Partially implemented | Integer dimensions 640–7680, 480–4320, and FPS 30–240 are normalized; owned Gamescope arguments, KScreen exact mode, requested_size, and conversion scaling exist. Pixel-rate probing, rational propagation, alignment policy, source-aware fallback, and full 30–240 matrix are missing. |
| H.264 | Implemented, hardware acceptance pending | Existing advertisement, hardware probe/open, packetization, IDR, and Vulkan encode path succeed in CI/startup probes. PR C must preserve it as the explicitly accepted recovery codec on its matching Artifact. |
| HEVC | Implemented, hardware acceptance pending | Existing Main/Main10 advertisement, probe/open, packetization, and Vulkan path exist; PR #12 local sessions recorded codec 1. Formal client/host selection reason and the complete SDR matrix are pending. |
| AV1 | Implemented, hardware acceptance pending | Existing AV1 advertisement, open probe, Vulkan encode and packetization components exist and startup probing succeeded. A verified client decode, latency/power, high-FPS, reconnect, and recovery matrix has not been accepted. |
| HDR | Blocked by external dependency | Client intent, display-device policy, owned `--hdr-enabled`, 10-bit RGB PipeWire formats, BT.2020/PQ recognition, P010 conversion/encode profiles, metadata, and GameStream signaling components exist. SteamOS end-to-end success requires a consistent Gamescope 10-bit/HDR producer and matching client acceptance; Main10 alone is not HDR. |
| VBR | Partially implemented | Existing encoder settings include VBR modes; Vulkan `vk_rc_mode=4` clears `rc_min_rate` and startup bitrate/max/VBV values are configured. A formal envelope, active reporting, and cross-backend acceptance are missing. |
| Adaptive Bitrate | Not implemented | Existing loss/IDR/socket/capture/encode/network diagnostics are inputs only. The 500 ms sample, 2 s controller, congestion state, hysteresis/cooldown, runtime adapter, and next-session learning do not exist. |
| Per-client profiles | Detailed design only | Paired identity exists, but negotiation profiles, capability signatures, network classes, previous-success selection, safe area, and learned starting bitrate are not implemented. |
| Negotiation Web UI | Detailed design only | Existing Monitor/status facade shows route and pipeline diagnostics. It does not yet show four-stage geometry/color/codec/rate state or expose negotiation policies and profile reset. |
| Hardware matrix | Partially implemented | Service installation, physical 1080p60, encoder probes, input diagnostics, and earlier owned/attached cases have evidence. The canonical geometry/FPS/codec/color/source/network matrix must be run independently for each matching feature Artifact; unavailable cases remain `not tested`. |
