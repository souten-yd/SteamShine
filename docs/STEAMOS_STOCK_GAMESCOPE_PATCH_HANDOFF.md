# Stock Gamescope PipeWire patch handoff

## Purpose

This file is the durable resume point for the stock Game Mode Gamescope
producer investigation. Update it before every reboot, transition into Game
Mode, installation, or other action that can interrupt the working session.
The full design and acceptance gates are in
`docs/STEAMOS_GAMESCOPE_PIPEWIRE_CAPTURE_PLAN.md`.

## Resume protocol

After a conversation or machine restart:

1. Read this file and the stock producer remediation section of the plan.
2. Run `git status -sb` and verify the expected branch and commit.
3. Read the current checkpoint below. Do not repeat a completed destructive or
   state-changing action.
4. Verify the recorded artifact digest, active override, rollback state,
   Gamescope executable, service status and current SteamOS mode.
5. If actual state differs from this file, collect evidence and update this
   file before continuing.

Never overwrite `/usr/bin/gamescope`, edit the stock SteamOS launcher, signal a
stock Gamescope on SteamShine teardown, or enter Game Mode with an unarmed test
override.

## Current checkpoint

Updated: 2026-07-29 10:48 JST

Repository:

```text
path: /home/deck/SteamShine
branch: fix/steamos-session-display-endpoint
functional baseline commit: 1b0ec872ac9000bc686c53676579856fd0adba4b
current documentation commit before this checkpoint: 1b0ec872ac9000bc686c53676579856fd0adba4b
PR: https://github.com/souten-yd/SteamShine/pull/11
PR state: open draft
```

State:

- The detailed implementation plan is recorded and Stage 0 evidence is saved
  at `/home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-095521-stage0`.
- Valve tag `3.16.23.4`, peeled commit
  `2b79e07b3da1723c7e5c5f44f18de36c6cb78b9e`, and all recorded
  submodules have been fetched to
  `/home/deck/.local/src/steamshine-gamescope/gamescope-3.16.23.4`.
- An unmodified optimized/LTO build completed in `build-unpatched` using GCC
  15.1.1, glibc 2.41+r65, PipeWire 1.6.4 and the current SteamOS 3.8 package
  repository dependencies. Gamescope and subproject tests passed 122/122.
- The stripped unmodified binary is
  `/home/deck/.local/src/steamshine-gamescope/gamescope-3.16.23.4/build-unpatched/src/gamescope.stripped`.
  Its SHA-256 is
  `85a6259924ccb38abe3ebc474bf7facc53d83f815548a0a2ec8677f82f57e877`
  and its ELF build ID is `ca337d0297ab1da35b47e2771163741ae7295305`.
- The unmodified binary and stock binary have identical required shared-library
  names and CLI options. Both selected the RX 9070 XT, created a headless
  compositor and PipeWire node, and exited successfully in the same smoke test.
- The SteamOS packaging GitLab requires authentication. The official package
  archive exposes PKGBUILD digest
  `2230e8b0f392521ddc29ee240beaa6cfaff25ac105fe8474da9163163e06fb33`,
  but distribution-local patch absence remains unverified. Functional parity
  is therefore still gated on the unpatched Game Mode test.
- No systemd override or rollback timer has been created.
- The first non-enabled evidence collector was stopped after the failed case.
  Its unit is `steamshine-stock-gamescope-stage1-capture.service` and its
  incomplete report directory is
  `/home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-103335-stage1-unpatched-vrr-on-menu`.
  It remained at `waiting-for-stock-gamescope` because stock Gamescope changes
  its process name to `gamescope-wl`, while the collector incorrectly required
  `pgrep -x gamescope`. The corrected collector uses the verified systemd
  `MainPID` and records Steam-family identities and launch rejection snapshots.
- Game Mode ran from 10:35:39 to 10:38:46 JST. Three Moonlight launch attempts
  were rejected before capture with `A verified resident Steam environment is
  required for the selected Game Mode session`; no session diagnostic was
  created, so this is not a valid Stage 1 performance result.
- The rejection was caused by endpoint selection applying the broad Steam
  singleton classifier to a game `reaper` retained outside the vendor launcher
  cgroup. The source fix validates the unique current-user `steam` executable
  for the resident endpoint, continues to reject an outside or duplicate Steam
  executable, and leaves the independent singleton placement check unchanged.
  Related GTest groups passed 34/34, 6/6, and 4/4; the Python integration suite
  passed 17/17.
- The tested fix artifact is
  `/home/deck/SteamShine/dist/resident-steam-endpoint-fix-20260729-1047/steamshine-steamos-x86_64-1b0ec872ac9000bc686c53676579856fd0adba4b.tar.zst`,
  SHA-256
  `3fd459b24fd5adef345774f6bd21c6392ef9a2747f8e5266b90db207e99ed54e`.
  The installed binary digest is
  `7b51f316e3551cddeb7299ce7c2c1c0e0d8357a4acfd56dd85b35029523373b6`.
  SteamShine baseline is PID `292981`, start `2026-07-29 10:47:27 JST`,
  `NRestarts=0`, `active/running`; KWin capture and Vulkan H.264/HEVC/AV1
  probes passed and configuration remains `auto`/`auto`.
- The corrected collector is armed at PID `293210`, report
  `/home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-104752-stage1-unpatched-vrr-on-menu`,
  status `waiting-for-stock-gamescope`.
- `/usr/bin/gamescope` remains the stock SteamOS binary.
- Installed package observed before implementation:
  `gamescope 3.16.23.4-1`.
- Stock executable SHA-256 observed before implementation:
  `7bbc654019ed17a8cf3637d221c2fd1cb59a4098198de2493337fe5629adbea2`.
- Stock ELF build ID observed before implementation:
  `0baea3ee9ff9f8d5df2fcbf7f8fcb7881e8eab09`.
- Stock capability observed before implementation:
  `cap_sys_nice=eip`.
- The source branch contains unrelated untracked reports and build output.
  Stage only explicit files; never use `git add -A` for this worktree.

Next action:

1. Use Steam's normal **Return to Gaming Mode** action. Do not reboot and do not
   restart SteamShine.
2. Wait at least 30 seconds after Game Mode is fully visible. The corrected collector
   performs the executable and service verification automatically; no Game
   Mode console is required.
3. Confirm VRR is enabled in the Game Mode display settings, then open
   Moonlight and run the `Desktop` application for 60 seconds while navigating
   the Steam menu continuously. Disconnect Moonlight after the interval.
4. Wait 30 seconds after disconnect so the post-session snapshot can finish,
   then return to Desktop Mode normally and resume this investigation. Do not
   start a second Moonlight session or change VRR before the evidence is
   inspected.

## Mandatory pre-transition handoff block

Before every reboot or Game Mode transition, replace all placeholders and
commit or otherwise persist this block:

```text
timestamp: 2026-07-29 10:48 JST
current SteamOS mode: Desktop Mode, KDE Wayland
current stage: Stage 1, unpatched stock A/B, before corrected case 1 retry
completed actions: plan pushed; Stage 0 captured; unpatched build/tests/smoke passed; invalid pre-capture attempt inspected; resident endpoint fix installed; corrected collector armed
evidence directory: Stage 0 /home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-095521-stage0; invalid case /home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-103335-stage1-unpatched-vrr-on-menu; active retry /home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-104752-stage1-unpatched-vrr-on-menu
SteamShine commit and binary digest: source 1b0ec872; installed 7b51f316e3551cddeb7299ce7c2c1c0e0d8357a4acfd56dd85b35029523373b6; service PID 292981, start 2026-07-29 10:47:27 JST, NRestarts=0
Gamescope package/path/digest/build ID/capability: 3.16.23.4-1; /usr/bin/gamescope; 7bbc654019ed17a8cf3637d221c2fd1cb59a4098198de2493337fe5629adbea2; 0baea3ee9ff9f8d5df2fcbf7f8fcb7881e8eab09; cap_sys_nice=eip
test artifact path and digest, or NONE: NONE; unpatched build exists but is not installed or injected
active gamescope-session override, or NONE: NONE
rollback mechanism and armed state: not applicable because stock Gamescope remains selected; corrected evidence collector PID 293210 is manually active, static, and non-mutating
expected next boot/session path: normal SteamOS Return to Gaming Mode using /usr/bin/gamescope
first verification commands after resume: read active retry status.txt; inspect game-mode-before-moonlight and after-moonlight-disconnect snapshots; validate session-diagnostic.json; systemctl --user show steamshine.service -p MainPID -p NRestarts
single operator action required: Return to Gaming Mode; wait 30 seconds; verify VRR on; stream Desktop while continuously navigating the Steam menu for 60 seconds; disconnect; wait 30 seconds; return to Desktop Mode; resume
success evidence: corrected collector status capture-complete; stock executable identity; Game Mode services active; SteamShine baseline PID unchanged with NRestarts=0; one new session diagnostic of at least 60 seconds
failure evidence to collect before retry: collector status and both snapshots when present; user-unit journal; Gamescope PID/cmdline/executable; SteamShine PID/restart count; any new session diagnostic
rollback steps: none; no Gamescope override or system binary change exists. Stop steamshine-stock-gamescope-stage1-capture.service if the case is abandoned.
```

## Test-artifact ledger

No Gamescope test artifact exists yet. Add one immutable row before use.

| Purpose | Source revision | Patch set | Path | SHA-256 | Build ID | Capability | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Stock reference | SteamOS package 3.16.23.4-1 | distribution | `/usr/bin/gamescope` | `7bbc654019ed17a8cf3637d221c2fd1cb59a4098198de2493337fe5629adbea2` | `0baea3ee9ff9f8d5df2fcbf7f8fcb7881e8eab09` | `cap_sys_nice=eip` | Baseline |

## Chronological progress

### 2026-07-29: plan fixed before implementation

- Confirmed the direct stock Gamescope PipeWire DMA-BUF architecture.
- Confirmed that Gamescope's requested-size property and SteamShine's
  one-buffer latest-frame policy are already implemented on PR #11.
- Confirmed that the stock user unit launches
  `/usr/lib/steamos/gamescope-session`, and the launcher resolves Gamescope via
  `exec gamescope` rather than an absolute executable path.
- Chose a service-local, versioned, root-owned artifact injection design with
  a one-shot rollback. The final persistent directory remains gated on mount
  and SteamOS update-behavior verification.
- No implementation or machine-state change had occurred when this checkpoint
  was written.

### 2026-07-29 10:07 JST: Stage 0 and unpatched build completed

- Saved the non-secret Desktop baseline under the Stage 0 evidence directory.
  SteamShine was PID 2362, active with `NRestarts=0`; stock Game Mode services
  and Gamescope were inactive.
- Confirmed `/var` is a writable, persistent-candidate ext4 filesystem, while
  the SteamOS root filesystem remains read-only. No `/var/lib/steamshine`
  directory has been created yet.
- Downloaded the official SteamOS package archive and verified that its
  `usr/bin/gamescope` digest exactly matches the installed stock executable.
- Built the exact Valve `3.16.23.4` tag in a disposable SteamOS-compatible
  container. The container uses current SteamOS repository packages but a
  dedicated build-only pacman configuration with signature enforcement
  disabled because the rootless container cannot read the host's mode-700
  pacman keyring. Repository transport is HTTPS; source and output revisions
  and digests are recorded. Do not treat this container as a package trust
  boundary.
- The unpatched build passed 122 tests, matched stock CLI and dynamic-library
  requirements, and passed a side-by-side owned headless smoke test. It is not
  capability-enabled and must not be injected in its current source-tree path.
- Next is the unmodified stock Game Mode A/B baseline. No vblank patch has been
  applied yet.

### 2026-07-29 10:34 JST: console-free case-1 capture armed

- Corrected the transition procedure because the local development console is
  unavailable in Game Mode.
- Installed and manually started a user-level evidence collector before the
  transition. It waits for the exact stock `/usr/bin/gamescope` and both vendor
  Game Mode services, waits another ten seconds for settlement, captures the
  pre-Moonlight snapshot, then waits for exactly the first new SteamShine
  session diagnostic and captures the post-disconnect state.
- The collector is not enabled at login, does not restart either service, does
  not consume the Gamescope stats FIFO, and makes no launcher, override, stock
  binary, configuration, or VRR change.
- The operator no longer needs a Game Mode terminal. The only Game Mode work is
  the normal transition, confirming VRR is on, waiting 30 seconds, and running
  one 60-second continuously navigated Steam-menu stream.
