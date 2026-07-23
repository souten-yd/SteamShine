# SteamOS hardware acceptance test

Install a validated PR artifact without installing development packages or changing the read-only image:

```bash
./steamshine.sh install --channel pr --pr 6
./steamshine.sh hardware-test --interactive
```

The hardware command must be run from a desktop/user systemd session with the physical display disconnected. Confirm only when prompted that Moonlight video, audio, keyboard, mouse, and gamepad work. The scripts collect Gamescope/PipeWire/DRM state, owned process groups, runtime paths, reconnect results, latency evidence, and SteamShine write counters. A failure leaves the report in `~/.local/state/steamshine/hardware-tests/`.

Expected automated result: each disconnect removes SteamShine-owned `session-*` runtime paths and process groups within five seconds; ten cycles pass before latency and SSD tests are collected.
