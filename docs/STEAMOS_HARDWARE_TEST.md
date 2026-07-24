# SteamOS hardware acceptance test

Install a validated PR artifact without installing development packages or changing the read-only image:

```bash
./steamshine.sh install --channel pr --pr 6
./steamshine.sh hardware-test --interactive
```

The hardware command must be run from a desktop/user systemd session with the physical display disconnected. It first records a strict SteamOS 3.8.16 compatibility gate: ABI floors, Gamescope headless/device options, and the expected RX 9070 XT PCI BDF/render node. Confirm only when prompted that Moonlight video, audio, keyboard, mouse, and gamepad work. During the first connected cycle it keeps Moonlight connected for 60 seconds and records latency evidence plus SteamShine write counters; the remaining cycles check teardown and reconnect behavior. The scripts collect Gamescope/PipeWire/DRM state, supported headless options, optional ROCm telemetry when installed, ownership-marked Gamescope process groups, runtime paths, reconnect results, latency evidence, and start/end/delta SteamShine write counters including the user-journal byte delta when it is readable. `pidstat`, `vainfo`, `pw-dump`, ROCm tools, and MangoHud are optional diagnostics: an unavailable tool is recorded as skipped and never causes acceptance to fail. Every command run shares one report directory at `~/.local/state/steamshine/hardware-tests/<timestamp>/`; it contains `hardware-report.json`, service-journal evidence, PipeWire dumps when available, and the raw logs. A failure stops the user service but does not disable its autostart setting and leaves the collected evidence there.

Expected automated result: each disconnect removes only SteamShine-owned, marker-verified `session-*` runtime paths and process groups within five seconds; ten cycles pass before latency and SSD tests are collected. The sender maintains packet, byte, and IDR counters in memory and logs their final values once during owned-session cleanup. Every disconnect cycle must contain positive packet, byte, and IDR evidence in its user-service journal; the harness writes the ten verified rows to `encoded-stream-evidence.tsv`. `hardware-report.json` also records the final counter entry. A `null` or zero counter is not a successful encoder proof.
