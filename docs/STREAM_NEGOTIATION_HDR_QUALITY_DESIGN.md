# SteamShine stream negotiation, HDR, codec, and quality design

- Status: approved detailed design
- Parent roadmap: [`PROJECT_ROADMAP.md`](./PROJECT_ROADMAP.md)
- Baseline: `master` after PR #8
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
struct stream_geometry_t {
  uint32_t width {0};
  uint32_t height {0};
  uint32_t refresh_millihz {0};
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
  uint64_t maximum_bps {0};
};

struct encoder_selection_t {
  stream_codec_e codec;
  std::string profile;
  uint8_t bit_depth;
  bitrate_envelope_t bitrate;
  std::string backend;
  std::string render_node;
  std::string selection_reason;
};
```

The active encoder must report the opened codec/profile/bit depth and whether runtime rate changes are supported.

### 3.5 Full snapshot

```cpp
struct stream_negotiation_snapshot_t {
  uint64_t generation {0};
  std::string paired_client_id;
  std::string capability_signature;

  stream_geometry_t requested_geometry;
  stream_geometry_t selected_canvas;
  stream_geometry_t selected_encode;
  stream_geometry_t active_source;
  stream_geometry_t active_encode;

  stream_color_state_t requested_color;
  stream_color_state_t selected_color;
  stream_color_state_t active_color;

  std::vector<stream_codec_e> client_codecs;
  encoder_selection_t selected_encoder;
  encoder_selection_t active_encoder;

  session_origin_e source_origin;
  fit_mode_e fit_mode;
  content_rectangle_t visible_content;

  uint64_t client_bitrate_ceiling_bps {0};
  uint64_t administrator_bitrate_ceiling_bps {0};
  std::vector<std::string> fallback_reasons;
};
```

Prefer adding this snapshot to existing session/status ownership rather than creating another global manager.

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

Record the exact parser location and units in the PR description. Do not assume that every Moonlight implementation sends identical optional fields.

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

Odd dimensions must not be silently changed without reporting the selected dimensions and reason. Prefer a codec-specific align-up/down helper shared by all paths.

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
width + height + refresh + HDR intent + selected GPU + source identity
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

Display:

- client ID/name;
- requested geometry/FPS/HDR/bitrate;
- selected source and canvas;
- selected encode geometry/FPS;
- active capture/encoder state;
- codec/profile/bit depth/chroma/color space;
- HDR request/selection/active state;
- bitrate envelope, current target, actual rate;
- congestion state and last decision reason;
- queue depths and ages;
- p50/p95/p99 latency;
- fallback reasons;
- Artifact commit and service launch mode.

Controls:

- automatic/custom geometry policy;
- FPS ceiling;
- HDR `off/auto/require`;
- codec `auto/H.264/HEVC/AV1` with validation;
- administrator bitrate ceiling;
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

## 15. Failure and rollback

Feature flags must permit returning to the verified baseline:

```ini
steamos_session_source = owned_private
steamos_local_presentation = off
steamshine_hdr_policy = off
steamshine_codec_policy = auto
steamshine_adaptive_bitrate = false
```

Exact key names may be adjusted to existing configuration conventions; do not create duplicate synonymous keys.

Fallback order:

1. disable adaptive bitrate;
2. force H.264 SDR;
3. use the owned private session;
4. install the last verified Artifact;
5. use the upstream Sunshine UI backup to recover configuration.

Ownership and safety validation must never be weakened as a rollback mechanism.
