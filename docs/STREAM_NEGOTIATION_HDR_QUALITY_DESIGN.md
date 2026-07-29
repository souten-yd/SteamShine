# SteamShine stream negotiation, HDR, codec, and quality design

- Status: approved detailed design
- Parent roadmap: [`PROJECT_ROADMAP.md`](./PROJECT_ROADMAP.md)
- Baseline `master` / PR #12 merge:
  `8a946f1e6f3b6540b5d75bc258a2a1e7c6d1927a`.
- PR #12 validated head: `c0d5d61657a3895b586892d10756a7d89715af23`;
  successful SteamOS Runtime Build run `30459891199`.
- Scope: Moonlight/Artemis request handling, display selection, HDR, codec policy, frame pacing, adaptive bitrate, and observability

## 1. Design principle

The existing Sunshine-derived pipeline remains authoritative:

```text
Moonlight GameStream request
  -> nvhttp / RTSP launch session
  -> display preparation / SteamOS session selection
  -> application launch
  -> capture
  -> video conversion and encoder
  -> GameStream packetization and network send
```

SteamShine adds a canonical policy and diagnostics layer around this pipeline. It does not add a second server, second encoder stack, second pairing database, or separate application/configuration store.

## 2. Current code that must be reused

| Concern | Existing implementation to extend |
| --- | --- |
| Client launch request | `rtsp_stream::launch_session_t`, `nvhttp.cpp`, `rtsp.cpp` |
| Application environment | `process.cpp` and `SUNSHINE_CLIENT_*` variables |
| Physical resolution/refresh/HDR concepts | `config::video.dd`, `display_device::*` |
| KDE exact-mode preparation | `scripts/configure-steamos-client-display.py` |
| SteamOS source choice | `steamos_virtual_session.*`, `steamos_virtual_session_core.*` |
| Gamescope geometry/HDR | `gamescope_arguments()` |
| Physical/Portal/KWin capture | existing Linux capture backends |
| Gamescope PipeWire capture | `gamescope_source.*`, `pipewire_capture.*`, `gamescopegrab.cpp` |
| Encoding | `video.*`, `vulkan_encode.cpp`, VA-API and existing encoder probes |
| Codec availability | existing H.264/HEVC/AV1 probe and advertisement paths |
| Network/FEC/packet size | `stream.*`, configured `fec_percentage`, `packetsize` |
| Queue/latency diagnostics | merged bounded input/video/network pipeline |
| Web status | `/api/steamshine/v1/` facade and existing status snapshots |

Current implementation classification:

| Capability | Current state at the validated PR #12 head |
| --- | --- |
| NVHTTP width/height/integer FPS/HDR/client/app request | Implemented |
| RTSP viewport/FPS/FPS×100/bitrate/codec/dynamic-range parsing | Implemented |
| Physical, attached stock, retained owned, new owned route selection | Implemented; hardware acceptance differs by route |
| Owned width/height/FPS normalization and Gamescope arguments | Implemented for integer 640–7680, 480–4320, and 30–240 |
| Owned `--hdr-enabled` request | Implemented; end-to-end HDR acceptance pending |
| PipeWire Gamescope `requested_size` | Implemented |
| One-buffer latest-frame-wins DMA-BUF handoff | Implemented |
| Same-GPU producer/capture/Vulkan validation | Implemented |
| DMA-BUF lease and Vulkan completion-fence lifetime | Implemented |
| Vulkan fit/letterbox and cursor composition | Implemented; canonical content rectangle and full input acceptance pending |
| H.264/HEVC/AV1 advertisement and encoder open probe | Implemented |
| Vulkan VBR `rc_mode=4` and `rc_min_rate=0` | Implemented as startup configuration; runtime update absent |
| 10-bit RGB import and P010 encoder input components | Implemented; SteamOS HDR transaction acceptance pending |
| GameStream HDR control signaling | Inherited and implemented; source-to-client consistency gate pending |
| Web status and final session diagnostics JSON | Implemented for existing counters; canonical four-stage snapshot absent |
| Canonical requested/selected/active/observed ownership | Not implemented |
| Adaptive bitrate and per-client profile | Not implemented |

Before introducing a new type or subsystem, search these integration points and document why they cannot be extended.

## 3. Canonical stream state

### 3.1 Required layers

The state model has four immutable or append-only stages.

```cpp
enum class negotiation_stage_e {
  requested,
  selected,
  active,
  observed,
};
```

- `requested`: client protocol values before policy.
- `selected`: validated decision before display/capture/encoder startup.
- `active`: values reported by opened display, capture, and encoder objects.
- `observed`: runtime counters, latency distributions, and client feedback.

Never overwrite requested values with selected values. A fallback must remain visible.

### 3.2 Geometry and refresh

```cpp
struct rational_rate_t {
  uint32_t numerator {0};
  uint32_t denominator {1};
};

struct stream_geometry_t {
  uint32_t width {0};
  uint32_t height {0};
  rational_rate_t frame_rate;
};
```

Compatibility fields may continue to expose integer FPS, but internal comparisons and display-mode selection should use millihertz or an exact rational where the protocol supplies one.

Helper conversions must:

- detect overflow;
- round only at an explicit boundary;
- preserve 59.94 versus 60.00 when known;
- expose when the client only supplied integer FPS.

### 3.3 HDR and color

```cpp
enum class hdr_policy_e {
  off,
  auto_select,
  require,
};

struct stream_color_state_t {
  bool hdr_requested {false};
  bool hdr_selected {false};
  bool hdr_active {false};
  uint8_t bit_depth {8};
  color_primaries_e primaries;
  transfer_function_e transfer;
  color_matrix_e matrix;
  color_range_e range;
  chroma_format_e chroma;
};
```

`codec=hevc`, `profile=main10`, `bit_depth=10`, and `hdr_active=true` are independent facts.

### 3.4 Codec and rate

```cpp
enum class stream_codec_e {
  h264,
  hevc,
  av1,
};

struct bitrate_envelope_t {
  uint64_t minimum_bps {0};
  uint64_t initial_bps {0};
  uint64_t target_bps {0};
  uint64_t maximum_bps {0};
  uint64_t peak_bps {0};
  uint64_t vbv_buffer_bits {0};
};

struct encoder_selection_t {
  stream_codec_e codec;
  std::string profile;
  uint8_t bit_depth;
  bitrate_envelope_t bitrate;
  std::string backend;
  std::string render_node;
  bool runtime_rate_update_supported {false};
  std::string selection_reason;
};
```

The active encoder must report the opened codec/profile/bit depth and whether runtime rate changes are supported.

### 3.5 Full snapshot

```cpp
struct stream_request_t {
  stream_geometry_t geometry;
  uint64_t client_bitrate_ceiling_bps {0};
  uint32_t client_codec_mask {0};
  bool hdr_requested {false};
  std::string client_id;
  int application_id {0};
};

struct stream_negotiation_snapshot_t {
  uint64_t generation {0};
  stream_request_t requested;
  stream_selection_t selected;
  stream_active_t active;
  stream_observed_t observed;
  std::vector<std::string> fallback_reasons;
};
```

These are conceptual shapes, not an instruction to duplicate existing enums or
session types. Add the minimum fields to existing launch/stream/status ownership.
Prefer adding this snapshot to existing session ownership rather than creating
another global manager.

## 4. Request ingestion

### 4.1 Trace before changing

Codex must first trace all current launch and RTSP fields that contribute to:

- width;
- height;
- FPS or refresh;
- bitrate;
- codec capabilities;
- HDR capability and request;
- client unique ID;
- client display mode or SOPS flags.

The current trace is:

| Value | Current parser and units |
| --- | --- |
| Launch width/height/FPS | `nvhttp.cpp::make_launch_session()`, `mode=WIDTHxHEIGHTxFPS`, pixels and integer FPS |
| HDR request | `nvhttp.cpp::make_launch_session()`, `hdrMode`, boolean intent |
| Client ID / app ID | `nvhttp.cpp::make_launch_session()`, `uniqueid` string and integer `appid` |
| Stream width/height | `rtsp.cpp::cmd_announce()`, `clientViewportWd/Ht`, pixels |
| Stream FPS | `maxFPS`, integer FPS; `clientRefreshRateX100`, hundredths of Hz when within 1% |
| Client bitrate | `bw.maximumBitrateKbps` and optional `configuredBitrateKbps`, Kbps; the latter is adjusted for FEC/audio/overhead |
| Selected codec | RTSP `bitStreamFormat`: 0 H.264, 1 HEVC, 2 AV1 |
| HDR/bit depth request | RTSP `dynamicRangeMode`: 0 8-bit, 1 10-bit, plus `encoderCscMode` and chroma |
| Host codec capability | `nvhttp.cpp` advertisement from the existing encoder probe and configured HEVC/AV1 modes |
| Reconnect/resume | `/resume` creates a new `launch_session_t`; retained owned Gamescope is reused only when the current compatibility checks pass |

The first implementation PR must re-verify this trace against its current base
and record any client-specific optional fields. Do not assume every Moonlight or
Artemis version sends identical values.

### 4.2 Validation

Initial safe limits remain compatible with the merged implementation:

```text
width:  640..7680
height: 480..4320
fps:    30..240
```

These are service safety limits, not a fixed profile list.

Additional checks:

- multiplication and buffer-size overflow;
- encoder maximum dimensions;
- codec alignment constraints;
- zero and negative values;
- total pixel-rate budget;
- GPU capability;
- configured administrator ceiling.

The dimensions alone never authorize an extreme combination. Validate
`width * height * FPS` against a probed pixel-rate budget, encoder coded extent,
GPU capability, decoder capability, Gamescope/display capability, VRAM/buffer
budget, and administrator ceilings. For example, 7680×4320@240 is not safe merely
because each individual value is in range.

Odd-dimension policy:

- `auto`: minimally adjust to the codec and 4:2:0 alignment, preserving the
  requested value and recording the selected value and reason;
- `require_exact`: reject when the exact request cannot be represented.

Odd dimensions must not be silently changed. Prefer a codec-specific alignment
helper shared by all paths.

## 5. Source-aware geometry resolution

### 5.1 Decision inputs

```cpp
struct source_geometry_input_t {
  stream_geometry_t request;
  session_origin_e origin;
  std::vector<display_mode_t> physical_modes;
  std::optional<stream_geometry_t> resident_gamescope_canvas;
  std::optional<stream_geometry_t> retained_owned_canvas;
  bool allow_owned_virtual_fallback;
  bool allow_scaling;
};
```

### 5.2 Decision output

```cpp
struct source_geometry_decision_t {
  stream_geometry_t canvas;
  stream_geometry_t encode;
  fit_mode_e fit_mode;
  content_rectangle_t content_rectangle;
  bool recreate_owned_session {false};
  bool use_physical_mode_change {false};
  std::string reason;
};
```

### 5.3 Owned private Gamescope

Use the normalized request as the canvas whenever the encoder and Gamescope support it.

Retained-session compatibility key:

```text
width + height + rational refresh + HDR intent + selected GPU/render node
+ source identity + capture pixel-format requirements
```

If the key changes:

- do not silently reuse the retained session;
- if no game must be preserved, replace only the owned session;
- if preserving an active game is required, keep the current canvas and expose scaling/fallback or reject according to policy;
- never terminate an unrelated Steam or Gamescope process.

Gamescope command generation must continue to inspect `gamescope --help` and fail closed when required flags are unavailable.

### 5.4 Physical KDE desktop

Unify policy around existing display-device concepts and the packaged KScreen helper.

Preferred order:

1. exact requested width/height with nearest refresh;
2. exact requested width/height at a lower supported refresh only when policy permits;
3. owned virtual session fallback;
4. explicit rejection.

Do not silently choose another aspect ratio. Do not leave the physical mode changed after the application or stream exits.

Avoid two independent pieces of code both deciding modes. The long-term target is one pure mode-selection helper used by the display-device path and the KScreen command wrapper.

### 5.5 Existing Game Mode Gamescope

The resident session is not owned. Its local output and canvas are authoritative.

If requested encode geometry differs:

- keep the resident process untouched;
- scale in the existing video conversion path;
- use `fit` by default;
- compute and expose the visible content rectangle;
- map touch/absolute pointer coordinates to that rectangle;
- drop input in letterbox/pillarbox margins or clamp according to a documented policy;
- never report that the resident canvas changed.

Treat resident canvas, PipeWire producer size, selected encode size, and visible
content rectangle as separate facts. Reuse Gamescope `requested_size` for
producer-side downscale when supported. A request larger than the source must
follow an explicit `upscale`, `owned fallback`, or `reject` policy.

Changing or replacing the resident Game Mode session requires a separate explicit migration workflow and is outside this streaming PR.

## 6. FPS and frame pacing

### 6.1 Selection

FPS selection must consider:

- requested FPS;
- source refresh or production cadence;
- encoder maximum pixel rate;
- client decoder capability;
- administrator ceiling;
- current source type.

For owned sessions, create Gamescope at the selected refresh. For fixed resident sources, encode at a sustainable cadence without manufacturing an unbounded backlog.

### 6.2 Pacing rules

- no unbounded frame queue;
- capture keeps latest work at its configured bound;
- encoder keeps latest unsubmitted frame at its configured bound;
- packet ordering remains correct;
- a slow sender applies bounded backpressure;
- frame age, not just queue count, is measured;
- do not call `vkQueueWaitIdle` or `vkDeviceWaitIdle` in the normal frame path;
- avoid timer drift by scheduling against a monotonic target timeline.

### 6.3 Observability

Expose:

- requested FPS;
- selected refresh;
- source observed FPS;
- encoded FPS;
- rendered/client FPS when available;
- frame-time p50/p95/p99;
- duplicate/skipped/dropped frame reasons;
- queue age and depth.

## 7. HDR negotiation

### 7.1 Gate sequence

```text
client request/capability
  -> source HDR capability
  -> display/Gamescope HDR activation
  -> capture metadata
  -> 10-bit conversion path
  -> encoder profile
  -> GameStream HDR signaling
  -> client-render acceptance
```

Every gate returns a stable result and reason.

### 7.2 Owned Gamescope HDR

Reuse `enable_hdr` and the existing `--hdr-enabled` generation.

Add validation that:

- Gamescope advertises the option;
- selected Vulkan device supports the required format path;
- PipeWire supplies compatible format/color metadata;
- Vulkan conversion and encoder agree on bit depth and color space.

For `auto`, failure returns to SDR before application launch where possible. For `require`, startup fails with a clear error.

### 7.3 Physical HDR

Reuse `config::video.dd.hdr_option` and the existing display-device preparation flow.

The KScreen helper must not invent HDR control independently if the display-device subsystem already owns it. If KDE-specific support is missing, add an adapter behind the same display preparation interface.

### 7.4 Resident Game Mode HDR

Do not toggle a resident Gamescope session without explicit ownership and user intent.

- If the resident source is already HDR and the client supports it, negotiate HDR.
- If the resident source is SDR, `auto` remains SDR.
- `require` fails rather than restarting or mutating the resident session.

### 7.5 Metadata consistency

Before first encoded frame, assert consistency among:

- capture pixel format;
- bit depth;
- primaries;
- transfer function;
- matrix;
- range;
- encoder profile;
- GameStream HDR flags.

On mismatch, fail or fall back before sending incorrectly signaled frames.

### 7.6 External Gamescope HDR dependency

SteamShine cannot declare owned/attached Gamescope HDR complete until the
Gamescope PipeWire producer provides a consistent 10-bit transaction. Track this
dependency separately from the SteamShine runtime PR:

- `xBGR_210LE` / `XBGR2101010` DMA-BUF export;
- BT.2020 primaries;
- SMPTE ST 2084/PQ transfer;
- HDR LUT and paint-path PQ EOTF consistency;
- HDR state-change renegotiation;
- compatible modifiers and same-GPU DMA-BUF import.

Do not mix an unrelated Gamescope patch or Artifact into a SteamShine runtime PR.
Never accept a washed-out hybrid stream where pixels and signaling disagree.

## 8. Codec policy

### 8.1 Existing encoders are authoritative

Do not create SteamShine-specific H.264/HEVC/AV1 encoders. Extend the existing probe and selection path.

### 8.2 Candidate scoring

For each client/host-compatible codec, score:

- compression efficiency;
- measured encode latency;
- client decode latency;
- maximum sustainable resolution/FPS;
- HDR/10-bit support;
- power use;
- historical success for this client/network class;
- compatibility priority.

Suggested preference before measurement:

```text
AV1 > HEVC Main10 > HEVC > H.264
```

Suggested recovery priority:

```text
H.264 > HEVC > AV1
```

The selected order depends on the goal: quality/efficiency versus maximum compatibility.

Supported policy values are `auto`, `h264`, `hevc`, and `av1`. A manual codec
uses either strict rejection or explicitly enabled fallback; it never silently
switches codecs. Candidate profiles are taken from the existing protocol,
FFmpeg, and backend probe names rather than invented duplicate enums.

### 8.3 AV1

AV1 is preferred for high compression only when:

- the client explicitly advertises AV1;
- the hardware encoder opens;
- decode latency is acceptable;
- the selected resolution/FPS is supported;
- hardware acceptance shows no frame-pacing regression.

Do not infer AV1 streaming support from a device marketing specification alone.

### 8.4 Mid-stream changes

Initial implementation:

- codec remains fixed during a stream;
- bitrate may change when the backend supports it;
- geometry remains fixed during a stream;
- reconnect may select a different codec based on the previous report.

This avoids decoder resets and repeated encoder recreation.

## 9. Bitrate envelope

Encoder VBR and Adaptive Bitrate are distinct. VBR varies frame output according
to scene complexity within the configured envelope. Adaptive Bitrate changes the
target/maximum envelope based on network observations. SteamShine already has a
Vulkan startup VBR path for `vk_rc_mode=4`; it does not yet have runtime updates.

### 9.1 Inputs

```text
profile maximum
client requested ceiling
administrator maximum bitrate
encoder maximum
pixel-rate safety ceiling
historical network-class ceiling
```

```cpp
effective_max = min(all applicable ceilings);
initial = clamp(profile_or_model_start, minimum, effective_max);
```

Applicable ceilings include client request, administrator policy, encoder,
codec/pixel-rate, profile, and learned network class.

For an unknown custom request, derive the initial estimate from pixel rate, codec efficiency class, HDR/bit depth, and a conservative network class. Clamp it; never trust an unbounded client value.

### 9.2 Separate video target from overhead

The encoder video target excludes:

- audio;
- transport headers;
- encryption overhead;
- FEC;
- retransmission/recovery traffic.

The network budget includes all of them.

## 10. Adaptive bitrate

### 10.1 Reuse existing metrics

Add fields to the existing fixed-memory diagnostics rather than a separate collector.

Required samples:

- target and actual bitrate;
- packet loss;
- FEC/recovery activity;
- RTT and variation when available;
- socket output queue;
- ordered network queue age/depth;
- capture-to-encode age;
- encoder completion age;
- frame drops by reason;
- IDR request reason;
- optional client decode/render queue.

### 10.2 Controller state

```cpp
enum class congestion_state_e {
  unknown,
  clean,
  warning,
  congested,
  recovering,
};
```

Controller defaults:

- 500 ms sample interval;
- 2 s decision window;
- 20% reduction for persistent congestion;
- 10% reduction for repeated isolated loss;
- no more than 5% increase per clean 2 s interval;
- minimum clean windows before increase;
- hysteresis before reversing direction;
- cooldown after IDR/reconnect;
- bounded target within the envelope.

### 10.3 Backend rate updates

Probe whether the active encoder can update rate control without recreation.

- Supported: apply at a frame boundary and record requested/applied values.
- Unsupported: keep the current encoder stable and save a learned next-session start value.
- Failed update: retain the previous target and report the failure; do not loop on recreation.

The existing encoder abstraction should expose behavior equivalent to:

```cpp
bool supports_runtime_bitrate_update();
rate_update_result_t apply_bitrate_envelope(const bitrate_envelope_t &envelope);
```

Apply supported changes at a frame boundary and record requested values, applied
values, and the backend/driver result.

## 11. Quality and processing improvements

Improvements are accepted only when they do not violate latency and stability budgets.

Evaluate existing controls in this order:

1. correct geometry, color, and frame pacing;
2. correct low-latency rate control;
3. AV1/HEVC efficiency;
4. scaling filter quality;
5. chroma and bit-depth policy;
6. encoder quality/tune controls;
7. optional higher-quality desktop modes.

Do not use global “maximum quality” settings that increase buffering. Quality is measured against:

- encode latency;
- end-to-end latency;
- frame-time variance;
- bitrate;
- objective image samples where available;
- client visual acceptance;
- GPU power and VRAM.

## 12. SteamShine Web UI

SteamShine is the primary UI. The upstream Sunshine UI remains the backup.

Add one Stream Negotiation page or section using the existing facade.

Display four explicit sections:

- Requested: client ID/name, geometry/FPS, bitrate ceiling, codec capabilities,
  and HDR request.
- Selected: source origin, canvas/capture/encode sizes, fit policy, content
  rectangle, codec/profile/bit depth, HDR selection, bitrate envelope, and
  fallback reasons.
- Active: actual source geometry, PipeWire format, encoder/backend,
  codec/profile/color metadata, and runtime rate-update support.
- Observed: source/encode FPS, duplicate/drop counters, target/actual bitrate,
  congestion state, loss/RTT, queue age/depth, and latency percentiles.

Controls:

- geometry `exact/fit/virtual fallback`;
- FPS ceiling `auto/custom`;
- HDR `off/auto/require`;
- codec `auto/H.264/HEVC/AV1` with validation;
- bitrate `automatic/custom ceiling`;
- quality `low-latency/balanced/quality` using existing encoder settings;
- quality/latency preset implemented as existing setting values, not a parallel encoder configuration;
- reset learned client/network profile.

The backup Sunshine UI must continue to show and edit the shared underlying configuration where supported.

## 13. Persistence

Persist only low-rate configuration and final aggregate reports.

Per paired client and network class, store:

- last successful geometry/FPS;
- selected codec/profile;
- HDR success/failure reason;
- learned starting bitrate;
- safe-area/content rectangle override;
- last hardware acceptance timestamp and Artifact SHA.

Do not persist every 500 ms sample. Keep active samples in bounded memory and write one report when the final stream ends.

Use atomic replace, schema versioning, size bounds, and owner-only permissions.

## 14. Tests

### 14.1 Unit tests

- boundary width/height/FPS validation;
- odd/alignment normalization with reason;
- 59.94/60 distinction where supplied;
- physical exact mode selection;
- physical unsupported mode -> virtual fallback;
- resident Game Mode fixed canvas -> scaled encode geometry;
- content-rectangle and touch mapping;
- retained session compatibility key;
- HDR off/auto/require gates;
- Main10 SDR distinct from HDR;
- AV1 probe failure fallback;
- bitrate envelope clamping;
- adaptive controller reduction, recovery, hysteresis, cooldown;
- runtime-rate-update unsupported behavior;
- requested/selected/active state not overwritten.

### 14.2 Integration tests

- fake Moonlight launch requests across custom geometries;
- fake display modes and virtual fallback;
- fake Gamescope help with and without HDR;
- fake capture metadata mismatch;
- fake H.264/HEVC/AV1 probe matrix;
- slow network sender with bounded queue;
- congestion and recovery trace replay;
- reconnect uses learned start rate;
- SteamShine and backup Sunshine UI read shared settings;
- no active stream interruption when either UI route fails.

### 14.3 Hardware tests

Start with:

```text
1920x1080@60 SDR H.264
```

Then:

```text
custom aspect ratio SDR
custom high FPS SDR
HEVC SDR
AV1 SDR
HEVC Main10 SDR
one verified HDR path
```

For every run record:

- request, selection, active state;
- capture and encode GPU;
- codec/profile/color;
- bitrate target/actual;
- loss/RTT/queue;
- p50/p95/p99 latency;
- frame pacing and drops;
- audio and input;
- CPU/GPU/VRAM/power;
- SSD writes;
- reconnect and cleanup.

Acceptance matrix:

| Dimension | Required cases |
| --- | --- |
| Geometry | 1280×720, 1920×1080, 2560×1440, 2560×1600, 3040×1904, 3440×1440, 3840×2160, portrait/custom aspect |
| FPS | 30, 59.94, 60, 90, 120, 144, 165, 240 |
| Codec/color | H.264 SDR 8-bit, HEVC Main SDR, HEVC Main10 SDR, AV1 Main 8-bit SDR, HEVC Main10 HDR, AV1 Main10 HDR |
| Source | `owned_private`, `attached_existing`, physical KWin |
| Network | Ethernet, 5 GHz Wi-Fi, 6 GHz Wi-Fi where available, controlled loss/RTT/bandwidth reduction and recovery |

During continuous motion, observed source FPS must be at least 90% of the lesser
of actual presented source FPS and selected capture FPS. Observed encode FPS must
be at least 90% of selected encode FPS. First frame must arrive within one second;
queues must not grow long-term; deterministic input must appear within one or two
frames; HDR signaling must not be false or washed out; no AMDGPU fault/reset or
unexpected service restart is allowed. Every result names the matching Artifact
and `BUILD_INFO.json`; unavailable cases are `not tested`.

## 15. Failure and rollback

Rollback must first use existing configuration keys where they already express
the required behavior:

```ini
steamos_session_source = owned_private
steamos_local_presentation = off
hevc_mode = 1
av1_mode = 1
vk_rc_mode = 2
max_bitrate = <verified fixed ceiling in Kbps>
```

`hevc_mode = 1` plus `av1_mode = 1` leaves the existing H.264 8-bit path as the
advertised recovery codec, which also prevents a Main10/HDR selection.
`vk_rc_mode = 2` is the existing Vulkan CBR setting. PR D and PR E must add one
HDR policy and one Adaptive Bitrate enable/disable control only if no existing
key can represent their semantics; their exact names must follow the current
configuration convention and must not duplicate `dd_hdr_option`, `hevc_mode`,
`av1_mode`, `vk_rc_mode`, or `max_bitrate`. `dd_hdr_option` is not a SteamOS
force-SDR switch: its documented implementation is display-device preparation
and currently applies to Windows.

Fallback order:

1. disable adaptive bitrate;
2. return to a fixed VBR/CBR target;
3. force H.264 SDR;
4. set HDR off;
5. use owned-private fixed 1080p60;
6. install the last verified Artifact;
7. use the upstream Sunshine UI backup to recover configuration.

Ownership and safety validation must never be weakened as a rollback mechanism.
