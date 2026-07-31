# SteamShine canonical project roadmap

- Status: active source of truth
- Baseline `master` / PR #12 merge:
  `8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a`.
- PR #12 validated head: `c0d5d61657a3895b586892d10756a7d89715af23`;
  successful SteamOS Runtime Build run `30459891199`.
- Primary platform: SteamOS 3.8.x, AMD Radeon RX 9070 XT, `/dev/dri/renderD128`
- Primary clients: Moonlight and Artemis
- Primary server identity: SteamShine
- Compatibility fallback: upstream Sunshine UI and compatible protocol behavior

## 1. Product direction

SteamShine is the primary game-streaming server and management surface for this repository. The inherited Sunshine implementation remains an upstream-compatible fallback and recovery path, not a second independent server.

The project must continue to reuse the existing Sunshine/GameStream protocol, capture, audio, input, encoder, pairing, application, and configuration implementations. New SteamShine code may coordinate or strengthen those paths, but must not duplicate them without a measured and documented reason.

Operational policy:

1. The installed service and delivered binary are SteamShine.
2. `/steamshine/` is the primary management experience.
3. The upstream Sunshine UI remains available at its compatibility route as a backup.
4. Credentials, paired clients, applications, configuration, capture, encoding, audio, and input use one shared backend.
5. Disabling or breaking the SteamShine UI must not stop an active stream.
6. The upstream compatibility path must remain usable until at least two hardware-accepted SteamShine releases and a successful rollback drill.

## 2. Current implementation baseline

The following existing behavior is the foundation and must be extended rather than replaced.

### 2.1 Service and Artifact lifecycle

SteamShine is installed as a systemd user service from an immutable, checksum-
verified SteamOS Artifact. `BUILD_INFO.json`, versioned install directories,
the `current` symlink, and the recorded rollback version make the running commit
auditable and rollback recoverable.

The PR #12 matching Artifact is
`steamshine-steamos-x86_64-c0d5d61657a3895b586892d10756a7d89715af23`.
The immediately recorded rollback install is PR #11 head
`45d36f1cbf20e1c466f7c6adfec64eeb3d16cf35`. Neither reference proves a later
feature; every feature PR must build, install, and test its own matching Artifact.

### 2.2 Moonlight request ingestion

NVHTTP `make_launch_session()` parses the `mode` query as width, height, and
integer FPS, and also records `hdrMode`, `uniqueid`, `appid`, audio, controller,
SOPS, key, and resume data in `rtsp_stream::launch_session_t`. `process.cpp`
exports the client width, height, FPS, and HDR intent through
`SUNSHINE_CLIENT_*` before application preparation.

RTSP `ANNOUNCE` later parses the authoritative stream viewport width/height,
integer `maxFPS`, optional refresh multiplied by 100, maximum bitrate in Kbps,
configured total bitrate in Kbps, selected bitstream format, dynamic-range mode,
chroma, FEC, and packet parameters into `stream::config_t`. Codec choice and the
adjusted video bitrate therefore do not exist in `launch_session_t` at NVHTTP
launch time.

`launch_session_t` already carries:

- client width;
- client height;
- requested FPS;
- HDR intent;
- client identity;
- application ID;
- audio and controller options;
- encryption and resume state.

This split is why the canonical snapshot must be attached to existing session
ownership and populated in stages. It must not assume all negotiated fields are
available at launch.

### 2.3 Source routing and physical display handling

The inherited display-device configuration already defines automatic resolution, automatic refresh rate, automatic HDR, and mode remapping concepts.

SteamShine also packages a KDE/KScreen preparation hook that:

- applies a physical mode matching the Moonlight width and height when registered;
- otherwise applies the registered dimensions nearest to the Moonlight request;
- chooses the nearest safe refresh for the selected size;
- restores the previous mode and scale on application exit;
- skips physical KScreen changes for an owned Gamescope virtual session.

Automatic source routing already distinguishes:

- a capturable physical KDE Desktop;
- a verified attached stock Game Mode Gamescope;
- a compatible retained SteamShine-owned private Gamescope;
- a newly created SteamShine-owned private Gamescope;
- an explicit rejection when the selected ownership policy cannot be met.

Attached Gamescope is never killed or reconfigured by the normal route.

### 2.4 Owned and attached Gamescope capture

The SteamOS virtual-session path already:

- normalizes requested width to 640–7680;
- normalizes requested height to 480–4320;
- normalizes requested FPS to 30–240;
- creates an owned headless Gamescope session for remote-only operation;
- creates a fullscreen Wayland-backend Gamescope on verified KWin when local
  presentation is selected;
- passes nested width, height, and refresh to Gamescope;
- passes `--hdr-enabled` when HDR is requested and Gamescope advertises support;
- captures the verified Gamescope PipeWire `Video/Source` over DMA-BUF;
- encodes on the selected AMD GPU through Vulkan Video;
- validates one AMD render node for game, capture, and encode;
- retains owned sessions for reconnect when geometry, refresh, HDR intent,
  backend, KWin endpoint generation, local-presentation requirement, process
  identity, and socket state remain compatible;
- gracefully migrates only a uniquely verified idle Desktop Steam singleton and
  refuses active, ambiguous, reused-PID, or timed-out migration attempts.
- optionally lends a uniquely verified idle stock Game Mode session to an owned
  headless canvas, while active or ambiguous game activity remains attached to
  the unchanged stock producer;
- binds that transition to a boot/PID/start-time lease so the stock launcher is
  restored after normal cleanup, launch failure, process death, or reboot.

The attached and owned paths also include:

- verified producer PID, UID, start time, node identity, and render node;
- Gamescope private `requested_size` negotiation;
- a one-buffer latest-frame-wins DMA-BUF handoff;
- RAII buffer leases and a Vulkan completion fence before releasing borrowed
  PipeWire storage;
- Vulkan RGB-to-NV12/P010 conversion, centered fit/letterbox scaling, and cursor
  conversion;
- bounded reconnect and DMA-BUF lifetime handling.

The current retained compatibility key is not complete: it does not yet include
rational refresh, capture pixel-format requirements, or a canonical source
identity key. Attached source canvas, PipeWire size, encode size, and content
rectangle are also not yet represented as four-stage negotiation state.

### 2.5 Codec, color, and rate controls

The inherited video configuration already includes:

- H.264;
- HEVC capability/advertisement mode;
- AV1 capability/advertisement mode;
- hardware encoder probing;
- Vulkan Video rate-control mode;
- maximum client-requested bitrate;
- FEC percentage;
- packet-size configuration;
- encoder-specific quality and rate-control settings.

The Vulkan backend already recognizes VBR mode `vk_rc_mode = 4` and clears
`rc_min_rate` so it is not pinned to the target. The current session still opens
with one fixed bitrate; there is no backend-neutral runtime bitrate-update
interface or adaptive controller.

The generic video path already contains HEVC Main10 and AV1 Main 10-bit profile
selection, P010 encoder input, HDR metadata signaling, and H.264/HEVC/AV1 open
probes. The SteamOS PipeWire path recognizes 10-bit RGB formats, BT.2020, and
SMPTE ST 2084/PQ metadata. These are implemented components, not proof that the
SteamOS HDR transaction or AV1 hardware matrix has passed end to end.

The new work must centralize policy around these existing controls. It must not create a parallel encoder stack.

### 2.6 Latency protection, input, and diagnostics

The merged bounded pipeline already includes:

- an edge-preserving bounded input queue;
- coalescing for motion-like input;
- bounded capture and encoder work queues;
- bounded network producer backpressure;
- fixed-memory latency rings;
- IDR reason accounting;
- aggregated Web status;
- one final JSON report instead of per-event disk writes;
- the packaged Input Latency Visualizer.

The existing touch-port transform maps encoded fit geometry back to the source,
and the Vulkan path scales the cursor into the fitted rectangle. The policy for
input in pillarbox/letterbox margins and acceptance across all custom aspect
ratios remains incomplete.

Web status and the final session JSON expose source origin, requested and
negotiated FPS, observed source/encode FPS, capture/encode/network age,
bounded-queue counters, IDR reasons, input counters, and Artifact commit. They do
not yet expose one canonical requested/selected/active/observed snapshot,
codec/profile selection reasons, active color state, or adaptive bitrate state.

These counters and queues are the feedback source for later quality optimization.

## 3. Required end state

A Moonlight or Artemis client may request any safe width, height, FPS, codec set, bitrate ceiling, and HDR mode supported by the protocol. SteamShine must resolve that request into an explicit negotiated stream without relying on the physical monitor’s current geometry.

The end-to-end state must distinguish:

```text
requested  -> what the client asked for
selected   -> what SteamShine decided before startup
active     -> what capture and encoder actually opened
observed   -> what runtime counters and the client report show
```

A request is successful only when the selected and active state are observable. Silent substitution is not acceptable.

## 4. Goal order

### Goal G0 — PR #12 baseline freeze and documentation cleanup

Deliverables:

- keep this roadmap as the canonical project plan;
- keep `STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md` as the detailed design;
- keep `CODEX_GOAL_MODE_STREAMING.md` as the execution contract;
- remove superseded handoff and pre-implementation planning documents from the active documentation set;
- retain implementation records and configuration reference documents;
- record the PR #12 merge SHA, validated head, successful CI run, matching
  Artifact, and rollback Artifact/commit.

Exit conditions:

- docs-only CI passes;
- no active documentation index links to deleted plans;
- the next implementation branch starts from current `master`.

### Goal G1 — canonical request and negotiation model

Do not add another launch/session object. Extend or wrap the existing `launch_session_t`, display-device request, virtual-session request, and video configuration with one canonical negotiation snapshot.

Required fields:

- paired client ID and capability signature;
- requested width and height;
- requested refresh as integer FPS plus a future-proof millihertz/rational representation;
- requested HDR intent;
- requested/allowed codecs;
- requested bitrate ceiling;
- selected source origin;
- selected canvas geometry and refresh;
- selected encode geometry and frame rate;
- selected codec, profile, bit depth, chroma, and color space;
- selected bitrate envelope;
- fallback reason;
- active capture and encoder state;
- observed frame, queue, bitrate, loss, and latency values.

Exit conditions:

- the status API exposes requested, selected, active, and observed values separately;
- existing 1080p60 SDR behavior does not change;
- no duplicate state model owns the same settings.

### Goal G2 — arbitrary safe resolution and FPS

Resolution and FPS handling must be source-aware.

#### Owned private Gamescope

Preferred path:

- create the Gamescope canvas at the normalized Moonlight request;
- preserve arbitrary aspect ratios;
- require encoder-compatible dimensions;
- keep exact requested refresh when representable;
- use the existing DMA-BUF/Vulkan path.

A retained owned session may be reused only when its canvas, HDR intent, GPU, and source identity remain compatible. A request that requires a different canvas must not silently reuse an incompatible session.

#### Physical KDE desktop

Preferred path:

- use the existing KScreen/display-device path with an exact registered pixel size when available;
- otherwise choose the registered physical size with the shortest squared width/height distance;
- choose the nearest safe refresh for that size;
- restore the prior mode and scale;
- reserve an owned virtual session for cases where no capturable physical KDE output is available.

#### Existing Game Mode Gamescope

The resident Gamescope session is not owned and must not be restarted to satisfy a client request. When its source canvas differs from the requested encode geometry:

- preserve the resident session;
- use the existing video scaling/composition path;
- preserve aspect ratio through fit/letterbox by default;
- expose both source canvas and encoded output geometry;
- map absolute input against the visible content rectangle;
- never claim that the resident canvas itself changed.

Normalization policy:

- enforce width/height bounds, multiplication overflow, coded extent, codec
  alignment, YUV 4:2:0 even dimensions, probed pixel rate, GPU/client capability,
  administrator ceiling, VRAM/buffer budget, and Gamescope/display limits;
- reject zero, negative, absurd, or overflow-prone values;
- treat `auto` odd dimensions as a minimal reported alignment and
  `require_exact` as rejection when exact representation is impossible;
- preserve 16:9, 16:10, ultrawide, phone-native, and custom aspect ratios;
- do not change resolution mid-stream in the first implementation;
- prefer bitrate reduction before geometry reduction during congestion.

Exit conditions:

- custom resolutions within safe limits work on owned virtual sessions;
- 30–240 FPS requests are selected or rejected with a stable reason;
- physical unsupported modes route to a safe virtual fallback when policy permits;
- requested/selected/active geometry is visible in the SteamShine UI;
- touch and pointer mapping remain correct with letterbox/pillarbox.

### Goal G3 — H.264 / HEVC / AV1 SDR codec policy

Use the existing H.264, HEVC, and AV1 encoder implementations, advertisement,
and open probes. Candidate selection must consider client advertisement, host
open result, geometry, rational FPS, pixel rate, bit depth, encode/decode
latency, power, prior success, network class, and administrator policy.

Policy values are `auto`, `h264`, `hevc`, and `av1`. Quality/limited-bandwidth
preference may rank AV1 before HEVC before H.264; compatibility/recovery ranks
H.264 before HEVC before AV1. AV1 is never an unconditional default. Manual
selection either rejects strictly or falls back with a stable reason according
to explicit policy.

Initial codec/color targets:

- H.264 8-bit SDR;
- HEVC Main 8-bit SDR;
- HEVC Main10 SDR;
- AV1 Main 8-bit SDR;
- AV1 Main10 SDR where the existing protocol and probe path support it.

Codec remains fixed mid-stream. H.264 remains the recovery codec and unexpected
software fallback is prohibited.

Exit conditions:

- client and host probe results jointly determine candidates;
- codec/profile/bit depth and selection/fallback reason are visible;
- H.264 and HEVC SDR hardware gates pass;
- AV1 remains `hardware acceptance pending` until its matching Artifact passes.

### Goal G4 — end-to-end HDR10

HDR must reuse existing client HDR intent, display-device HDR policy, Gamescope HDR option, video color-space handling, and 10-bit encoder paths.

HDR activation requires all of the following:

1. the client requests and advertises HDR;
2. the selected source can produce an HDR-capable canvas;
3. Gamescope or the physical display is configured for HDR as appropriate;
4. capture supplies valid color primaries, transfer function, range, and bit-depth metadata;
5. the selected encoder supports the required 10-bit profile;
6. GameStream signaling is consistent with the encoded stream;
7. the client confirms or demonstrates correct HDR rendering in hardware acceptance.

Policy modes:

- `off`: always SDR;
- `auto`: use HDR only when every gate passes, otherwise fall back to SDR;
- `require`: reject startup if every HDR gate does not pass.

Do not equate HEVC Main10 with active HDR. Record codec, profile, bit depth, transfer function, primaries, and HDR-active state separately.

Exit conditions:

- SDR remains the safe fallback;
- HDR mismatch cannot produce washed-out or incorrectly signaled output without a visible error;
- HEVC Main10 SDR and HEVC Main10 HDR are reported distinctly;
- HDR is verified first on one known-good display/client pair before expanding the matrix.

### Goal G5 — VBR envelope and Adaptive Bitrate

VBR and Adaptive Bitrate are separate features. Encoder VBR varies output with
scene complexity inside one rate-control envelope. Adaptive Bitrate changes the
target or maximum in response to network state.

The formal envelope contains minimum, initial, target/average, maximum, peak,
and VBV buffer values. Reuse Vulkan `rc_mode=4` and the existing
`rc_min_rate=0` behavior; do not add a parallel rate-control system.

Build on the merged queue and latency instrumentation. Do not add a second telemetry system.

Feedback inputs:

- client loss reports already available in the GameStream path;
- RTT and RTT growth when available;
- server socket output queue;
- ordered network queue age/depth;
- capture and encode frame age;
- dropped/coalesced frame counters;
- IDR requests and reasons;
- optional Artemis decode/render telemetry when available.

Initial controller:

- sample every 500 ms;
- aggregate over 2 seconds;
- reduce 15–25% on persistent loss, queue growth, or RTT growth;
- reduce about 10% for isolated repeated loss;
- increase at most 3–5% per clean window;
- hysteresis before direction reversal;
- clamp to client, administrator, encoder, codec/pixel-rate, profile, and learned
  network-class ceilings;
- keep audio/protocol/FEC overhead outside the encoder video target;
- change bitrate before considering a lower geometry on the next connection.

If Vulkan Video cannot safely change rate control at runtime:

- do not recreate the encoder repeatedly;
- retain the stable current target;
- record the limitation;
- learn a lower starting bitrate for the next connection and network class.

The encoder abstraction must eventually report
`supports_runtime_bitrate_update()` and return a typed result from a bounded
`apply_bitrate_envelope()` operation at a frame boundary. Failed updates retain
the previous target and must not enter a retry or encoder-recreation loop.

Exit conditions:

- no unbounded queue growth under sustained motion;
- frame pacing remains smooth at stable network capacity;
- congestion reduces bitrate before visible long-term latency accumulates;
- recovery is gradual and does not oscillate;
- LAN, Wi-Fi, and remote/VPN results are stored separately.

### Goal G6 — per-client profiles and Stream Negotiation UI

Profiles are defaults and envelopes, not replacements for the actual Moonlight request.

Initial profiles:

- generic custom request;
- generic 4K60;
- optional 4K120;
- Y700 2560x1600 variants;
- Y700 3040x1904@90 exact-match variant;
- iPhone 16 Plus landscape with safe area;
- Steam Deck LCD;
- Steam Deck OLED.

Selection priority:

1. explicit per-client override;
2. exact client request and advertised capability;
3. previous successful profile for the paired client/network class;
4. capability signature;
5. device-name hint;
6. generic safe profile.

Exit conditions:

- device marketing names never override actual capabilities;
- safe-area and content rectangle are persisted per client/orientation;
- requested/selected/observed results can be compared in the UI.

Add the Stream Negotiation page to the existing SteamShine facade; do not add a
second API server or polling loop. SteamShine UI failure must not stop a stream,
and the upstream Sunshine compatibility route remains the configuration recovery
path.

### Goal G7 — final hardware acceptance and release

Required matrix, as hardware is available:

| Dimension | Cases |
| --- | --- |
| Geometry | 1280×720, 1920×1080, 2560×1440, 2560×1600, 3040×1904, 3440×1440, 3840×2160, portrait/custom aspect |
| FPS | 30, 59.94, 60, 90, 120, 144, 165, 240 |
| Codec/color | H.264 SDR 8-bit, HEVC Main SDR, HEVC Main10 SDR, AV1 Main 8-bit SDR, HEVC Main10 HDR, AV1 Main10 HDR |
| Source | owned private, attached stock Gamescope, physical KWin |
| Network | Ethernet, 5 GHz Wi-Fi, 6 GHz Wi-Fi where available, controlled loss/RTT/bandwidth reduction and recovery |

Also cover Y700 native variants, iPhone 16 Plus, Steam Deck LCD/OLED,
remote/VPN network classes where available, and:

- audio, keyboard, mouse, gamepad, touch, and rotation;
- continuous-input latency test;
- ten reconnects;
- explicit cleanup, rollback, and service restart;
- streaming with SteamShine and backup Sunshine UIs open.

Unavailable hardware must be marked `not tested`, never `passed`.

## 5. Independent implementation PRs

This documentation change is merged separately to establish the source of truth.

Never implement the roadmap as one combined feature PR. Each PR starts from current
`master`, has independent CI, a matching Artifact, hardware acceptance, and a
recorded rollback:

| PR | Branch | Title | Primary scope |
| --- | --- | --- | --- |
| A | `feat/stream-negotiation-state` | `refactor(stream): add canonical Moonlight negotiation state` | request trace, four-stage snapshot, rational FPS, codec mask, bitrate envelope, API/JSON |
| B | `feat/arbitrary-geometry-refresh` | `feat(display): honor safe arbitrary client geometry and refresh` | source-aware geometry, pixel rate, alignment, retained key, content rectangle and input mapping |
| C | `feat/probed-codec-policy` | `feat(video): select H.264 HEVC and AV1 from probed capabilities` | codec/profile matrix, client capability, host probe, H.264 recovery, SDR acceptance |
| D | `feat/hdr10-negotiation` | `feat(hdr): validate end-to-end HDR10 streaming` | HDR gates, 10-bit capture/P010, Main10, signaling, `off/auto/require` |
| E | `feat/adaptive-bitrate` | `feat(stream): add bounded adaptive bitrate control` | VBR envelope, runtime adapter, controller, hysteresis/cooldown, learning |
| F | `feat/stream-profiles-ui` | `feat(web): expose stream negotiation and client profiles` | negotiation page, policies, per-client/network profile, reset/rollback controls |

Do not mix unrelated GPU-control, terminal, local-presenter, or Web-style work
into these PRs.

## 6. Merge gates

Each implementation PR may be merged only when:

- full CI is green for the reviewed head;
- the delivery Artifact SHA and commit match;
- 1080p60 SDR has no regression;
- that PR's scoped unit/integration gates pass;
- its scoped hardware cases are either passed or explicitly `not tested` where
  the PR policy permits deferral;
- bounded latency remains effective under a 60-second continuous-input test;
- no software encoder fallback occurs unexpectedly;
- no second Steam or unnecessary second Gamescope is created;
- rollback to the last verified Artifact is recorded.

## 7. Prohibited shortcuts

- a second protocol server beside the existing GameStream server;
- duplicated pairing, application, configuration, audio, input, capture, or encoder stores;
- hardcoding a device name instead of honoring the client request;
- forcing HDR from panel marketing information alone;
- treating 10-bit video as proof of HDR;
- always forcing AV1 without runtime and client probes;
- silently changing aspect ratio;
- unbounded queues;
- per-frame SSD telemetry;
- repeated encoder recreation for bitrate changes;
- automatic termination of an existing Steam process;
- killing an attached Game Mode Gamescope;
- weakening DMA-BUF, GPU identity, UID, PID start-time, or ownership checks;
- merging with red CI or without a matching delivery Artifact.
- mid-stream resolution, FPS, codec, or HDR/SDR switching in the initial version;
- geometry reduction as the first congestion response;
- optical-flow frame interpolation;
- source-FPS-only migration from a physical/attached source to owned-private;
- a parallel encoder stack or automatic software-encoder fallback;
- attached stock Gamescope restart or forced mode/HDR changes;
