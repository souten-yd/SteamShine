# SteamOS Stock Session Handoff Plan

## Status and Scope

This plan defines the implementation and hardware acceptance work for lending an
idle SteamOS stock Game Mode session to a SteamShine-owned headless Gamescope.
It is intentionally written before implementation because the transition may
end the current graphical login, restart SDDM, or reboot the host.

The feature applies only when all of the following are true:

- a uniquely verified stock `gamescope-session.service` source exists;
- the requested application prefers an owned Gamescope canvas;
- the configured handoff policy is `auto_idle`;
- stock Steam and its process identity are readable and unambiguous; and
- no active Steam game scope is present.

An active game is never migrated or stopped. SteamShine attaches to the verified
stock Gamescope and mirrors it for the lifetime of that Moonlight session. An
unknown activity state also keeps the stock session unchanged and uses the
verified attached source; if the source itself is not verifiable, launch fails
closed.

## User-visible Policy

Add a bounded configuration value:

```ini
steamos_stock_session_handoff = attach
```

Supported values:

- `attach`: preserve current behavior and always attach a verified stock Game
  Mode source;
- `auto_idle`: attach while a game is active, but temporarily replace a
  uniquely verified idle stock session with an owned headless session using the
  Moonlight-requested geometry.

The default remains `attach` so an upgrade cannot begin ending graphical
sessions without an explicit operator choice. Hardware acceptance on this host
will set `auto_idle` together with `steamos_local_presentation=off`.

## Non-negotiable Safety Invariants

1. Never send `SIGTERM` or `SIGKILL` directly to stock Gamescope, Steam, or a
   game. Stop only `gamescope-session.target` through the user systemd manager.
2. Revalidate the stock Gamescope PID/start time, Steam PID/start time, exact
   systemd cgroup components, and game-scope state immediately before acquiring
   the handoff lease.
3. If a non-reaper process is found in an `app-steam-*.scope` or
   `steam-app-*.scope`, keep the original stock PID and select
   `attached_existing`.
4. Ambiguous or unreadable activity metadata must not authorize a destructive
   transition. A still-verified stock source may be attached read-only.
5. Only the SteamShine-owned process group and runtime directory may be removed
   during failure recovery.
6. The stock launcher must resume automatically after normal disconnect,
   explicit cancellation, owned launch failure, SteamShine crash, or reboot.
7. A stale, malformed, symlinked, wrong-owner, or PID-reused lease must never
   suppress stock Game Mode indefinitely.
8. Never change capture producer, geometry, or input target during an active
   Moonlight stream. If a mirrored game exits, reconsider handoff only on the
   next application launch.

## Existing v1.2 Regressions That Must Stay Fixed

### First-connection HDR

The original cold-headless defect occurred because the first `serverinfo`
response was produced before a real HEVC/AV1 Main10 path had been probed. The
v1.2 startup preflight prepares a real HDR-capable owned source before HTTP when
the ordinary headless probe fails. A later route bug then destroyed that
preflight source by misclassifying it as stock.

The handoff implementation must therefore:

- never acquire or release a stock lease during startup encoder preflight;
- preserve the existing startup-preflight-owned route priority;
- carry the first Moonlight `hdrMode` intent into the same owned launch instead
  of requiring a reconnect;
- launch the patched, configured Gamescope with the requested HDR and exact
  output geometry;
- require a 10-bit PipeWire producer and `hdr_ready` before accepting the first
  hardware HDR session; and
- reject or report a real SDR fallback rather than labeling it HDR.

### Controller interruption and stock restart storms

The v1.2 investigation recorded 105 connectorless stock Gamescope failures,
106 Game Mode start attempts, and 106 SDDM display removals during one
152-second owned stream. Those repeated graphical-session rebuilds interrupted
held controller state. The installed guard currently suppresses this loop only
while every physical DRM connector is absent.

During a leased handoff, the guard must also hold the vendor launcher even when
a physical connector is present. There must be at most the intentional stop and
one restore transition, with no repeated `Starting Gamescope Session`, SDDM
`Removing display`, or competing stock Steam process while owned streaming is
active. Existing lazy desktop-mouse creation must remain intact so the false
`js0` device does not return. The owned EIS endpoint and virtual gamepad must be
newly verified after the stock endpoint disappears.

## Lease and Guard Design

Use an owner-only runtime lease below the already validated SteamShine runtime
root, for example:

```text
/run/user/1000/steamshine/stock-session-handoff.lease
```

The lease contains a schema version, boot ID, SteamShine PID, process start
time, and monotonically increasing handoff generation. It is created with
`O_CREAT|O_EXCL|O_NOFOLLOW`, mode `0600`, written completely, synchronized, and
atomically renamed. The runtime directory and lease must be owned by the
current UID and must not be symlinks.

The Game Mode guard waits while either condition is true:

- every non-writeback physical connector is disconnected; or
- a valid live handoff lease names the current SteamShine owner PID and start
  time in the current boot.

If the owner disappears, its PID start time changes, or the boot ID differs,
the guard removes only the validated lease path and resumes the unmodified
vendor launcher. Invalid lease content is logged and ignored rather than
holding Game Mode. Reboot naturally clears `/run/user`, but boot-ID validation
remains defense in depth.

The guard remains the effective `ExecStart` for
`gamescope-session.service`; no file under `/usr` is replaced. Installation,
rollback, and uninstall must update or remove the guard and drop-in through the
normal artifact workflow.

## Handoff State Machine

Expose a bounded state and reason in the virtual-display status:

```text
inactive
assessing
attached_active_game
attached_unknown
lease_acquired
stopping_stock
stock_stopped
owned_active
restoring_stock
restored
failed
```

The transition is serialized with the existing virtual-session manager lock
and a generation token. Long-running systemd waits occur outside the lock, then
state and identities are revalidated before committing the result.

### Idle stock to owned headless

1. Discover one verified stock Gamescope and its PipeWire identity.
2. Inspect current-user Steam and game scopes.
3. Revalidate all process identities and require a stable idle observation.
4. Create the handoff lease before requesting the stock stop so SDDM's
   `Relogin=true` path cannot race a replacement stock launcher.
5. Run `systemctl --user --wait stop gamescope-session.target` with a bounded
   timeout.
6. Require the original Gamescope and Steam identities and PipeWire node to
   disappear. If they remain, release the lease and restore stock.
7. Refresh KWin/stock discovery so no stale endpoint can be selected.
8. Start owned headless Gamescope at the Moonlight width, height, refresh, and
   HDR intent; verify its Wayland, PipeWire, GPU, and EIS identities.
9. Launch Steam Big Picture only inside that verified owned endpoint.

### Active or unknown game state

Do not create a lease or stop a target. Keep the original stock Gamescope PID,
PipeWire serial, Steam, and game processes unchanged. Attach capture and input
to the verified stock endpoint. Continue using that producer until disconnect
even if the game exits during the stream.

### Restore

When the owned session is actually destroyed—not merely temporarily retained
for reconnect—perform the following:

1. stop and reap the owned process group;
2. remove only its verified runtime directory;
3. release and synchronize the lease;
4. ensure `gamescope-session.target` has a start job, unless no physical
   connector is present and the normal headless guard condition still applies;
5. wait for a new stock Gamescope and Steam identity when a physical output is
   connected; and
6. report `restored` only after the stock source is independently verifiable.

Restore failure must remain visible in status and the journal, but must not
restart SteamShine in a loop. The guard's owner-liveness recovery is the final
fallback if SteamShine exits before completing these steps.

## Process Execution Boundary

Systemd transitions must use a fixed executable and fixed arguments; no shell
command is constructed from configuration. Child processes receive the
existing user-manager D-Bus address, have bounded deadlines, and are reaped.
Timeout handling may terminate only the helper process, not the stock target
members. Unit names are compile-time constants.

## Diagnostics and Doxygen

Add Doxygen for every new enum, structure, helper, public method, state field,
parameter, and return value. Journal events must include the policy, decision,
handoff generation, old stock PID/start time, owned PID, lease state, systemd
result, restore result, and bounded failure reason. The web status schema must
remain bounded and must not expose arbitrary lease contents or command output.

The session diagnostic must retain the selected source origin. An attached
active-game stream remains `attached_existing`; a successful handoff is
`owned_private` with `owned_gamescope_backend=headless` and the exact requested
capture geometry.

## Automated Test Matrix

### Pure C++ policy and lifecycle tests

- `attach` always selects verified stock;
- `auto_idle` plus active game selects stock attach without mutation;
- `auto_idle` plus unknown activity selects verified stock attach;
- `auto_idle` plus stable idle selects handoff only for a real application
  launch, never startup encoder preflight;
- PID reuse, missing cgroup data, duplicate Steam, and unreadable metadata do
  not authorize handoff;
- retained owned sessions do not reacquire or release the lease;
- failure and cancellation restore only an owned source;
- active streams never live-switch after game exit; and
- state/reason serialization is stable and bounded.

### Guard and installer integration tests

- connected connector without a lease starts the vendor launcher;
- disconnected connector holds it;
- connected connector with a live lease holds it;
- owner death, PID reuse, boot-ID change, malformed content, wrong mode, and a
  symlinked lease cannot hold it indefinitely;
- releasing a live lease resumes the launcher exactly once;
- install, upgrade, rollback, and uninstall manage the executable and drop-in;
- ShellCheck and Bash syntax pass.

### Existing regression suites

- startup HDR encoder preflight and first-launch route tests;
- HDR negotiation and stream-geometry tests;
- Gamescope EIS and gamepad lifecycle/hold diagnostics;
- Steam process placement and game-scope tests;
- virtual-session lifecycle and retained-session timeout tests;
- Web status/configuration tests; and
- the complete `test_sunshine` run, separating environmental fixture skips
  from individual failures.

## Hardware Acceptance and Reboot Gates

All deployable binaries use the immutable image and digest from
`ci/steamos/image.lock`, are packaged by
`scripts/package-steamos-artifact.sh`, and pass host `ldd` before installation.
Install only with the normal artifact installer so rollback remains available.

Before every reboot or Game Mode transition, record the artifact hash, current
and rollback version paths, config hash, boot ID, service PID/restart count,
journal capture command, and the next continuation step in
`docs/GAMESCOPE_SHARED_DISPLAY_RESUME.md`.

Acceptance cases:

1. **Idle stock, first HDR connection:** boot into physical stock Game Mode,
   make no preliminary Moonlight connection, then launch HDR Big Picture once.
   Require one handoff, exact client geometry, HEVC Main10/HDR10,
   `hdr_ready`, changing frames, and no reconnect.
2. **Held input:** during the same owned stream, hold a stick direction and an
   A-button action long enough to cover the former interruption interval.
   Require no unexplained release, zero input drops, one virtual controller,
   verified owned EIS, and no stock/SDDM restart loop.
3. **Active game fallback:** start a real game in stock Game Mode, record all
   identities, then connect. Require `attached_existing`, unchanged stock/game
   PIDs, working video/audio/input, and no lease.
4. **Unknown-state fallback:** inject or reproduce unreadable/ambiguous activity
   metadata. Require no stock stop; attach only if the source itself remains
   verifiable.
5. **Normal restore:** end an owned Moonlight application. Require owned-only
   cleanup followed by one new stable stock Gamescope and visible physical Game
   Mode.
6. **Crash restore:** terminate SteamShine during a leased owned session after
   evidence capture. Require the guard to detect owner death, resume stock, and
   systemd to restart SteamShine without a restart storm.
7. **Cable changes:** disconnect and reconnect the physical output during idle,
   attached, owned, and restore states. The connector guard and lease condition
   must compose without launching competing stock sessions.
8. **Cold boots:** perform three boots covering stock idle, no connector, and a
   prior crash-recovery state. Require no persistent lease, no missing library,
   no service restart loop, and a usable rollback path.

Kernel evidence must be checked for AMDGPU page faults, ring timeouts, GPU
reset, VRAM loss, and Vulkan device loss. A handoff is not accepted merely
because the client connects.

## Delivery Phases

1. Commit and push this plan as a reviewable checkpoint on a new branch in the
   `souten-yd/SteamShine` fork.
2. Implement policy/state and pure tests without enabling handoff by default.
3. Implement the lease, guard recovery, fixed systemd helper, diagnostics, and
   installer tests.
4. Build and run focused and full validation in the pinned SteamOS image.
5. Package, run host `ldd`, install through the normal installer, and set this
   host to `auto_idle` only after rollback metadata is recorded.
6. Execute the hardware matrix, updating the persistent resume checkpoint
   before disruptive transitions and reboots.
7. Push validated implementation and evidence to a draft PR in the fork. Never
   create a PR or issue in the LizardByte organization.

