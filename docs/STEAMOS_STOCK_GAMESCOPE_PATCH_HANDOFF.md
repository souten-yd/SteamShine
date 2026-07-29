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

Updated: 2026-07-29 JST

Repository:

```text
path: /home/deck/SteamShine
branch: fix/steamos-session-display-endpoint
baseline commit: cf8408a90e161739d340e5c6fbe697ef3ff9237b
PR: https://github.com/souten-yd/SteamShine/pull/11
PR state: open draft
```

State:

- The detailed implementation plan is recorded, but no Gamescope source has
  been fetched, built, installed or injected yet.
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

Next action after this documentation commit is pushed to PR #11:

1. Create a fresh external evidence directory under
   `/home/deck/.local/state/steamshine/stock-gamescope-investigation/`.
2. Capture the complete Stage 0 software, process, service, PipeWire, GPU,
   display and VRR baseline without changing the active session.
3. Determine whether the four-way unpatched A/B matrix can begin in the
   current mode. Before any required Game Mode transition, replace this section
   with the exact evidence path and operator steps.

## Mandatory pre-transition handoff block

Before every reboot or Game Mode transition, replace all placeholders and
commit or otherwise persist this block:

```text
timestamp:
current SteamOS mode:
current stage:
completed actions:
evidence directory:
SteamShine commit and binary digest:
Gamescope package/path/digest/build ID/capability:
test artifact path and digest, or NONE:
active gamescope-session override, or NONE:
rollback mechanism and armed state:
expected next boot/session path:
first verification commands after resume:
single operator action required:
success evidence:
failure evidence to collect before retry:
rollback steps:
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
