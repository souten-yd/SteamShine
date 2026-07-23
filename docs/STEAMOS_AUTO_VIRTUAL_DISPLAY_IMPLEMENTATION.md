# SteamOS automatic virtual display implementation

- Status: Implementing — hardware validation required
- Last updated: 2026-07-23

## Current integration map

1. `src/main.cpp` initializes Sunshine services.
2. `src/nvhttp.cpp::launch()` parses Moonlight/GameStream launch parameters into `rtsp_stream::launch_session_t`; it includes width, height, FPS, bitrate-related protocol data and HDR intent.
3. `display_device::configure_display()` applies existing physical-display policy and `video::probe_encoders()` selects capture/encoder before `proc::proc.execute()` starts the requested application.
4. `rtsp_stream::launch_session_raise()` accepts the RTSP connection, then `stream::session::start()` creates video, audio, and input workers.
5. `stream::session::join()` stops workers, releases input/audio/video, reverts display configuration, and now requests virtual-session cleanup.

Capture implementations are `src/platform/linux/pipewire.cpp` (PipeWire/DMA-BUF where negotiated), `wayland.cpp`, `kmsgrab.cpp`, and `x11grab.cpp`. VA-API and Vulkan Video are `vaapi.cpp` and `vulkan_encode.cpp`. Application prep/undo is owned by `process.cpp` and configured in `config.cpp`. Linux service packaging is `packaging/linux/*.service.in`; tests are GTest under `tests/`.

## Implemented boundary

`steamos_virtual_display_enabled=false` preserves normal Sunshine behavior. When enabled, `steamos_virtual_session` normalizes client dimensions to 640x480–7680x4320 and FPS to 30–240, creates an owned directory under `XDG_RUNTIME_DIR/steamshine`, starts a process group, and requires a Wayland socket before continuing. Failure is returned as GameStream 503 before application launch. Cleanup kills only that group and deletes only its owned directory.

## Remaining hardware-gated work

Gamescope option sets, PipeWire node discovery, display-source selection, and same-dGPU DMA-BUF capture must be validated on the target SteamOS build before enabling the flag. The current environment has no CMake or SteamOS GPU stack, so no executable build or hardware test was possible. Use `scripts/diagnose-steamos-virtual-display.sh` and `scripts/test-steamos-virtual-display.sh` on the target system.

## Configuration

The added keys are `steamos_virtual_display_enabled`, `steamos_virtual_display_mode`, `steamos_gamescope_path`, `steamos_runtime_directory`, GPU preference keys, startup/shutdown timeout keys, default display values, and `steamos_cleanup_orphan_sessions`. GPU preference keys are recorded but not yet connected to the existing PipeWire/VA-API/Vulkan adapter selectors; therefore the feature must remain disabled until that adapter integration is complete.
