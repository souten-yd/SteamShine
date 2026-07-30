# SteamShine documentation

The active planning context is intentionally limited to the documents below. Load these first and do not automatically load retired plans from Git history.

## Canonical project documents

1. [`PROJECT_ROADMAP.md`](./PROJECT_ROADMAP.md)
   - SteamShine-primary product direction
   - Moonlight arbitrary resolution/FPS
   - HDR, codec, quality, adaptive bitrate, profiles, and release order
   - PR boundaries, counterarguments, merge gates, and prohibited shortcuts

2. [`STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md`](./STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md)
   - detailed requested/selected/active/observed model
   - source-aware geometry and refresh handling
   - end-to-end HDR gates
   - H.264/HEVC/AV1 policy using existing encoders
   - frame pacing, bitrate envelope, adaptive controller, persistence, tests, and rollback

3. [`CODEX_GOAL_MODE_STREAMING.md`](./CODEX_GOAL_MODE_STREAMING.md)
   - exact Goal Mode execution instructions for the next implementation PR
   - required goal order, tests, hardware gates, commit sequence, and stop conditions

4. [`IMPLEMENTATION_STATUS.md`](./IMPLEMENTATION_STATUS.md)
   - concise evidence-based status of what is merged, locally tested, hardware tested, or still pending

## Current implementation records

- [`STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md`](./STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md)
  - implemented SteamOS virtual-session lifecycle and ownership behavior

- [`STEAMOS_GAMESCOPE_PIPEWIRE_CAPTURE_PLAN.md`](./STEAMOS_GAMESCOPE_PIPEWIRE_CAPTURE_PLAN.md)
  - Gamescope PipeWire/DMA-BUF capture architecture and implementation record

- [`configuration.md`](./configuration.md)
  - configuration reference

- [`STREAM_PROFILES_UI.md`](./STREAM_PROFILES_UI.md)
  - exact client/network/capability profile matching and four-stage Stream UI

Implementation records describe the code that exists. They do not override the canonical roadmap for future work.

## Documentation lifecycle

Historical pre-implementation plans and temporary handoff files were removed from the active tree after their valid decisions were consolidated. See [`archive/README.md`](./archive/README.md) for the retired file list. Full history remains available through Git.

Rules:

- keep one active roadmap and one active detailed design for a workstream;
- update `IMPLEMENTATION_STATUS.md` using measured facts, not planned claims;
- remove temporary handoff files after the owning PR is merged;
- do not retain multiple contradictory Goal plans;
- preserve important historical evidence in PR descriptions, commits, reports, and Git history instead of default Codex context.
