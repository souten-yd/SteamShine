# SteamOS hardware acceptance test

Install a validated PR artifact without installing development packages or changing the read-only image:

```bash
./steamshine.sh install --channel pr --pr 6
./steamshine.sh hardware-test --interactive
```

The hardware command must be run from a desktop/user systemd session with the physical display disconnected. It first records a strict SteamOS 3.8.16 compatibility gate: ABI floors, Gamescope headless/device options, and the expected RX 9070 XT PCI BDF/render node. Confirm only when prompted that Moonlight video, audio, keyboard, mouse, and gamepad work. During the first connected cycle it keeps Moonlight connected for 60 seconds and records latency evidence plus SteamShine write counters; the remaining cycles check teardown and reconnect behavior. The scripts collect Gamescope/PipeWire/DRM state, supported headless options, optional ROCm telemetry when installed, owned process groups, runtime paths, reconnect results, latency evidence, and SteamShine write counters. Every command run shares one report directory at `~/.local/state/steamshine/hardware-tests/<timestamp>/`; a failure stops the user service and leaves the collected evidence there.

Expected automated result: each disconnect removes SteamShine-owned `session-*` runtime paths and process groups within five seconds; ten cycles pass before latency and SSD tests are collected.
