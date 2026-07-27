# SteamOS hardware acceptance test

Install a validated PR artifact without installing development packages or changing the read-only image:

```bash
./steamshine.sh install --channel pr --pr 7
./steamshine.sh hardware-test --interactive
```

Validate the following matrix from the exact PR 7 artifact and packaged user
systemd unit. Do not substitute a locally built binary:

| Session | Display state | Required video result | Required input result |
| --- | --- | --- | --- |
| Desktop | Connected physical display with working KWin ScreenCast or Portal capture | Prefer KWin ScreenCast, otherwise Portal; stream the physical Desktop even if sysfs reports its CRTC disabled; no green placeholder and no Steam requirement | Mouse, keyboard, touch, and gamepad operate the physical Desktop |
| Desktop | No connected physical display or no working physical compositor capture source | Create or reuse the owned virtual desktop and show a usable Desktop | Pointer cursor is visible and pointer/keyboard input operates the streamed virtual Desktop |
| Game Mode | Verified existing Gamescope | Attach without stopping or starting a second Steam instance; show the current Gamescope/Big Picture surface | Mouse and keyboard operate that Gamescope surface through its verified EIS endpoint, never the physical Desktop |

For the monitorless Desktop row, verify that the configured
`steamos_virtual_desktop_command` surface appears over a black compositor
background. A black fallback frame is acceptable only during startup or source
reattachment; a connection that remains black is a failure.

Automatic physical Desktop capture uses KWin's direct ScreenCast protocol when
available so a broken Portal chooser cannot block service startup. Explicit
Portal capture remains supported, but every D-Bus call and response wait is
bounded; a timed-out request is closed before fallback or shutdown.

For Game Mode, a missing or unverifiable EIS endpoint is an acceptance failure
with a stable `gamescope_input_*` reason. Physical-Desktop input fallback is
intentionally forbidden.
The SteamShine dashboard must report `input_route_target=gamescope_eis` and an
empty `input_route_error` before input acceptance. After a disconnect made
while touch or pen is held, reconnect and confirm the primary pointer button is
released rather than stuck.

The separate force-mode helper copies the active configuration, disables
session retention, and uses a private short runtime path. It restores the
normal user service on exit, and its owned Gamescope must not retain the
streaming ports after the helper finishes.

The hardware command must be run from a desktop/user systemd session with the physical display disconnected. It first records a strict SteamOS 3.8.16 compatibility gate: ABI floors, Gamescope headless/device options, and the expected RX 9070 XT PCI BDF/render node. Confirm only when prompted that Moonlight video, audio, keyboard, mouse, and gamepad work. During the first connected cycle it keeps Moonlight connected for 60 seconds and records latency evidence plus SteamShine write counters; the remaining cycles check disconnect-and-resume behavior. The scripts collect Gamescope/PipeWire/DRM state, supported headless options, optional ROCm telemetry when installed, ownership-marked Gamescope process groups, runtime paths, reconnect results, latency evidence, and start/end/delta SteamShine write counters including the user-journal byte delta when it is readable. `pidstat`, `vainfo`, `pw-dump`, ROCm tools, and MangoHud are optional diagnostics: an unavailable tool is recorded as skipped and never causes acceptance to fail. Every command run shares one report directory at `~/.local/state/steamshine/hardware-tests/<timestamp>/`; it contains `hardware-report.json`, service-journal evidence, PipeWire dumps when available, and the raw logs. A failure stops the user service but does not disable its autostart setting and leaves the collected evidence there.

Expected automated result: while connected and after every Moonlight disconnect, each cycle must prove the same marker-owned runtime, the recorded Gamescope group-leader PID, a matching `/proc/<pid>/environ` runtime value, and a private `gamescope-0` UNIX socket. Disconnect is intentionally non-destructive: the app and its SteamShine-owned virtual Gamescope remain ready for `/resume`. After the tenth disconnect, the operator explicitly stops SteamShine (or cancels the app); only then must those `session-*` runtime paths and process groups disappear within five seconds. The sender maintains captured-frame, packet, byte, and IDR counters in memory and logs their final values once during this explicit owned-session cleanup. The harness writes one positive final counter row to `encoded-stream-evidence.tsv`, and `hardware-report.json` records it together with retained-session and cleanup evidence. A `null` or zero counter is not a successful capture or encoder proof.
