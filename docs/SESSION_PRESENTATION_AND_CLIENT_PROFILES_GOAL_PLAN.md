# SteamShine: Single-Gamescope session presentation and client profile Goal Plan

- Status: Approved implementation plan — execute through hardware acceptance
- Target branch: `feat/steamos-auto-virtual-display`
- Updated: 2026-07-26
- Primary host: SteamOS 3.8.16, AMD Radeon RX 9070 XT, `/dev/dri/renderD128`
- Related documents:
  - [`STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md`](./STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md)
  - [`STEAMOS_GAMESCOPE_PIPEWIRE_CAPTURE_PLAN.md`](./STEAMOS_GAMESCOPE_PIPEWIRE_CAPTURE_PLAN.md)
  - Existing client-adaptive display and bitrate design

## 1. Goal

Implement one canonical Gamescope session that can be reused for remote streaming and, when needed, local presentation without restarting Gamescope, Steam, or the game.

```text
                                 DMA-BUF
Steam / game -> one Gamescope Video/Source node
                         |                    |
                         |                    +-> optional local Vulkan presenter
                         |                        -> host Wayland display
                         |
                         +-> SteamShine PipeWire consumer
                             -> same-GPU Vulkan Video encoder
                             -> Moonlight / Artemis
```

The implementation must optimize for the following order:

1. low input-to-display latency;
2. no CPU frame copy;
3. no duplicate Steam or Gamescope process;
4. bounded GPU and memory overhead;
5. predictable reconnect and hotplug behavior;
6. high image quality within the client and network limits.

The completed feature must support:

- an existing SteamOS Game Mode Gamescope session;
- a retained SteamShine-owned private Gamescope session;
- cold creation of an owned headless Gamescope session;
- optional local mirroring of an owned headless session;
- 4K displays;
- Lenovo Legion Y700 / Legion Tab variants;
- iPhone 16 Plus;
- Steam Deck LCD and OLED.

## 2. Verified baseline that must not regress

The current private-session path has already produced live hardware evidence:

```text
captured_frames   = 2079
encoded_packets   = 2229
encoded_bytes     = 69585793
idr_frames        = 1
render_node       = /dev/dri/renderD128
capture_memory    = DMA-BUF
same_gpu          = true
```

The host PipeWire endpoint, owned Gamescope `Video/Source`, client-PID ownership resolution, DMA-BUF capture, Vulkan encoding, Moonlight video, audio, touch, and private-Xwayland Steam Big Picture have all been observed.

Every new implementation stage must preserve:

- `capture_memory=DMA-BUF`;
- capture and encoder on `/dev/dri/renderD128`;
- no software encoder fallback;
- no desktop capture fallback while an owned session is active;
- positive captured frame, packet, byte, and IDR counters;
- bounded in-memory telemetry without per-frame SSD writes.

## 3. Chosen session presentation architecture

### 3.1 Source selection policy

Add:

```ini
steamos_session_source = auto
steamos_local_presentation = auto
steamos_keep_session_alive = true
steamos_existing_gamescope_pid = 0
```

Allowed values:

```text
steamos_session_source:
  auto
  existing_gamescope
  owned_private

steamos_local_presentation:
  auto
  off
  mirror
```

`auto` selects in this order:

1. a uniquely verified current-user SteamOS Game Mode Gamescope source;
2. an already retained SteamShine-owned session;
3. a newly created SteamShine-owned headless Gamescope session.

Never silently select:

- another user's Gamescope;
- an ambiguous Gamescope candidate;
- a desktop portal or desktop Wayland source;
- a source on another GPU;
- a second Steam instance.

### 3.2 Existing Game Mode Gamescope

When attaching to an existing Game Mode session:

```text
process_owned = false
runtime_owned = false
local output  = resident Gamescope physical output
local presenter = disabled
```

SteamShine attaches only its PipeWire consumer and encoder. It must not:

- send signals to Gamescope;
- remove its runtime;
- restart Steam;
- create another Gamescope;
- create another local presenter.

This is the lowest-overhead path.

### 3.3 Retained owned private Gamescope

When an owned session is already `Ready`, validate and reuse:

- ownership marker;
- Gamescope PID and `/proc/<pid>/stat` start time;
- private runtime and Wayland socket;
- PipeWire client and object serial;
- selected DRM render node;
- Steam process location.

Reconnect must attach capture and encoding without replacing the Gamescope or Steam process.

### 3.4 New owned private Gamescope

Cold creation remains the current verified headless path:

```text
private XDG runtime
+ host PipeWire runtime
+ owned process group
+ private Xwayland
+ Gamescope Video/Source
```

After Moonlight disconnect:

```text
Streaming -> Ready
```

Do not terminate Gamescope or Steam. Explicit cancel or service stop performs owned cleanup.

### 3.5 Local mirror for an owned private session

When a physical display is connected and `steamos_local_presentation=auto|mirror`, attach a second consumer to the same PipeWire object serial:

```text
Gamescope DMA-BUF
  -> remote consumer -> Vulkan Video encoder
  -> local consumer  -> Vulkan image import -> host Wayland swapchain
```

The local presenter must:

- never encode or decode;
- never map full frames to CPU memory;
- accept only `SPA_DATA_DmaBuf` in production;
- verify same-GPU import;
- use a separate PipeWire core connection;
- keep only the latest pending frame;
- drop stale local frames rather than block remote capture;
- stop independently when the monitor or host Wayland display disappears.

## 4. Source and ownership data model

Add or refactor toward:

```cpp
enum class session_origin_e {
  none,
  owned_private,
  attached_existing,
};

enum class source_state_e {
  unavailable,
  discovered,
  verified,
  attached,
  disappeared,
};

struct gamescope_source_t {
  uint32_t node_id {PW_ID_ANY};
  uint64_t object_serial {SPA_ID_INVALID};
  uint32_t client_id {PW_ID_ANY};

  pid_t producer_pid {-1};
  uid_t producer_uid {static_cast<uid_t>(-1)};
  uint64_t producer_start_time {0};

  std::filesystem::path executable;
  std::string node_name;
  std::string node_description;
  std::string application_name;
  std::string media_class;
  std::string render_node;

  session_origin_e origin {session_origin_e::none};
};
```

Extend the status snapshot with:

```cpp
session_origin_e origin;
bool process_owned;
bool runtime_owned;
std::string source_description;
uint64_t producer_start_time;
bool local_presenter_active;
uint64_t local_presented_frames;
uint64_t local_dropped_frames;
std::string selected_profile_id;
std::string profile_selection_reason;
```

PID alone is not an identity. Revalidate PID, process start time, executable, UID, PipeWire client ID, and object serial before reuse.

## 5. Steam singleton policy

Do not attempt a normal second Steam launch in the same user session.

Classify the existing Steam location:

```cpp
enum class steam_instance_location_e {
  absent,
  inside_target_gamescope,
  outside_target_gamescope,
  unknown,
};
```

Inspect Steam, steamwebhelper, reaper, and pressure-vessel processes using:

- parent and cgroup chain;
- `XDG_RUNTIME_DIR`;
- `WAYLAND_DISPLAY`;
- `DISPLAY`;
- process start time.

Rules:

- inside target Gamescope: reuse and send the requested Steam URI;
- absent: launch Steam inside the target Gamescope;
- outside target Gamescope: do not launch a second instance; attach to its resident Game Mode Gamescope when possible, otherwise report `steam_instance_outside_target` and offer an explicit one-time migration action;
- unknown: fail safely and display diagnostics.

SteamShine must never automatically terminate the user's Steam process.

## 6. Client profile engine

### 6.1 Source of truth

Selection priority:

1. explicit per-client saved override;
2. exact width, height, refresh, codec, HDR, and decoder capability request;
3. previously successful profile for the paired client ID;
4. capability-signature profile;
5. device-name heuristic;
6. generic adaptive profile.

A device marketing name must not override real client capabilities.

The stream request remains authoritative. A profile supplies defaults, caps, fallback order, safe-area behavior, bitrate envelope, codec preference, and validation expectations.

### 6.2 Profile descriptor

```cpp
struct client_profile_t {
  std::string id;
  std::string display_name;

  uint32_t native_width;
  uint32_t native_height;
  uint32_t preferred_fps_millihz;
  uint32_t maximum_fps_millihz;

  std::vector<stream_geometry_t> geometry_fallbacks;
  std::vector<codec_e> codec_order;

  hdr_policy_e hdr_policy;
  fit_mode_e fit_mode;
  safe_area_policy_e safe_area_policy;
  chroma_policy_e chroma_policy;

  uint64_t bitrate_start_bps;
  uint64_t bitrate_min_bps;
  uint64_t bitrate_max_bps;

  uint32_t capture_queue_limit;
  uint32_t encoder_queue_limit;
  uint32_t network_queue_limit;
  bool latest_frame_wins;
};
```

### 6.3 Codec policy

Codec selection is probe- and client-capability-driven:

```text
AV1 -> HEVC Main10 -> HEVC -> H.264
```

Only select a codec when both client and host runtime probes succeed. Do not infer Moonlight AV1 support solely from the device's media decoder specification.

Defaults:

- SDR mobile and handheld: 4:2:0;
- HDR: 10-bit only when end-to-end signaling and output are verified;
- 4:4:4: explicit desktop-quality option only;
- H.264: universal recovery path.

Do not recreate the encoder merely to prefer a codec after streaming has started.

### 6.4 HDR policy

Panel HDR support alone is insufficient. Enable HDR only when all are true:

- client advertises HDR;
- selected codec supports the required bit depth/profile;
- Gamescope session was created with HDR intent;
- capture color metadata is present and valid;
- encoder and GameStream signaling probes pass.

Otherwise use SDR without treating the profile as failed.

## 7. Canonical profiles

All values below are production defaults, not hard limits. Client requests, host probes, and measured network state can lower them. Mid-stream bitrate changes are preferred over geometry changes.

### 7.1 Generic 4K 60 profile

```yaml
id: generic_4k60
match:
  requested_width: 3840
  requested_height: 2160
geometry:
  preferred: [3840, 2160]
  refresh_millihz: [59940, 60000]
  fallbacks:
    - [3200, 1800, 60000]
    - [2560, 1440, 60000]
    - [1920, 1080, 60000]
codec_order: [av1, hevc_main10, hevc, h264]
hdr: auto
fit_mode: exact
safe_area: none
bitrate_mbps:
  start: 80
  min: 45
  max: 120
queues:
  capture: 1
  encoder: 1
  network: 2
latest_frame_wins: true
```

Requirements:

- retain distinct 59.94 and 60.00 modes;
- use HEVC or AV1 when verified;
- prefer stereo or 5.1 based on the client request;
- 4K120 is a separate opt-in profile and must not be selected from a 60 Hz request.

Optional 4K120 profile:

```yaml
id: generic_4k120
geometry: [3840, 2160, 120000]
bitrate_mbps: { start: 130, min: 80, max: 180 }
codec_order: [av1, hevc_main10, hevc]
hdr: auto
```

It requires an explicit 120 Hz client request, successful host encode probe, and a network ceiling that can sustain it.

### 7.2 Lenovo Legion Y700 / Legion Tab profiles

Lenovo model generations differ. The engine must match requested geometry and refresh rather than the string `Y700` alone.

Official 2.5K class profile:

```yaml
id: y700_2560x1600_balanced
match:
  requested_aspect: "16:10"
  native_or_requested: [2560, 1600]
geometry:
  preferred: [2560, 1600, 120000]
  maximum: [2560, 1600, 165000]
  fallbacks:
    - [2560, 1600, 90000]
    - [1920, 1200, 120000]
    - [1920, 1200, 90000]
codec_order: [av1, hevc, h264]
hdr: auto
fit_mode: exact
safe_area: none
bitrate_mbps:
  start: 48
  min: 28
  max: 75
queues: { capture: 1, encoder: 1, network: 2 }
latest_frame_wins: true
```

Maximum-refresh variant:

```yaml
id: y700_2560x1600_165
geometry: [2560, 1600, 165000]
bitrate_mbps: { start: 65, min: 40, max: 95 }
activation:
  require_explicit_refresh_request: true
  require_clean_network_profile: true
```

User-observed 3K geometry profile:

```yaml
id: y700_3040x1904_90
match:
  exact_requested_geometry: [3040, 1904]
geometry:
  preferred: [3040, 1904, 90000]
  fallbacks:
    - [2560, 1600, 90000]
    - [1920, 1200, 90000]
codec_order: [av1, hevc, h264]
hdr: auto
bitrate_mbps:
  start: 60
  min: 35
  max: 90
queues: { capture: 1, encoder: 1, network: 2 }
latest_frame_wins: true
```

Do not assign the 3040x1904 profile to every Y700. It activates only from exact client capability/request or a saved per-client override.

Touch behavior:

- map absolute touch coordinates to the negotiated content rectangle;
- compensate only for real letterbox/pillarbox offsets;
- keep host pointer capture independent from touch injection;
- validate rotation and multi-touch using the actual Android client.

### 7.3 iPhone 16 Plus profile

The native panel is 2796x1290, OLED, HDR-capable, and supports HEVC, H.264, and AV1 media playback. The streaming profile must still rely on Moonlight's advertised decoder capabilities.

```yaml
id: iphone16_plus_native_60
match:
  exact_or_saved_geometry: [2796, 1290]
geometry:
  preferred: [2796, 1290, 60000]
  fallbacks:
    - [2560, 1182, 60000]
    - [1920, 886, 60000]
    - [1920, 1080, 60000]
codec_order: [av1, hevc_main10, hevc, h264]
hdr: auto
fit_mode: safe_fit
safe_area:
  policy: client_reported_then_calibrated
  default_edge_guard_percent: 3.0
bitrate_mbps:
  start: 35
  min: 18
  max: 55
queues: { capture: 1, encoder: 1, network: 2 }
latest_frame_wins: true
```

Requirements:

- cap the default profile at 60 FPS;
- preserve landscape 2796x1290 rather than forcing 16:9;
- use client-reported safe insets when available;
- provide a per-client safe-area calibration UI for stock clients that do not report insets;
- map touch only to the visible content rectangle;
- prevent the Dynamic Island and rounded corners from hiding critical Big Picture controls through `safe_fit` or a calibrated content viewport;
- default to stereo audio;
- do not enable HDR until the iOS Moonlight path is verified end to end.

### 7.4 Steam Deck OLED profile

The OLED panel is 1280x800, HDR-capable, and supports up to 90 Hz.

```yaml
id: steamdeck_oled_90
match:
  geometry: [1280, 800]
  refresh_at_least_millihz: 89000
geometry:
  preferred: [1280, 800, 90000]
  fallbacks:
    - [1280, 800, 60000]
codec_order: [av1, hevc_main10, hevc, h264]
hdr: auto
fit_mode: exact
safe_area: none
bitrate_mbps:
  start: 25
  min: 12
  max: 40
queues: { capture: 1, encoder: 1, network: 2 }
latest_frame_wins: true
input_profile: steamdeck_gamepad_touch_trackpad
```

Requirements:

- exact 16:10 canvas;
- 90 FPS only when requested;
- HDR only after the Deck Moonlight client advertises and renders it correctly;
- verify controller, trackpads, touchscreen, gyro if exposed by the client, Steam and quick-access overlay behavior;
- avoid supersampling by default because the panel is 1280x800.

### 7.5 Steam Deck LCD profile

```yaml
id: steamdeck_lcd_60
match:
  geometry: [1280, 800]
  refresh_max_millihz: 61000
geometry:
  preferred: [1280, 800, 60000]
codec_order: [hevc, h264, av1]
hdr: false
fit_mode: exact
safe_area: none
bitrate_mbps:
  start: 18
  min: 10
  max: 30
queues: { capture: 1, encoder: 1, network: 2 }
latest_frame_wins: true
input_profile: steamdeck_gamepad_touch_trackpad
```

## 8. Geometry and UI scaling

Geometry selection order:

```text
explicit user override
> exact requested stream geometry
> saved successful profile
> profile native geometry
> profile fallback
> generic request
```

Rules:

- width and height must be even and within encoder limits;
- preserve fractional refresh values;
- do not change resolution during a stream by default;
- reduce bitrate before considering a lower geometry on the next reconnect;
- use `fit` by default; cropping requires explicit `fill`;
- persist the last successful geometry per paired client and network class;
- UI scale must be selected separately from stream resolution.

Recommended initial UI scale:

```text
4K TV                     1.50 to 2.00
Y700 2560x1600            1.15 to 1.30
iPhone 16 Plus            1.20 to 1.35 with safe-area guard
Steam Deck 1280x800       1.00
```

Do not hardcode a Steam UI scale until the launched application supports a safe and reversible setting. Expose profile recommendations in Web UI first.

## 9. Adaptive bitrate implementation

### 9.1 Envelope

A profile defines `min`, `start`, and `max`. The client request and administrator ceiling can only reduce `max`.

```text
effective_max = min(profile_max, client_ceiling, administrator_ceiling, encoder_limit)
start         = clamp(profile_start, profile_min, effective_max)
```

### 9.2 Feedback controller

Use bounded delay/loss control rather than a full bandwidth speed test before each connection.

Initial controller:

- sample every 500 ms;
- aggregate over 2 seconds;
- reduce target by 20% when persistent loss, RTT growth, or socket queue growth indicates congestion;
- reduce by 10% for isolated but repeated loss without queue growth;
- increase by 5% no more than once every 2 seconds after a clean interval;
- require hysteresis before reversing direction;
- clamp within the profile envelope;
- request an IDR only when required and rate-limit keyframe requests;
- keep audio and protocol overhead outside the encoder video target.

Do not write every sample to disk. Use an in-memory ring buffer and batch only the final report.

### 9.3 Runtime encoder rate changes

Create an encoder rate adapter and probe support. When the active Vulkan Video backend cannot safely change rate control at runtime:

- keep the stable starting target;
- record the limitation;
- do not repeatedly recreate the encoder;
- apply a learned lower start rate on the next reconnect.

## 10. Queue and latency policy

Production defaults:

```text
capture pending frames:  1
encoder pending frames:  1
network queue target:    <= 2 frames
local presenter pending: 1 latest frame
```

Rules:

- latest frame wins;
- a slow local presenter cannot block remote capture;
- a slow Web client cannot block telemetry production;
- no `vkQueueWaitIdle` or `vkDeviceWaitIdle` on the normal frame path;
- do not retain DMA-BUF file descriptors beyond their ownership contract;
- cache Vulkan imports by stable buffer identity, format, modifier, dimensions, planes, offsets, and pitches;
- invalidate cache entries when the PipeWire buffer or source disappears.

## 11. Proposed file and interface changes

Refactor before adding the local presenter. Do not include one `.cpp` file from another new `.cpp` file.

Recommended files:

```text
src/client_profile.h
src/client_profile.cpp
src/steamos_session_source.h
src/steamos_session_source.cpp
src/platform/linux/pipewire_common.h
src/platform/linux/pipewire_common.cpp
src/platform/linux/gamescope_source.h
src/platform/linux/gamescope_source.cpp
src/platform/linux/gamescope_presenter.h
src/platform/linux/gamescope_presenter.cpp
```

Responsibilities:

- `client_profile.*`: capability normalization, matching, fallback and bitrate envelope;
- `steamos_session_source.*`: attach/reuse/create policy and ownership flags;
- `pipewire_common.*`: connected UNIX socket, registry snapshot, object serial target, shared descriptors;
- `gamescope_source.*`: verified Gamescope source classification;
- `gamescope_presenter.*`: local same-GPU DMA-BUF presentation.

Integrate with:

```text
src/nvhttp.cpp
src/rtsp.cpp
src/stream.cpp
src/video.cpp
src/config.cpp / config.h
src/web_services.cpp
src/steamos_virtual_session.cpp / .h
src/platform/linux/gamescopegrab.cpp
src/platform/linux/pipewire.cpp
src/platform/linux/vulkan_encode.cpp
```

## 12. Web UI and API

Add settings:

```text
Session source:
  Automatic
  Existing Game Mode Gamescope
  SteamShine private Gamescope

Local display:
  Automatic
  Off
  Mirror private virtual display

Client profile:
  Automatic
  Generic 4K60
  Generic 4K120
  Y700 2560x1600 balanced
  Y700 2560x1600 165 Hz
  Y700 3040x1904 90 Hz
  iPhone 16 Plus native
  Steam Deck OLED
  Steam Deck LCD
  Custom
```

Runtime status:

- source origin and ownership;
- Gamescope PID and process start time;
- Steam location;
- node ID and object serial;
- selected profile and reason;
- requested and selected geometry;
- codec, bit depth, HDR and chroma;
- bitrate envelope and current target;
- capture, encode and local-present counters;
- queue depth and frame-drop reasons;
- same-GPU result;
- reconnect generation.

Safe-area calibration:

- show a test pattern and corner controls;
- save insets per paired client and orientation;
- allow reset to client-reported or profile defaults;
- never expose credentials in the profile API.

## 13. Test plan

### 13.1 Unit tests

Session source:

- verified owned source;
- verified existing source;
- different UID rejection;
- PID start-time mismatch rejection;
- executable mismatch rejection;
- ambiguous candidates;
- explicit PID selection;
- attached-existing stop never signals or deletes;
- owned-private stop removes only owned resources.

Profiles:

- exact capability wins over device name;
- Y700 2560x1600 and 3040x1904 remain distinct;
- iPhone landscape and safe-area selection;
- Deck OLED vs LCD selection by refresh/HDR capability;
- 59.94 vs 60.00 preservation;
- unsupported AV1 or HDR falls back without failure;
- client/admin ceilings reduce profile bitrate maximum;
- odd dimensions normalize safely.

Local presenter:

- rejects non-DMA-BUF production buffers;
- rejects cross-GPU import;
- latest-frame-wins;
- stale local frames drop without remote blocking;
- node disappearance stops presenter only for attached-existing;
- owned capture failure follows owned teardown policy.

### 13.2 Integration tests

Use a fake PipeWire server with one Gamescope source and two independent consumers:

1. remote consumer attaches;
2. local presenter attaches;
3. both target the same object serial through separate core connections;
4. local consumer is deliberately delayed;
5. remote consumer continues receiving frames;
6. local consumer detaches;
7. remote consumer continues;
8. source disappears and ownership-specific cleanup is verified.

Test ten simulated disconnect/resume cycles without changing Gamescope PID or object serial.

### 13.3 Hardware matrix

For each profile, record:

- requested and selected geometry/FPS;
- codec, HDR, bit depth and chroma;
- captured frames;
- encoded packets/bytes/IDRs;
- DMA-BUF and same-GPU result;
- average, p50, p95 and p99 capture-to-encode latency;
- network queue and packet loss;
- audio and input result;
- CPU, GPU, VRAM and power observations;
- SSD writes;
- process and runtime cleanup.

#### 4K60

- 3840x2160 59.94 and 60.00;
- SDR HEVC baseline;
- HDR Main10 only after baseline;
- ten reconnects;
- audio and gamepad;
- optional 4K120 as a separate test.

#### Y700

- exact model/requested geometry evidence;
- 2560x1600 90/120 and optional 165;
- 3040x1904 90 only on the matching client;
- touch, rotation and controller;
- Wi-Fi stability and adaptive bitrate.

#### iPhone 16 Plus

- 2796x1290 60;
- safe-area calibration in both landscape orientations;
- touch mapping;
- HEVC SDR baseline;
- HDR and AV1 only when advertised and verified;
- Wi-Fi and mobile-network profiles remain separate.

#### Steam Deck

- LCD 1280x800 60 SDR;
- OLED 1280x800 90 SDR, then HDR;
- built-in gamepad, trackpads and touchscreen;
- Steam and quick-access overlay behavior;
- suspend/resume and ten reconnects.

## 14. Performance acceptance

Required invariants:

```text
additional Steam processes when attaching existing  = 0
additional Gamescope processes when attaching existing = 0
CPU full-frame copy                                 = 0
production capture memory                           = DMA-BUF
capture and encode GPU                              = same selected render node
software encoder fallback                           = 0
```

Relative regression limits against the current verified private-session baseline:

- remote capture-to-encode p95 must not regress by more than 5% or 1 ms, whichever is larger, when local presentation is enabled;
- local presenter must not increase remote queue depth beyond the configured bound;
- attach-to-existing should produce the first IDR within 1.5 seconds after a verified node is available;
- retained-session resume should produce the first IDR within 1.5 seconds;
- cold owned-session launch should complete within the existing startup timeout and report each stage;
- explicit owned cleanup should complete within 5 seconds;
- attached-existing cleanup must not terminate or alter the resident Gamescope.

If a strict timing threshold is not achievable on the target host, retain the measured baseline, document it, and set the acceptance threshold from reproducible p95 evidence rather than hiding the failure.

## 15. Goal Mode implementation order

### Goal G0 — freeze baseline and documentation

- verify the current head CI and Artifact;
- save current hardware report;
- update implementation status and link this plan;
- do not modify the verified remote data path yet.

Exit:

- baseline report and rollback Artifact recorded.

### Goal G1 — profile engine

- implement capability descriptor and profile schema;
- add the six canonical profiles and custom overrides;
- integrate selected geometry/FPS into Gamescope launch;
- integrate codec/HDR selection through runtime probes;
- add Web profile selection and diagnostics;
- add unit tests.

Exit:

- profile selection tests pass;
- current 1080p baseline still streams;
- at least Steam Deck and Y700 profile requests select the expected geometry.

### Goal G2 — session source abstraction

- generalize owned Gamescope registry code;
- implement verified existing Game Mode source detection;
- add ownership flags and safe lifecycle behavior;
- implement Steam location classification;
- add attach/reuse/create policy;
- add unit and integration tests.

Exit:

- Game Mode attach creates no new Steam or Gamescope process;
- owned-private path still passes the verified baseline;
- stopping attached-existing does not signal resident processes.

### Goal G3 — retained session and reconnect completion

- preserve source identity and profile across disconnect;
- reuse existing Steam/Gamescope;
- make first-IDR and reconnect counters observable;
- complete ten real reconnect cycles.

Exit:

- same Gamescope PID and Steam PID across ten cycles;
- positive counters every cycle;
- explicit stop cleans only owned sessions.

### Goal G4 — zero-copy local presenter

- refactor shared PipeWire utilities;
- implement second independent consumer;
- implement Vulkan DMA-BUF import and host Wayland present;
- implement latest-frame-wins and hotplug;
- verify no remote-path latency regression.

Exit:

- physical display can connect/disconnect without restarting Gamescope or Steam;
- local and remote show the same canonical session;
- no encoder/decode/CPU-copy is added for local presentation.

### Goal G5 — adaptive bitrate and profile tuning

- implement runtime feedback aggregation;
- implement encoder rate adapter where supported;
- implement learned next-session fallback where not supported;
- tune profile envelopes on actual devices;
- store only aggregated results.

Exit:

- stable LAN and Wi-Fi results for all available target devices;
- congestion lowers bitrate before geometry;
- recovery is gradual and bounded.

### Goal G6 — final hardware acceptance

Run the complete matrix:

- 4K60;
- available Y700 geometry;
- iPhone 16 Plus;
- Steam Deck LCD/OLED as available;
- Game Mode attach;
- Desktop/private owned session;
- monitorless;
- hotplug;
- audio, keyboard/mouse, gamepad and touch;
- ten reconnects;
- cleanup and rollback.

Exit:

- all available hardware passes;
- unavailable hardware is clearly marked `not tested`, never `passed`;
- PR body and implementation documents reflect actual evidence;
- latest CI and Artifact are verified;
- PR may leave Draft only after required acceptance is complete.

## 16. CI and push discipline

Before every push:

```bash
gh run list --repo souten-yd/SteamShine \
  --branch feat/steamos-auto-virtual-display --limit 20
```

Do not push while a run is queued or in progress. Local development and commits may continue, but batch related changes into one push. After pushing, wait for the full run result before the next push.

Required CI coverage:

- format, ShellCheck and actionlint;
- clean configure and runtime build;
- profile unit tests;
- session-source and ownership tests;
- fake PipeWire dual-consumer integration;
- lifecycle and reconnect tests;
- Web API and validation tests;
- ABI and runtime linkage ceiling;
- package and installer smoke;
- Artifact checksum and upload.

## 17. Rollback

Keep feature flags and the last verified Artifact.

```ini
steamos_session_source = owned_private
steamos_local_presentation = off
steamos_client_profile = auto
```

If attach-existing or local presentation fails:

- revert to the current verified owned-private PipeWire path;
- do not weaken ownership validation;
- do not add desktop capture fallback;
- do not allow CPU-copy production fallback;
- keep the PR Draft and record the blocking stage.

## 18. Prohibited shortcuts

- duplicate Steam in the same user session;
- unnecessary second Gamescope;
- kill or delete an attached-existing session;
- trust PID without process start time;
- trust node ID without object serial and client identity;
- use a device name as the sole profile selector;
- assume device media support equals Moonlight support;
- enable HDR without end-to-end evidence;
- production MemFd or CPU-copy fallback;
- local encode-to-decode mirroring;
- unbounded queues;
- per-frame persistent logs;
- claim completion from build/CI alone;
- push over a running CI job.

## 19. Completion report format

The final report must include:

1. commit and Artifact digest;
2. selected session origin and ownership flags;
3. Gamescope PID/start time and Steam PID/location;
4. PipeWire node/client/object serial;
5. selected profile and selection reason;
6. requested and negotiated geometry/FPS;
7. codec, HDR, bit depth and chroma;
8. bitrate min/start/max and final target;
9. capture memory and same-GPU evidence;
10. captured frames and encode counters;
11. local-present and dropped-local counters;
12. average/p50/p95/p99 stage latency;
13. CPU/GPU/VRAM/power and SSD-write observations;
14. audio and input matrix;
15. monitorless/hotplug result;
16. ten reconnect results;
17. cleanup result;
18. CI run and all job results;
19. unavailable hardware clearly marked;
20. remaining limitations.

## 20. Reference device facts

Use these only as profile seeds. Runtime capability negotiation remains authoritative.

- Apple iPhone 16 Plus: 2796x1290 OLED HDR display; HEVC, H.264 and AV1 media playback support.
- Lenovo Legion Tab Gen 3 / Y700-class global model: 2560x1600, up to 165 Hz, HDR10-class display.
- Steam Deck OLED: 1280x800 HDR OLED, up to 90 Hz.
- User-observed Y700 target: 3040x1904 at 90 FPS; activate only by exact client request or saved override.

Official references:

- Apple iPhone 16 Plus specifications: https://support.apple.com/ja-jp/121030
- Lenovo Legion Tab Gen 3 specifications: https://psref.lenovo.com/Product/LenovoTablets/Legion_Tab_8_8_3?tab=spec
- Steam Deck OLED specifications: https://www.steamdeck.com/ja/tech/oled
- Gamescope: https://github.com/ValveSoftware/gamescope
- PipeWire DMA-BUF: https://docs.pipewire.org/page_dma_buf.html
- Moonlight PC capabilities: https://github.com/moonlight-stream/moonlight-qt
