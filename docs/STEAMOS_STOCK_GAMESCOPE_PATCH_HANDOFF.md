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

Updated: 2026-07-29 10:07:45 JST

Repository:

```text
path: /home/deck/SteamShine
branch: fix/steamos-session-display-endpoint
functional baseline commit: cf8408a90e161739d340e5c6fbe697ef3ff9237b
current documentation commit: 0ba0d33487d62b168eac00c375ec965a0e65c349
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

Next action after this checkpoint is pushed to PR #11:

1. Use Steam's normal **Return to Gaming Mode** action. Do not reboot and do not
   restart SteamShine.
2. Resume this investigation after Game Mode is fully visible.
3. Before opening Moonlight, verify the stock Gamescope executable and capture
   the commands listed in the pre-transition block below.
4. Run the first 60-second unpatched matrix case: VRR on, Steam menu with
   continuous visible navigation. Save the resulting SteamShine session JSON
   and journal before changing VRR or starting another case.

## Mandatory pre-transition handoff block

Before every reboot or Game Mode transition, replace all placeholders and
commit or otherwise persist this block:

```text
timestamp: 2026-07-29 10:07:45 JST
current SteamOS mode: Desktop Mode, KDE Wayland
current stage: Stage 1, unpatched stock A/B, before case 1
completed actions: plan pushed; Stage 0 captured; unpatched build/tests/smoke passed
evidence directory: /home/deck/.local/state/steamshine/stock-gamescope-investigation/20260729-095521-stage0
SteamShine commit and binary digest: source 0ba0d334; installed digest must be recaptured after resume
Gamescope package/path/digest/build ID/capability: 3.16.23.4-1; /usr/bin/gamescope; 7bbc654019ed17a8cf3637d221c2fd1cb59a4098198de2493337fe5629adbea2; 0baea3ee9ff9f8d5df2fcbf7f8fcb7881e8eab09; cap_sys_nice=eip
test artifact path and digest, or NONE: NONE; unpatched build exists but is not installed or injected
active gamescope-session override, or NONE: NONE
rollback mechanism and armed state: not applicable because stock Gamescope remains selected
expected next boot/session path: normal SteamOS Return to Gaming Mode using /usr/bin/gamescope
first verification commands after resume: pgrep -xo gamescope; readlink -f /proc/<pid>/exe; tr '\0' ' ' </proc/<pid>/cmdline; systemctl --user show gamescope-session.service steam-launcher.service steamshine.service -p Id -p ActiveState -p SubState -p MainPID -p NRestarts
single operator action required: select Return to Gaming Mode, then resume this task without starting Moonlight
success evidence: Game Mode visible; gamescope-session and steam-launcher active; /proc/<pid>/exe is /usr/bin/gamescope; SteamShine remains active with NRestarts=0
failure evidence to collect before retry: user-unit status and journal, Gamescope PID/cmdline/executable, SteamShine PID/restart count
rollback steps: none; no override or system file change exists. Return to Desktop Mode normally if Game Mode fails.
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
