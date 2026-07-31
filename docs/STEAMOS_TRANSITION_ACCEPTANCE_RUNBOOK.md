# SteamOS display and session transition acceptance runbook

## Purpose

This runbook preserves the complete procedure used to build, install, exercise,
and diagnose SteamShine across SteamOS Desktop Mode, stock Game Mode, and an
owned monitorless Gamescope session. It is the durable acceptance procedure;
`hardware-acceptance-transition-resume.md` is the chronological investigation
log and must not be used as the only source of instructions.

The procedure distinguishes three capture origins:

1. **Physical Desktop**: KWin PipeWire capture of a connected display.
2. **Attached Game Mode**: direct PipeWire capture from the stock Gamescope
   started by `gamescope-session.service`. SteamShine does not own or stop it.
3. **Owned private headless**: a remote-only Gamescope started and owned by
   SteamShine when no usable KWin or resident Game Mode source exists.
4. **Owned private nested**: a fullscreen Wayland-backend Gamescope hosted by a
   verified live KWin. Its own PipeWire source is consumed directly by
   Moonlight, so the local window and remote stream share one virtual canvas.

Automatic source selection occurs when Moonlight starts an application. A
source is re-discovered and its process, PipeWire object, display endpoint, and
GPU identity are reverified for that request. A stream already in progress is
not migrated live between capture backends. If the active compositor or display
disappears, disconnect and start the application again to exercise automatic
selection against the new state.

## Safety and evidence rules

- Never remove an owned runtime, restart the service, reinstall, or roll back
  until the failure bundle described below has been collected.
- Never signal a stock Gamescope process. Only a runtime with SteamShine's
  ownership marker and matching process identity may be reclaimed.
- Do not use `scripts/test-steamos-virtual-display.sh` to judge attached stock
  Game Mode. That harness intentionally requires an owned private runtime.
- Do not infer a TLS root cause from the Moonlight dialog alone. A client may
  display a security/TLS-flavoured error after the server rejects application
  launch because video initialization failed. Call it TLS only when the server
  journal contains a certificate, crypto, or handshake failure for the same
  request.
- Preserve unrelated worktree changes. This procedure creates reports and
  build output but does not reset the repository or create a pull request.

## Fixed acceptance variables

Run commands as the interactive Deck user in Desktop Mode unless a step says
otherwise. Use a fresh report directory for every complete pass:

```bash
cd /home/deck/SteamShine
acceptance_stamp="$(date +%Y%m%d-%H%M%S)"
acceptance_root="/home/deck/.local/state/steamshine/transition-acceptance/${acceptance_stamp}"
mkdir -p "${acceptance_root}"
chmod 700 "${acceptance_root}"
printf '%s\n' "${acceptance_root}"
```

Write that printed path into the test notes. In every later shell, restore the
same literal value instead of creating another timestamp:

```bash
acceptance_root='/home/deck/.local/state/steamshine/transition-acceptance/YYYYMMDD-HHMMSS'
```

## Record the starting state

Capture this before building or installing anything:

```bash
cd /home/deck/SteamShine
git status --short >"${acceptance_root}/git-status-before.txt"
git rev-parse HEAD >"${acceptance_root}/git-head.txt"
git diff --stat >"${acceptance_root}/git-diff-stat.txt"
systemctl --user cat steamshine.service >"${acceptance_root}/service-unit.txt"
systemctl --user show steamshine.service \
  -p MainPID -p NRestarts -p ActiveState -p SubState \
  -p ActiveEnterTimestamp -p ExecMainStartTimestamp \
  >"${acceptance_root}/service-before.txt"
systemctl --user show-environment \
  >"${acceptance_root}/systemd-user-environment-before.txt"
pgrep -a gamescope >"${acceptance_root}/gamescope-before.txt" || true
find /sys/class/drm -maxdepth 2 -name status -print -exec sed -n '1p' {} \; \
  >"${acceptance_root}/drm-status-before.txt"
pw-dump >"${acceptance_root}/pipewire-before.json" 2>&1 || true
./steamshine.sh autostart-status \
  >"${acceptance_root}/autostart-before.txt" 2>&1 || true
```

Record the current service PID, restart count, start timestamp, physical cable
state, current SteamOS mode, and client address in a plain-text operator note.
Do not put Moonlight PINs, session tokens, passwords, or private keys in it.

## Build and static verification

Read the immutable image and digest from `ci/steamos/image.lock`. Never use a
mutable local tag or rolling distrobox for a binary that will run on SteamOS.
The build directory name must retain the `cmake-build-` prefix.

```bash
cd /home/deck/SteamShine
steamos_image="$(sed -n 's/^image=//p' ci/steamos/image.lock)"
steamos_digest="$(sed -n 's/^digest=//p' ci/steamos/image.lock)"
podman run --rm --userns=keep-id \
  -v /home/deck/SteamShine:/workspace:rw -w /workspace \
  "${steamos_image}@${steamos_digest}" \
  cmake --build cmake-build-transition-fix-full -j2 \
  --target sunshine test_sunshine steamshine-input-visualizer
```

Run the focused tests inside the same image so the result is not affected by
missing host libraries:

```bash
podman run --rm --userns=keep-id \
  -v /home/deck/SteamShine:/workspace:rw -w /workspace \
  "${steamos_image}@${steamos_digest}" \
  ./cmake-build-transition-fix-full/tests/test_sunshine \
  --gtest_filter='SteamOSVirtualSessionCore.*:ProcessCommandSelectionTest/*:MainLoop.*:VideoPipelineDiagnostics.*' \
  --gtest_color=yes

podman run --rm --userns=keep-id \
  -v /home/deck/SteamShine:/workspace:rw -w /workspace \
  "${steamos_image}@${steamos_digest}" \
  ./cmake-build-transition-fix-full/tests/test_sunshine \
  --gtest_filter='SteamOSVirtualSessionLifecycle.*' --gtest_color=yes

python3 -m unittest tests.integration.test_steamos_client_display
```

Run format and repository checks without rewriting unrelated files:

```bash
podman run --rm --userns=keep-id \
  -v /home/deck/SteamShine:/workspace:rw -w /workspace \
  "${steamos_image}@${steamos_digest}" \
  clang-format --dry-run --Werror \
  src/platform/linux/pipewire_capture.h \
  src/platform/linux/pipewire_capture.cpp \
  src/platform/linux/pipewire.cpp \
  src/platform/linux/gamescopegrab.cpp \
  tests/unit/test_steamos_virtual_session_core.cpp

git diff --check
shellcheck scripts/package-steamos-artifact.sh
```

All new or changed C/C++ declarations must have Doxygen comments, and changed
methods must have GTest coverage. A host execution failure caused only by a
missing dynamic library is not a test failure if the same binary passes in the
fixed compatibility image; record both outcomes.

## Package and identify the exact artifact

Choose a new output directory rather than overwriting earlier evidence:

```bash
cd /home/deck/SteamShine
artifact_dir="/home/deck/SteamShine/dist/transition-acceptance-${acceptance_stamp}"
./scripts/package-steamos-artifact.sh \
  cmake-build-transition-fix-full \
  "${artifact_dir}"
ls -l "${artifact_dir}" >"${acceptance_root}/artifact-list.txt"
```

Read the packaging script's usage if its accepted option spelling changes.
Before installation, record and verify the generated checksum from within the
artifact directory, because its checksum file uses the artifact basename:

```bash
cd "${artifact_dir}"
sha256sum -c ./*.sha256
sha256sum ./*.tar.zst >"${acceptance_root}/artifact-sha256.txt"
tar --zstd -xOf ./*.tar.zst BUILD_INFO \
  >"${acceptance_root}/artifact-build-info.txt"
```

The `BUILD_INFO` capture backend for this implementation must describe direct
Gamescope PipeWire DMA-BUF capture, not Desktop Wayland DMA-BUF discovery.

## Install and establish the service baseline

Install only the artifact whose digest was recorded:

```bash
cd /home/deck/SteamShine
artifact_path="$(find "${artifact_dir}" -maxdepth 1 -type f -name '*.tar.zst' -print -quit)"
./steamshine.sh install --artifact "${artifact_path}" \
  --non-interactive --yes --no-build --no-packages
systemctl --user show steamshine.service \
  -p MainPID -p NRestarts -p ActiveState -p SubState \
  -p ActiveEnterTimestamp -p ExecMainStartTimestamp \
  | tee "${acceptance_root}/service-installed.txt"
```

Copy the literal `MainPID`, `NRestarts`, and `ExecMainStartTimestamp` into the
operator note. This is the baseline used to detect a daemon exit across mode
transitions. `NRestarts=0` alone is insufficient if a manual reinstall changed
the PID, so always compare both PID and start timestamp.

Start a journal window at this point:

```bash
date --iso-8601=seconds | tee "${acceptance_root}/journal-window-start.txt"
journalctl --user -u steamshine.service -n 200 --no-pager -o short-precise \
  >"${acceptance_root}/journal-installed.txt"
```

## Common snapshot procedure

At every `before`, `connected`, and `after-disconnect` checkpoint, replace
`LABEL` with a short stable name such as `desktop-physical-before` or
`desktop-to-game-connected`:

```bash
label='LABEL'
mkdir -p "${acceptance_root}/${label}"
systemctl --user show steamshine.service \
  -p MainPID -p NRestarts -p ActiveState -p SubState \
  -p ActiveEnterTimestamp -p ExecMainStartTimestamp \
  >"${acceptance_root}/${label}/service.txt"
pgrep -a gamescope >"${acceptance_root}/${label}/gamescope.txt" || true
find /sys/class/drm -maxdepth 2 -name status -print -exec sed -n '1p' {} \; \
  >"${acceptance_root}/${label}/drm-status.txt"
pw-dump >"${acceptance_root}/${label}/pipewire.json" 2>&1 || true
journalctl --user -u steamshine.service -n 500 --no-pager -o short-precise \
  >"${acceptance_root}/${label}/steamshine-journal.txt"
latest_diagnostic="$(find /home/deck/.local/state/steamshine/session-diagnostics \
  -maxdepth 1 -type f -name 'session-*.json' -printf '%T@ %p\n' 2>/dev/null \
  | sort -n | tail -1 | cut -d' ' -f2-)"
if [[ -n "${latest_diagnostic}" ]]; then
  cp -- "${latest_diagnostic}" \
    "${acceptance_root}/${label}/latest-session-diagnostic.json"
fi
```

Also record these manual observations at every connected checkpoint:

- whether Moonlight displayed continuously changing video;
- negotiated resolution, refresh rate, SDR/HDR, and codec;
- audio present and synchronized;
- controller buttons and axes;
- keyboard key injection;
- mouse motion, buttons, and wheel;
- disconnect behaviour and whether reconnect worked.

A successful launch page, a non-black first frame, or positive input counters
alone does not prove final video output.

## Scenario matrix

Run every scenario from a known state. Disconnect Moonlight before changing
SteamOS mode or cable state unless the scenario explicitly tests disruption of
an active stream.

For the current development baseline, disable VRR before running this matrix
and keep it disabled for the complete evidence set. Do not inject a custom
Gamescope binary. The repeated-launch route contract and the three-cold-boot
minimum execution that covers the four required physical-output scenarios are
defined in
[`STEAMOS_CONNECTION_ROUTE_REDESIGN.md`](STEAMOS_CONNECTION_ROUTE_REDESIGN.md).
Run that compact matrix first. The broader scenarios below remain release
coverage and should not cause redundant repetitions of an already accepted
compact row.

At every launch, require one `SESSION_EVENT mode_decided` row containing the
selected `route` and `reason`. If the observed prerequisites differ from the
planned row, classify the actual observation before judging the route. In
particular, a verified stock Gamescope correctly outranks both a physical
Desktop and a retained owned session in automatic mode.

### A. Physical display in Desktop Mode

1. Connect the physical display and enter Desktop Mode.
2. Save `desktop-physical-before`.
3. Start the application from Moonlight and save
   `desktop-physical-connected`.
4. Verify changing video, audio, and all four input classes.
5. Disconnect Moonlight and save `desktop-physical-after-disconnect`.

Expected source: KWin PipeWire physical capture. No virtual Gamescope may be
created merely because the resident service was started in another mode.

This scenario is the latency gate for all later virtual-display testing. Run at
least 20 seconds of changing content while exercising controller, keyboard, and
pointer input, then require:

- `frame_age_at_encode_ms.p95 <= 2.0` and
  `frame_age_at_network_ms.p95 <= 5.0`;
- `input_queue_age_ms.p95 <= 1.0` with no dropped edge events;
- no sustained audio discontinuity or A/V drift;
- no repeated client IDR recovery pattern after the initial connection; and
- replacements, if any, remain deliberate latest-frame drops rather than an
  accumulating capture or network backlog.

Do not proceed to owned or attached Gamescope acceptance if physical KWin misses
this gate. The earlier accepted KWin baseline was approximately 2.7--3.2 ms
capture-to-network, while the rejected independently phased pacer measured
17.35 ms and held unique frames about 14.13 ms before encode.

### B. Desktop Mode to stock Game Mode

1. Begin after scenario A with Moonlight disconnected.
2. Record the service PID, restart count, and start timestamp.
3. Select **Return to Gaming Mode** and wait for the stock UI to settle.
4. From Moonlight, start the application.
5. Save `desktop-to-game-connected`, verify final video/audio/input, disconnect,
   and save `desktop-to-game-after-disconnect`.

Expected source: attached stock Gamescope. Expected event sequence includes:

```text
SESSION_EVENT mode_decided ... verified_existing_gamescope=true
SESSION_DISPLAY_ENDPOINT_READY origin=attached_existing
GAMESCOPE_SOURCE_ATTACHED origin=attached_existing
CAPTURE_SOURCE source=gamescope_pipewire
PIPEWIRE_DMABUF_DEVICE source=verified_render_node render_node=/dev/dri/renderD...
```

The PipeWire format must have positive dimensions, an encoder must initialize,
and encoded packets/bytes must increase. The baseline SteamShine PID and start
timestamp must remain unchanged and `NRestarts` must not increase.

With `steamos_stock_session_handoff=auto_idle`, repeat after stock Game Mode is
verified idle. A new application launch must acquire one owner-bound lease,
stop `gamescope-session.target` normally, and start an owned headless source at
the Moonlight geometry. End the application and require one stable stock Game
Mode restore. Then repeat with a real stock game running: the original
Gamescope, Steam, and game PIDs must remain unchanged and the route must stay
`attached_existing` without creating a lease.

### C. Stock Game Mode to Desktop Mode

1. Begin in stock Game Mode with Moonlight disconnected.
2. Record the service identity, then select **Switch to Desktop**.
3. Wait for Plasma, KWin, PipeWire, and the physical connector to settle.
4. Start the application from Moonlight and save
   `game-to-desktop-connected`.
5. Verify final video/audio/input, disconnect, and save
   `game-to-desktop-after-disconnect`.

Expected source: newly refreshed KWin PipeWire physical capture. Stale Game Mode
display variables and stale Gamescope PipeWire identities must not be reused.
The daemon PID and start timestamp must remain unchanged.

### D. Cold start directly into stock Game Mode

1. Reboot or log into stock Game Mode without first opening Desktop Mode.
2. Restore the same `acceptance_root` literal in a shell when possible.
3. Record `game-cold-before`, then start the application from Moonlight.
4. Record `game-cold-connected`, verify final video/audio/input, disconnect, and
   record `game-cold-after-disconnect`.

Expected source: attached stock Gamescope with no dependency on
`WAYLAND_DISPLAY=wayland-0`. A missing Desktop Wayland socket is normal here.

### E. No physical display and no stock Game Mode source

1. Disconnect the physical display while in a state without stock Game Mode
   Gamescope as an eligible capture source.
2. Save `monitorless-owned-before`.
3. Start the application from Moonlight and save
   `monitorless-owned-connected`.
4. Verify final video/audio/input, disconnect, and save
   `monitorless-owned-after-disconnect`.

Expected source: SteamShine-owned private Gamescope and an owner-only runtime
under the configured `XDG_RUNTIME_DIR`. The diagnostics must report an owned
origin. This is the scenario for `scripts/test-steamos-virtual-display.sh` or
`./steamshine.sh hardware-test --interactive` when their documented
preconditions are satisfied.

### E2. KDE fullscreen shared Big Picture canvas

1. Enter Desktop Mode with a connected physical output and verified live KWin.
2. Set `steamos_local_presentation=auto` and launch Big Picture from Moonlight.
3. Require `owned_gamescope_backend=wayland_nested`,
   `presentation=remote_and_local`, and `presentation_reason=verified_kwin_nested`.
4. Verify the fullscreen local window and Moonlight show the same virtual
   resolution and content while PipeWire reports the owned Gamescope producer,
   not a second KWin recapture.
5. Disconnect and reconnect. When geometry, HDR intent, backend, KWin endpoint
   generation, and presentation requirement are unchanged, the Gamescope PID
   and PipeWire object serial must be retained.

Repeat with `steamos_local_presentation=off`; the backend must be `headless`.
Repeat with `mirror` after making KWin unavailable; launch must be rejected with
`nested_wayland_unavailable`. For HDR, require the verified KWin endpoint to
advertise `wp_color_manager_v1` or `frog_color_management_factory_v1`; the
backend must then remain `wayland_nested`. Without a compatible color protocol,
automatic policy must preserve the remote HDR path with
`nested_hdr_unverified_remote_only`, while mandatory `mirror` rejects.

### E3. Idle Desktop Steam migration

1. In KDE, start Steam and leave it idle with no game scope containing a
   process other than a residual `reaper`.
2. Set `steamos_steam_migration=auto_idle`, then launch Big Picture.
3. Require the state sequence `checking_idle`, `shutting_down`, `migrated`.
   Verify the original PID/start-time disappears after Steam's normal
   `-shutdown` command and that the new Steam starts only inside the selected
   owned Gamescope. Repeat with `steamos_local_presentation=off` and require
   exact Moonlight geometry from the headless producer.
4. Repeat with a running game. Require HTTP 503 and
   `blocked_active_game`; the original Steam and game PIDs must remain alive.
5. Repeat with two Steam processes, unreadable procfs metadata, a changed KWin
   endpoint, and forced shutdown timeout. Each must return HTTP 503 without
   SIGTERM/SIGKILL. Timeout must remove only the newly owned session.

### F. Physical connector changes before the next launch

Test both directions with Moonlight disconnected:

1. Start without a physical display, connect it, wait for DRM/KWin to settle,
   then launch from Moonlight.
2. Start with a physical display, disconnect it, wait for DRM/KWin to settle,
   then launch from Moonlight.

Save a snapshot before the cable change, after it settles, while connected, and
after Moonlight disconnects. The next launch must choose the source that is
valid at request time. It must not reuse the preceding session's unverified
endpoint.

### G. Physical connector changes while attached to stock Game Mode

While Game Mode Gamescope is active, connect or disconnect the physical cable,
then launch from Moonlight. The stock Gamescope remains the capture authority;
SteamShine must not replace or signal it. Verify resolution and final output
because Gamescope may update its physical output topology independently.

### H. Physical Desktop display disappears during an active stream

Begin a physical KWin stream, save a connected snapshot, and disconnect the
display. Record whether the existing stream stops cleanly or Moonlight drops.
Reconnect by launching the application again and verify that source selection
uses the new state.

This scenario passes the current contract when failure is bounded, the daemon
stays resident, and a new launch automatically selects a valid source and
restores final output. Seamless in-stream KWin-to-Gamescope migration is not a
current acceptance requirement and must not be reported as implemented.

## Pass criteria

A scenario passes only when all applicable conditions hold:

- the service remains `active/running`;
- daemon PID and start timestamp remain stable across non-install transitions;
- `NRestarts` does not increase;
- the selected origin matches the scenario and is uniquely verified;
- attached stock Gamescope is never signalled or cleaned up by SteamShine;
- PipeWire negotiation reports positive dimensions and frames are captured;
- the selected GPU and verified render node agree;
- a hardware encoder initializes and encoded frames, packets, and bytes grow;
- Moonlight shows sustained final video, not merely a successful launch;
- audio, controller, keyboard, and mouse work;
- disconnect is clean and the subsequent launch can reconnect;
- diagnostics name the same origin and contain positive relevant counters;
- no stale Desktop Wayland endpoint is used in Game Mode.

Any fail-closed rejection is preferable to capturing another user's source,
the wrong GPU, or an ambiguous process, but it is still a functional failure
for the scenario and must be reported as such.

## Failure bundle before any recovery action

Run this immediately when the PID changes, Moonlight cannot connect, video is
black/frozen, input fails, or capture/encoding terminates:

```bash
failure_stamp="$(date +%Y%m%d-%H%M%S)"
failure_root="${acceptance_root}/failure-${failure_stamp}"
mkdir -p "${failure_root}"
chmod 700 "${failure_root}"
systemctl --user show steamshine.service \
  -p MainPID -p NRestarts -p ActiveState -p SubState \
  -p Result -p ExecMainCode -p ExecMainStatus \
  -p ActiveEnterTimestamp -p ExecMainStartTimestamp \
  >"${failure_root}/service.txt"
journalctl --user -u steamshine.service --since "-15 min" \
  --no-pager -o short-precise >"${failure_root}/steamshine-journal.txt"
journalctl --user --since "-15 min" --no-pager -o short-precise \
  >"${failure_root}/user-journal.txt"
coredumpctl --user --no-pager info steamshine \
  >"${failure_root}/coredump.txt" 2>&1 || true
pgrep -a gamescope >"${failure_root}/gamescope.txt" || true
pw-dump >"${failure_root}/pipewire.json" 2>&1 || true
find /sys/class/drm -maxdepth 2 -name status -print -exec sed -n '1p' {} \; \
  >"${failure_root}/drm-status.txt"
./scripts/diagnose-steamos-virtual-display.sh \
  >"${failure_root}/virtual-display-diagnosis.txt" 2>&1 || true
./scripts/collect-steamos-runtime-baseline.sh \
  >"${failure_root}/runtime-baseline.txt" 2>&1 || true
cp -a /home/deck/.local/state/steamshine/session-diagnostics \
  "${failure_root}/session-diagnostics" 2>/dev/null || true
```

If a helper prints a separate report directory, record that path in
`failure_root`. Inspect helper usage before retrying with options; do not assume
that stdout contains its complete report.

Classify the first server-side failure in this order:

1. daemon exited or restarted;
2. session/source identity verification failed;
3. display endpoint verification failed;
4. PipeWire node connection or format negotiation failed;
5. DMA-BUF import capability or GPU identity failed;
6. encoder initialization failed;
7. frames existed but packets/bytes did not increase;
8. network/client failure after positive server video counters;
9. certificate/TLS failure explicitly present in server logs.

For the previously observed Game Mode failure, the decisive evidence was
successful stock Gamescope selection followed by an attempted connection to
the vanished Desktop `wayland-0` during DMA-BUF capability discovery. The
Moonlight TLS/Security wording was secondary, not the root cause.

## Recovery and rollback

After the failure bundle is complete, a normal service restart is allowed for a
repeatability check:

```bash
systemctl --user restart steamshine.service
systemctl --user show steamshine.service \
  -p MainPID -p NRestarts -p ActiveState -p SubState \
  -p ActiveEnterTimestamp -p ExecMainStartTimestamp
```

If the installed build must be removed, use the installer's recoverable
rollback path rather than deleting version directories manually:

```bash
cd /home/deck/SteamShine
./steamshine.sh rollback
```

Record the rollback output and resulting service identity. A result obtained
after restart or rollback belongs to a new baseline and must not be merged with
the earlier PID-continuity claim.

## Acceptance report format

For each build, retain:

- Git commit plus the fact that the artifact contains dirty worktree changes;
- artifact absolute path and SHA-256;
- `BUILD_INFO` contents and ABI ceiling result;
- exact build image and build directory;
- test filters and pass counts;
- installed version directory;
- initial service PID, start timestamp, and restart count;
- one row for every scenario A through H: state, selected origin, video, audio,
  controller, keyboard, mouse, reconnect, service continuity, and evidence path;
- every failure bundle path and its first server-side failure;
- explicit remaining gates, including any case not run on hardware.

Do not describe a local dirty-worktree artifact as a release. A release result
must repeat the same matrix using the exact published artifact and record its
independent digest. Automated CI proves compilation, tests, packaging, and ABI
compatibility; it does not prove live Gamescope video, audio, input, cable
handling, or compositor transitions.

## Current 2026-07-28 continuation point

The installed acceptance artifact is:

```text
/home/deck/SteamShine/dist/game-mode-direct-gpu-fix/steamshine-steamos-x86_64-4fadb824ac996bdd492b1dfdc4839aa4b4665203.tar.zst
SHA-256: 3eda773effefb46765350159365c20bd695f9ffdd9d4bc68b85d33a4f629a6b3
```

Its Desktop Mode baseline was PID `79918`, start time
`2026-07-28 16:41:41 JST`, and `NRestarts=0`. The next unresolved gate is
scenario B: switch from Desktop Mode to stock Game Mode, launch from Moonlight,
and verify the verified-render-node event, positive capture/encode counters,
final video, audio, all input classes, and unchanged service identity. Treat
these numeric values only as the continuation baseline for this installed
artifact; a restart or reinstall creates a new baseline.
