# SteamOS automatic virtual display implementation

- Status: Implementing — hardware validation required
- Last updated: 2026-07-24

## Current integration map

1. `src/main.cpp` initializes Sunshine services.
2. `src/nvhttp.cpp::launch()` parses Moonlight/GameStream launch parameters into `rtsp_stream::launch_session_t`; it includes width, height, FPS, bitrate-related protocol data and HDR intent.
3. `display_device::configure_display()` applies existing physical-display policy and `video::probe_encoders()` selects capture/encoder before `proc::proc.execute()` starts the requested application.
4. `rtsp_stream::launch_session_raise()` accepts the RTSP connection, then `stream::session::start()` creates video, audio, and input workers.
5. `stream::session::join()` stops workers, releases input/audio/video, reverts display configuration, and now requests virtual-session cleanup.

Capture implementations are `src/platform/linux/pipewire.cpp` (PipeWire/DMA-BUF where negotiated), `wayland.cpp`, `kmsgrab.cpp`, and `x11grab.cpp`. The SteamOS virtual route uses `wayland.cpp`/`wlgrab.cpp` and accepts Vulkan Video capture memory so DMA-BUF frames can be imported by `vulkan_encode.cpp` without CPU readback. VA-API and Vulkan Video are `vaapi.cpp` and `vulkan_encode.cpp`. Application prep/undo is owned by `process.cpp` and configured in `config.cpp`. Linux service packaging is `packaging/linux/*.service.in`; tests are GTest under `tests/`.

## Implemented boundary

`steamos_virtual_display_enabled=false` preserves normal Sunshine behavior. When enabled, `steamos_virtual_session` normalizes client dimensions to 640x480–7680x4320 and FPS to 30–240, creates an owned directory under `XDG_RUNTIME_DIR/steamshine`, starts a process group, and requires a real UNIX Wayland socket before continuing. It derives a Gamescope command from the installed binary's `--help` output; Gamescope 3.16 uses `--backend headless`, while older binaries are accepted only when they advertise a legacy `--headless` option. The command includes `--expose-wayland`, nested size/refresh, the `fit` policy when available, optional HDR, and an AMD Vulkan device preference.

The existing Wayland DMA-BUF backend now opens the owned socket by file descriptor instead of changing Sunshine's process-wide `XDG_RUNTIME_DIR`. After the backend has verified its Wayland capture interfaces, it marks the virtual session capture-ready. Existing application execution receives only the owned `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`, so prep/undo semantics remain under the existing process manager. Failure is returned as GameStream 503 before application launch. Cleanup kills only the owned process group and deletes only its owned directory. On SteamShine restart, an ownership marker plus an exact `XDG_RUNTIME_DIR` process-environment match is required before an orphan runtime or process is removed.

## Remaining hardware-gated work

The dedicated PipeWire-node provider has not been implemented; the current virtual path uses the existing Wayland DMA-BUF capture backend, which is the low-copy display source exposed by Gamescope. PipeWire node ownership/disappearance tests and monitorless Moonlight acceptance remain hardware-gated. GitHub Actions run 30067001826 exercised configure, build, targeted GTest, installer smoke, runtime linkage, packaging, and Artifact upload. On the SteamOS 3.8.16 RX 9070 XT host, the final Artifact installed in user space, initialized its packaged assets, started its user service, and passed the Vulkan H.264 encoder-open probe; it did not yet stream a live Moonlight frame. Use `./steamshine.sh hardware-test --interactive` on the target system.

## Configuration

The added keys are `steamos_virtual_display_enabled`, `steamos_virtual_display_mode`, `steamos_gamescope_path`, `steamos_runtime_directory`, GPU preference keys, startup/shutdown timeout keys, default display values, and `steamos_cleanup_orphan_sessions`. A blank GPU preference selects the AMD render node with the largest dedicated VRAM (requiring at least 1 GiB) and refuses the usual UMA iGPU path. PCI BDF, card node, and render-node selectors are resolved through sysfs. Because Gamescope advertises a vendor/device selector rather than a PCI-BDF selector, SteamShine rejects a launch if two AMD render nodes share the requested identifier. The active virtual session feeds its render node into existing VA-API and Vulkan Video device resolution; capture and encoder overrides must resolve to the same node.
