# SteamOS HDR implementation plan

- Status: Required release target
- Scope: SteamOS headless Gamescope, Wayland DMA-BUF, Vulkan Video, Moonlight/GameStream
- Target clients: Lenovo Legion Y700 at 3040x1904 90 FPS and 4K television at 3840x2160 59.94/60 FPS

## Release policy

HDR is not optional for the final SteamShine release target. The project may keep intermediate H.264/HEVC SDR artifacts for bring-up and regression isolation, but the production milestone is not complete until the required HDR acceptance cases pass on real SteamOS hardware.

The initial PR may remain Draft while SDR bring-up proceeds. It must not be presented as feature-complete when HDR capture, signaling, encode, transport, decode, and display validation are still missing.

## Required HDR targets

### 4K television

Mandatory production target:

- 3840x2160 at 59.94 and 60.00 FPS;
- HEVC Main10;
- 10-bit P010 encode input;
- HDR10 static metadata;
- correct BT.2020 primaries;
- PQ/ST 2084 transfer characteristics;
- limited-range television output unless the client explicitly negotiates otherwise;
- stable audio, input, cleanup, reconnect, latency, and write-volume behavior.

### Lenovo Legion Y700

Mandatory compatibility target:

- 3040x1904 at 90 FPS;
- 10-bit HDR when the Moonlight client, Android display path, and panel mode report a valid HDR-capable chain;
- HEVC Main10 as the required first HDR codec;
- SDR fallback only when the client or display chain explicitly lacks HDR capability;
- no silent HDR-to-SDR downgrade when the client requests HDR and all required capabilities are present.

Because Android devices may expose different HDR capabilities by OS build, display mode, and Moonlight version, Y700 HDR support must be capability-gated and reported truthfully. A failed HDR capability check must produce a clear reason instead of silently changing color mode.

## End-to-end HDR path

The implementation must validate every stage:

1. Moonlight requests HDR and advertises a supported HDR codec/profile.
2. `nvhttp` and RTSP launch state preserve HDR intent, bit depth, codec preference, and color metadata.
3. SteamShine requests an HDR-capable Gamescope virtual display mode.
4. Gamescope exposes a 10-bit-capable Wayland DMA-BUF format and modifier on the owned socket.
5. The capture backend preserves 10-bit data and does not convert to 8-bit RGB/NV12.
6. Vulkan import validates the same AMD DRM render node and imports the 10-bit buffer without CPU readback.
7. RGB-to-YUV conversion writes P010 with correct range, primaries, matrix, and transfer function.
8. `hevc_vulkan` opens Main10 on the selected RX 9070 XT device.
9. Encoded output contains valid VPS/SPS/PPS, 10-bit profile information, IDR/keyframe output, and HDR metadata.
10. GameStream packetization and Moonlight negotiation preserve the HDR stream without codec or profile mismatch.
11. The client decodes as HDR and the display enters the expected HDR mode.
12. Disconnect and reconnect remove only SteamShine-owned resources and restore the next session correctly.

## Sunshine-core changes

The following changes belong in Sunshine-derived capture/encode/protocol code rather than shell-only integration.

### 10-bit capture and frame representation

- Extend the SteamOS virtual capture path to negotiate and preserve 10-bit DMA-BUF formats.
- Record DRM fourcc, modifier, plane layout, bit depth, range, primaries, transfer, and matrix for every negotiated capture format.
- Reject an HDR session when the capture backend produces only an 8-bit format.
- Do not implement hidden CPU conversion or 8-bit staging as an HDR fallback.
- Keep the existing same-GPU enforcement for Gamescope, capture, conversion, and encode.

### P010 Vulkan conversion

- Add or validate an RGB-to-P010 compute path.
- Select conversion coefficients from explicit HDR colorspace state rather than assuming SDR BT.709.
- Support BT.2020 non-constant-luminance matrix where required by the encode path.
- Preserve limited/full-range intent and ensure chroma siting matches encoder expectations.
- Add shader and CPU-side unit tests for representative black, white, primary, gray, and near-black values.
- Prevent 8-bit quantization before the encoder receives the frame.

### HEVC Main10 encoder path

- Add a selected-device `hevc_vulkan` Main10 probe using `AV_PIX_FMT_VULKAN` with `AV_PIX_FMT_P010` software format.
- Validate that FFmpeg, Vulkan Video profiles, and the RX 9070 XT expose the required Main10 capability.
- Open a real encoder context and submit at least one frame during the hardware acceptance test.
- Confirm non-empty bitstream output and an actual keyframe.
- Parse or inspect stream headers sufficiently to verify 10-bit HEVC Main profile compatibility.
- Preserve explicit H.264/HEVC SDR fallback only when HDR was not requested or capability negotiation fails before stream launch.

### HDR metadata

The path must carry, where available:

- color primaries;
- transfer characteristics;
- matrix coefficients;
- mastering display metadata;
- MaxCLL and MaxFALL;
- content light metadata validity state.

Policy:

- pass through trusted source metadata when available;
- use a documented conservative default only when the source is HDR but omits optional static metadata;
- never fabricate display-specific peak brightness claims;
- log whether metadata was source-derived, client-derived, configured, or defaulted;
- avoid per-frame persistent logging.

### Protocol and Moonlight negotiation

- Verify that the existing GameStream launch and RTSP paths retain HDR intent and codec/profile selection.
- Reject inconsistent combinations such as HDR requested with H.264 8-bit output.
- Ensure HEVC Main10 is selected only when both host and client support it.
- Preserve immediate IDR for a new connection while applying the planned repeated-IDR rate limit.
- Keep bitrate-only updates from unnecessarily recreating Gamescope or the virtual display.

## SteamShine-specific changes

### Client profiles

Add explicit profile fields:

```yaml
hdr_policy: required | auto | disabled
hdr_codec_order:
  - hevc_main10
  - av1_10bit
sdr_fallback: explicit_only
bit_depth: 10
color_primaries: bt2020
transfer: smpte2084
matrix: bt2020nc
range: limited
```

For the 4K television production profile, `hdr_policy` is `required` when the user selects the HDR profile. For the Y700 profile, `auto` may be the default, but a user-selected required-HDR mode must fail clearly rather than silently downgrading.

### Gamescope launch policy

- Detect the exact installed Gamescope HDR-related options from `gamescope --help`.
- Generate only options actually supported by that version.
- Keep inherited desktop `DISPLAY` and `WAYLAND_DISPLAY` cleared for the private headless session.
- Record the selected HDR mode, format, refresh rate, and private socket in the hardware report.
- Fail before launching the application when the requested HDR virtual display cannot be created.

### Web UI

Expose:

- HDR Auto / Required / Disabled;
- detected client HDR capability;
- selected codec/profile and bit depth;
- negotiated colorspace;
- reason for HDR rejection or SDR fallback;
- mastering metadata source;
- current HDR session state.

The server must validate all combinations. The UI must not claim HDR merely because the user selected an HDR checkbox.

## Performance requirements

HDR must retain the existing low-copy design:

- no CPU readback;
- no software encoder fallback;
- no cross-GPU conversion;
- no normal-stream `vkQueueWaitIdle` or `vkDeviceWaitIdle`;
- bounded queue depth;
- DMA-BUF cache aware of bit depth, format, modifier, planes, and device identity;
- cache invalidation on SDR/HDR transition, format change, resolution change, encoder recreation, device loss, or session stop.

The latency instrumentation plan must distinguish SDR NV12 and HDR P010 conversion/encode timings.

## HDR-specific watchdog and recovery

Recovery must not silently change color mode.

1. Capture reattach must request the same 10-bit HDR format.
2. Encoder recreation must request the same Main10 profile and metadata.
3. Virtual-session recreation must preserve the requested HDR mode.
4. If HDR recreation fails, stop the session and report the reason.
5. SDR fallback is allowed only when policy is `auto` and the fallback event is visible to the user and hardware report.
6. `VK_ERROR_DEVICE_LOST` requires full Vulkan and encoder destruction; the same context must not be reused.

## Hardware acceptance matrix

| Scenario | Required codec/profile | Result |
| --- | --- | --- |
| 1920x1080 60 FPS SDR | H.264 8-bit | Bring-up baseline |
| 3040x1904 90 FPS SDR | HEVC 8-bit | Y700 SDR regression baseline |
| 3040x1904 90 FPS HDR | HEVC Main10 | Mandatory Y700 HDR capability case when client chain supports HDR |
| 3840x2160 59.94 FPS HDR | HEVC Main10 | Mandatory 4K television production case |
| 3840x2160 60.00 FPS HDR | HEVC Main10 | Mandatory alternate 4K production case |
| 3840x2160 60 FPS SDR | HEVC 8-bit | Explicit SDR compatibility case |
| 3040x1904 90 FPS HDR | AV1 10-bit | Later codec target; not a substitute for required HEVC Main10 |
| 3840x2160 60 FPS HDR | AV1 10-bit | Later codec target; not a substitute for required HEVC Main10 |

For every HDR case collect:

- negotiated codec, profile, level, bit depth, and pixel format;
- capture fourcc/modifier/planes;
- color primaries, transfer, matrix, and range;
- mastering metadata, MaxCLL, and MaxFALL source;
- capture, conversion, encode, and packet latency average/p95/p99/max;
- dropped frames and reason;
- CPU wait count/duration;
- GPU in-flight submission depth;
- encoder queue depth;
- network send queue depth;
- render-node identity;
- client decoder latency when available;
- cleanup time, reconnect result, and persistent write volume.

## Visual acceptance

Automated metadata checks are necessary but insufficient. A human acceptance pass must confirm:

- television or tablet enters HDR mode;
- highlights are not clipped unexpectedly;
- near-black detail is visible without raised blacks;
- SDR desktop content is not incorrectly treated as PQ;
- colors are not washed out or over-saturated;
- no green/purple plane-order corruption;
- no 8-bit banding regression caused by an unintended conversion;
- reconnect preserves HDR mode;
- switching between explicit SDR and HDR profiles does not leave stale colorspace state.

Use known HDR test clips and game scenes, but do not make acceptance depend solely on subjective viewing. Save screenshots only when the client capture path preserves HDR information; ordinary SDR screenshots are not proof of HDR correctness.

## CI requirements

CI must cover logic that does not require a real GPU:

- HDR profile validation;
- invalid H.264-plus-HDR rejection;
- P010 frame-pool selection;
- SDR/HDR cache-key separation and invalidation;
- HDR watchdog preserving mode across recovery;
- metadata source/default policy;
- HEVC Main10 probe success/failure paths with mocks or capability fixtures;
- explicit-only SDR fallback policy;
- existing same-GPU and user-space installation protections.

CI must not claim real HDR output. Real Gamescope, DMA-BUF, Vulkan Video Main10, Moonlight decode, and display-mode acceptance remain hardware-gated.

## Implementation order

1. Complete H.264 SDR live Moonlight acceptance and retain it as the regression baseline.
2. Complete the observability, wait detection, DMA-BUF cache, bounded queue, and recovery work.
3. Complete HEVC 8-bit SDR live streaming on Y700 and 4K television.
4. Add 10-bit capture negotiation and P010 frame representation.
5. Add and validate RGB-to-P010 conversion with HDR colorspace handling.
6. Add selected-device HEVC Main10 probe and actual-frame encode test.
7. Preserve and transmit HDR metadata through encode and GameStream negotiation.
8. Validate 4K television HDR at 59.94 and 60.00 FPS.
9. Validate Y700 HDR at 3040x1904 90 FPS when its client/display chain reports support.
10. Run ten reconnect cycles, device-loss recovery, watchdog recovery, latency, and SSD-write tests in HDR mode.
11. Mark the HDR milestone complete only after all mandatory cases pass.
12. Add AV1 10-bit HDR afterward as an additional codec, not as a replacement for HEVC Main10 acceptance.

## Completion gate

SteamShine is not HDR-complete until:

- 4K television HEVC Main10 HDR passes at both 59.94 and 60.00 FPS;
- Y700 HDR passes when the actual client/display chain advertises HDR support, with a documented capability result when it does not;
- the stream is confirmed to remain 10-bit from capture through encoder input;
- HDR metadata and colorspace are correct and visible in diagnostics;
- no CPU readback, software fallback, or cross-GPU path is used;
- ten reconnects pass in HDR mode;
- watchdog and device-loss recovery preserve HDR or fail explicitly;
- no unresolved color corruption, banding, clipping, or stale SDR/HDR state remains;
- CI remains green and the SteamOS artifact stays user-space-only.
