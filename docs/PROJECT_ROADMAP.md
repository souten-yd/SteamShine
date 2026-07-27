# SteamShine canonical project roadmap

- Status: active source of truth
- Baseline: `master` after PR #8 (`0273a1149348669e458cbb0aa1b83ad3020db720`)
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

### 2.1 Moonlight launch request

`rtsp_stream::launch_session_t` already carries:

- client width;
- client height;
- requested FPS;
- HDR intent;
- client identity;
- application ID;
- audio and controller options.

`process.cpp` already exports the client width, height, FPS, and HDR intent to application preparation through `SUNSHINE_CLIENT_*` variables.

### 2.2 Physical display handling

The inherited display-device configuration already defines automatic resolution, automatic refresh rate, automatic HDR, and mode remapping concepts.

SteamShine also packages a KDE/KScreen preparation hook that:

- applies an exact physical mode matching the Moonlight width and height;
- chooses the nearest refresh for that exact size;
- restores the previous mode and scale on application exit;
- skips physical KScreen changes for an owned Gamescope virtual session.

### 2.3 Owned virtual display

The SteamOS virtual-session path already:

- normalizes requested width to 640–7680;
- normalizes requested height to 480–4320;
- normalizes requested FPS to 30–240;
- creates an owned headless Gamescope session;
- passes nested width, height, and refresh to Gamescope;
- passes `--hdr-enabled` when HDR is requested and Gamescope advertises support;
- captures the verified Gamescope PipeWire `Video/Source` over DMA-BUF;
- encodes on the selected AMD GPU through Vulkan Video;
- retains owned sessions for reconnect;
- refuses a second Steam singleton.

### 2.4 Codec and quality controls

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

The new work must centralize policy around these existing controls. It must not create a parallel encoder stack.

### 2.5 Latency protection and diagnostics

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

### Goal G0 — freeze the merged baseline and clean project context

Deliverables:

- keep this roadmap as the canonical project plan;
- keep `STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md` as the detailed design;
- keep `CODEX_GOAL_MODE_STREAMING.md` as the execution contract;
- remove superseded handoff and pre-implementation planning documents from the active documentation set;
- retain implementation records and configuration reference documents;
- record the last successful PR #8 CI run and rollback commit.

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

- use the existing KScreen/display-device path when the physical output advertises an exact pixel size;
- choose the nearest safe refresh for that exact size;
- restore the prior mode and scale;
- if no exact physical mode exists, prefer an owned virtual session rather than changing aspect ratio silently.

#### Existing Game Mode Gamescope

The resident Gamescope session is not owned and must not be restarted to satisfy a client request. When its source canvas differs from the requested encode geometry:

- preserve the resident session;
- use the existing video scaling/composition path;
- preserve aspect ratio through fit/letterbox by default;
- expose both source canvas and encoded output geometry;
- map absolute input against the visible content rectangle;
- never claim that the resident canvas itself changed.

Normalization policy:

- enforce protocol and encoder limits;
- reject zero, negative, absurd, or overflow-prone values;
- make odd-dimension handling explicit;
- preserve 16:9, 16:10, ultrawide, phone-native, and custom aspect ratios;
- do not change resolution mid-stream in the first implementation;
- prefer bitrate reduction before geometry reduction during congestion.

Exit conditions:

- custom resolutions within safe limits work on owned virtual sessions;
- 30–240 FPS requests are selected or rejected with a stable reason;
- physical unsupported modes route to a safe virtual fallback when policy permits;
- requested/selected/active geometry is visible in the SteamShine UI;
- touch and pointer mapping remain correct with letterbox/pillarbox.

### Goal G3 — end-to-end HDR

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

### Goal G4 — codec and processing-quality policy

Use the existing H.264, HEVC, and AV1 encoder implementations and probes.

Default selection order is conditional, not absolute:

```text
AV1 -> HEVC Main10 -> HEVC -> H.264
```

A codec is selected only when:

- the client advertises it;
- the selected hardware encoder opens successfully;
- the requested bit depth and HDR mode are supported;
- measured encode/decode latency is within the profile budget;
- the requested FPS and resolution are sustainable.

Counterargument to “always AV1”:

AV1 generally improves compression efficiency, but it may increase encode latency, decode latency, power use, or incompatibility on a particular client. Therefore AV1 is preferred only after runtime probe and hardware acceptance. H.264 remains the universal recovery codec.

Processing-quality work must strengthen existing controls:

- Vulkan Video tune and rate-control mode;
- encoder QP/quality controls;
- color conversion and scaling quality;
- IDR cadence and recovery accounting;
- frame-pacing and queue bounds;
- same-GPU DMA-BUF import;
- no software fallback unless explicitly enabled for diagnostics.

Exit conditions:

- selected codec reason is visible;
- unsupported or slow AV1 falls back cleanly;
- no codec change recreates the encoder repeatedly during a stable stream;
- quality changes remain within latency and power budgets.

### Goal G5 — communication quality and adaptive bitrate

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
- fast reduction on persistent loss, queue growth, or RTT growth;
- smaller reduction for isolated repeated loss;
- gradual increase after a clean interval;
- hysteresis before direction reversal;
- clamp to client, administrator, encoder, and profile ceilings;
- keep audio/protocol/FEC overhead outside the encoder video target;
- change bitrate before considering a lower geometry on the next connection.

If Vulkan Video cannot safely change rate control at runtime:

- do not recreate the encoder repeatedly;
- retain the stable current target;
- record the limitation;
- learn a lower starting bitrate for the next connection and network class.

Exit conditions:

- no unbounded queue growth under sustained motion;
- frame pacing remains smooth at stable network capacity;
- congestion reduces bitrate before visible long-term latency accumulates;
- recovery is gradual and does not oscillate;
- LAN, Wi-Fi, and remote/VPN results are stored separately.

### Goal G6 — profiles and per-client persistence

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

### Goal G7 — SteamShine-primary operation

SteamShine becomes the default management route after the streaming goals above pass their required gates.

Required behavior:

- SteamShine UI is the default route;
- upstream Sunshine UI remains enabled at the compatibility route;
- one shared backend and one service process;
- application/config/pairing changes are visible from both UIs;
- rollback is configuration-only where possible;
- active streaming is isolated from Web UI failures and heavy polling.

Exit conditions:

- localhost and LAN browser tests pass;
- pairing, applications, settings, monitor, terminal, and rollback pass on SteamOS hardware;
- two releases complete without a critical SteamShine UI regression before considering further hiding of upstream assets.

### Goal G8 — final hardware and release acceptance

Required matrix, as hardware is available:

- arbitrary custom width/height/FPS;
- 1080p60 baseline;
- 4K59.94/60;
- Y700 native variants;
- iPhone 16 Plus;
- Steam Deck LCD/OLED;
- SDR H.264, HEVC, and AV1;
- one verified HDR client/display path;
- owned virtual, physical desktop, and existing Game Mode source;
- Ethernet, Wi-Fi, and remote/VPN network classes;
- audio, keyboard, mouse, gamepad, touch, and rotation;
- continuous-input latency test;
- ten reconnects;
- explicit cleanup, rollback, and service restart;
- streaming with SteamShine and backup Sunshine UIs open.

Unavailable hardware must be marked `not tested`, never `passed`.

## 5. PR and commit strategy

This documentation change is merged separately to establish the source of truth.

The next implementation uses one draft umbrella PR so the complete negotiation path is reviewable together, but commits remain goal-scoped and bisectable:

```text
refactor(stream): add canonical negotiation snapshot
fix(steamos): honor arbitrary client geometry and refresh
feat(hdr): validate end-to-end HDR negotiation
feat(video): select probed codec and quality policy
feat(stream): add adaptive bitrate controller
feat(profile): persist per-client stream profiles
feat(web): expose requested selected active observed stream state
test(stream): add negotiation HDR quality and hardware gates
docs: record measured acceptance and rollback
```

Do not mix unrelated GPU-control, terminal, local-presenter, or Web-style work into the streaming PR.

## 6. Merge gates

The implementation PR may be merged only when:

- full CI is green for the reviewed head;
- the delivery Artifact SHA and commit match;
- 1080p60 SDR has no regression;
- arbitrary geometry/FPS works on an owned session;
- HDR auto fallback is safe;
- at least H.264, HEVC, and AV1 probe/fallback tests pass;
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
