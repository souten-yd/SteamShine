# PipeWire Frame Pacing Implementation Plan

## Producer-buffer lease ordering

The callback-driven consumer must release the PipeWire lease held by an idle
capture image before waiting for its next producer callback. Gamescope provided
a three-buffer pool during hardware acceptance. Waiting first allowed all three
buffers to remain leased in reusable image objects, so the producer could not
publish a fourth callback and the consumer could not reach the code that
released a lease. The observed stream consequently stopped after exactly three
buffers even though the source was repainting continuously.

`pipewire_display_t::snapshot()` now acquires and resets a reusable image before
`wait_for_frame()`. The DMA-BUF remains leased through conversion as before, but
an idle prior-generation lease cannot exhaust the producer pool. Latest-frame
replacement and callback-driven capture remain unchanged.

## Status and scope

This document is reviewed and kept current before implementation changes are
made on `fix/pipewire-frame-pacing`. At the maintainer's explicit request, tested
increments are pushed to the fork branch currently backing draft PR #10. Commits
remain independently scoped so the frame-pacing work can be split from autostart
history later without mixing source changes inside a single commit.

The observed failure is a 60 FPS client request with only 47.6 or 32.7 unique
PipeWire frames per second. The encoder then re-encodes the preceding converted
frame because its default minimum target is half the requested rate. Existing
`capture_frames_replaced`, encoder queue, and network counters do not explain the
missing unique frames: replacement and network drop counts were zero in the
measured sessions.

PC capacity is not assumed to be the cause. CPU, GPU, encoder, and power data are
still measured so that the change does not hide a capacity regression.

## Required behavior and invariants

1. A valid unique source frame is encoded immediately. If capture or encode
   falls behind, an obsolete pending frame is replaced explicitly so current
   input feedback takes priority over preserving every historical generation.
2. Duplicate deadlines use a rational rate. `60/1` and `60000/1001` remain
   distinct, do not accumulate rounding drift, and never create a catch-up
   burst. The default duplicate rate retains Sunshine's earlier half-requested
   FPS behavior instead of re-encoding unchanged content at the full rate.
3. A unique frame or forced IDR re-anchors the duplicate clock at its immediate
   output time. The next duplicate is therefore one full period later and an
   expired independent deadline cannot create a back-to-back burst.
4. Static or confirmed no-damage content uses a low-rate keepalive instead of
   continuously re-encoding at the requested FPS. IDR and reconnect requests
   bypass the static delay.
5. PipeWire callbacks do bounded work, return obsolete buffers promptly, and
   preserve DMA-BUF ownership. No per-frame SSD logging or unconditional CPU
   full-frame copy is introduced.
6. Queues are bounded and every deliberate latest-frame replacement is counted
   with a stable reason. No FIFO may accumulate several frames of visual latency.
7. Existing physical Desktop, attached Game Mode, and existing Gamescope refresh
   behavior is unchanged. A `gamescope -r` experiment is allowed only for an
   owned private session, behind a disabled-by-default feature flag, after source
   PTS/sequence evidence demonstrates a producer-side need.

### Guarantee boundary

It is mathematically impossible to guarantee finite memory, fixed output rate,
preservation of every source frame, and minimum latency when input remains faster
than the encoder. This implementation deliberately chooses minimum latency:
obsolete pending frames are replaced and counted rather than becoming a visual
backlog. Contract violations or a slow encoder enter an explicit
`consumer_limited` state and remain bounded.

## Considered objections and decisions

- **Latest frame can discard an irregular game frame.** Accepted. Hardware
  acceptance showed that the ordered four-frame handoff and independently
  phased deadline prioritize historical completeness over input feedback. The
  streaming handoff therefore retains only the newest unconsumed frame and
  counts replacements.
- **Strict CFR cannot invent smooth motion.** Accepted. Duplicates stabilize
  client delivery cadence and make source limitation visible; they do not add
  motion information. Optical-flow interpolation is excluded.
- **Holding several PipeWire buffers can starve the producer.** Accepted. The
  callback will not simply retain an arbitrary queue of `pw_buffer` objects.
  The implementation must first prove a GPU-safe ownership/import strategy. If
  it cannot, conversion/import occurs promptly into the existing bounded image
  pool before returning the PipeWire buffer.
- **Damage metadata is optional and producer-specific.** Absence of VideoDamage
  is `unknown`, not `no damage`. Static policy starts only from affirmative
  no-damage evidence or a tested conservative heuristic.
- **PTS can repeat or be absent.** Header sequence, PTS, damage, corruption flags,
  and an internal monotonic generation are recorded separately. A duplicate PTS
  alone does not turn a damaged/new sequence into a duplicate.
- **Always encoding 60 duplicates wastes power and bitrate.** Accepted. Unique
  frames are event-driven and immediate. Source gaps use the configured minimum
  FPS target, which defaults to half the requested rate, and confirmed static
  content uses a lower keepalive. Reconnect/IDR output remains immediate.
- **The issue may be producer pacing rather than capture code.** Producer
  interarrival measurements precede any Gamescope refresh experiment. Network
  or PC capacity is not presumed without evidence.

## Architecture

### 1. Bounded session diagnostics

Add a fixed-memory, 500 ms aggregation path and a RAM ring. Only a final session
JSON summary is written. It contains:

- `pipewire_buffers_received`, `pipewire_unique_frames`,
  `pipewire_redundant_pts`, `pipewire_no_damage_frames`;
- `capture_deadline_misses`, `encoded_unique_frames`,
  `encoded_duplicate_frames`, `duplicate_run_max`;
- source and encode interarrival p50/p95/p99/max;
- requested rational FPS, negotiated FPS range, and observed unique FPS;
- bounded queue depth/overflow and `source_limited`, `consumer_limited`, or
  `producer_limited` status reasons.

The PipeWire callback samples `SPA_META_Header` sequence/PTS,
`SPA_META_VideoDamage`, and buffer/chunk flags before returning a buffer.

### 2. Rational absolute-deadline pacer

Introduce a platform-independent duplicate pacer with an integer/rational time
model. Unique and forced recovery output bypasses the timer, then re-anchors its
next deadline. When no source frame arrives, each decision is one of duplicate,
static keepalive, wait, or skipped deadline. Late calls advance directly to the
first future deadline while counting missed deadlines; they never emit a burst.

### 3. Ordered source handoff

Use a one-slot latest-frame handoff for both PipeWire DMA-BUF delivery and the
shared capture-to-encode boundary. A monotonic generation identifies accepted
unique frames, while replacement counters make deliberate low-latency loss
visible. Corrupted and confirmed redundant/no-damage buffers are classified and
counted without becoming unique generations.

DMA-BUF lifetime is an implementation gate: tests and code review must show that
every descriptor remains valid through conversion and that every PipeWire buffer
is returned exactly once. The queue capacity remains configurable for tests but
has a small fixed production maximum.

### 4. Capture/stream clock separation

Remove the fixed 1000 ms unique-frame wait from the PipeWire stream clock. Source
arrival wakes the encoder and emits immediately. Only a source gap waits for a
duplicate deadline; shutdown and IDR/reconnect requests also remain immediate.
A lack of source frames no longer stops output timing, while compositor/pacer
phase mismatch cannot hold a real frame for another refresh interval.

PipeWire format negotiation already advertises the requested maximum frame rate
to the producer. Its capture thread therefore waits directly on the producer
callback and does not sleep on a second client-rate phase before dequeuing the
latest buffer. Hardware reconnect evidence showed that the redundant clock could
hold every 60 FPS Gamescope frame for about 12.7 ms even though every queue depth
remained one; callback-driven dequeue removes that phase-dependent fixed delay.

### 5. Static and IDR policy

Motion-to-static and static-to-motion transitions are explicit state changes.
Static keepalive defaults conservatively and will be tuned by tests and hardware
data. A queued IDR/reconnect request schedules the current converted frame at the
next immediate safe encode opportunity rather than waiting for a keepalive.

### 6. KScreen subprocess bounds

Set explicit timeouts for every KScreen subprocess: read 5 seconds, apply 10
seconds, and revert 10 seconds. Convert `TimeoutExpired` to stable errors. Apply
failure reports whether virtual fallback remains possible. Revert failure does
not block daemon teardown and retains the state file until restore succeeds.

The systemd service intentionally starts from `default.target` so it remains
available before a physical KDE session and in Game Mode. Consequently, its
process environment can predate the current physical Wayland session. Before a
physical KScreen operation, the helper refreshes only the graphical selectors
from the systemd user manager, which Plasma updates for the active session. A
refreshed `WAYLAND_DISPLAY` is accepted only when it is a relative socket name,
the socket exists below the service's existing `XDG_RUNTIME_DIR`, it is a Unix
socket, and it is owned by the service UID. `DISPLAY` is accepted only in the
canonical local X display form. The helper never imports arbitrary variables,
never hard-codes `wayland-0` or `:0`, and never changes the owned-private or
attached-Game-Mode environment. The environment query has the same bounded
five-second read timeout and fails through the existing stable fallback path.

This refresh is deliberately performed at application launch rather than by
restarting the daemon or binding the service to `graphical-session.target`.
That preserves early autostart, pairing, Game Mode, and headless operation while
making physical Desktop preparation use the session that actually exists at the
time of the Moonlight request.

### 7. Owned-session application surface selection

The private Desktop fallback is a minimal non-singleton Plasma folder surface;
it is not a complete Plasma session. It is launched only for a capture-only
application that has neither a primary command nor detached commands. A
commandless application with detached launch commands, such as Steam Big
Picture, relies on those commands and may show the existing bounded black startup
frame until its first real surface appears. Launching the folder fallback in
parallel would allow it to cover the requested application's surface and is
therefore excluded.

This decision is based on launch semantics, not a localized application name,
icon, or hard-coded Steam command. Physical Desktop and attached existing Game
Mode remain capture-only and never receive the private folder fallback.

### 8. Session-scoped launch environment

The parsed application environment is an immutable baseline. Every application
launch rebuilds its child environment from that baseline before adding the
current Moonlight request and, when selected, the current owned-session runtime
selectors. Owned Gamescope values such as `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`,
`PIPEWIRE_RUNTIME_DIR`, and `PIPEWIRE_REMOTE` must never leak into a later
physical Desktop or attached Game Mode launch.

This reset happens before prep commands so KScreen state is always stored under
the host runtime directory. It also avoids fixing individual variables with an
ever-growing denylist: any future per-session selector is discarded naturally
when the next launch starts from the parsed baseline. Unit coverage exercises an
owned-to-physical transition and verifies both removal of the owned selectors
and preservation of configured baseline variables.

## Delivery sequence

1. Record baseline producer PTS/sequence/damage and current encoded cadence.
2. Add pure diagnostics/statistics and rational pacer types with unit tests.
3. Integrate PipeWire metadata classification and the bounded latest-frame handoff.
4. Separate capture arrival from output deadlines and add static/IDR policy.
5. Add KScreen timeout behavior, launch-time graphical-environment refresh, and
   integration tests.
6. Prevent the private Desktop fallback from obscuring detached applications.
7. Rebuild child environments per launch so private-session selectors cannot
   contaminate later physical or attached launches.
8. Run synthetic PipeWire integration cases and full CI.
9. Run hardware acceptance. Only then evaluate the owned-session refresh flag if
   producer evidence still shows a Gamescope-side limit.
10. Push independently tested commits to the fork branch backing draft PR #10,
   then update its title/body and matching artifact after the full acceptance
   matrix is complete.

## Verification matrix

Unit coverage includes 60 FPS unique input; irregular 48 FPS; 30 FPS; static/no
damage; 59.94 versus 60; 90/120 FPS; missed deadlines; duplicate-run accounting;
duplicate PTS; corrupt frames; IDR during static; queue overflow; and KScreen
timeouts. Changed methods target 100% coverage and all new C++ APIs require
Doxygen documentation.

Integration coverage uses a synthetic PipeWire producer with programmable
timestamps for 60 seconds of motion, 10 seconds of static, motion-to-static-to-
motion transitions, and slow encoder/network consumers.

Hardware coverage records source unique FPS, encoded FPS, duplicate fraction,
interarrival p50/p95/p99/max, frame-time p95/p99, capture-to-encode latency, GPU
encoder utilization/power, bitrate, client decode/render queue, and visible
stutter for Input Latency Visualizer, a rotating game camera, physical Desktop,
owned Gamescope, existing Game Mode, H.264/HEVC/AV1, and Ethernet/Wi-Fi.

## Merge gates

- Continuous 60 FPS motion either removes the long-term 32--48 FPS unique-source
  shortage or reports the correct source-limited cause.
- Encoded cadence is stable and duplicate frames/runs are measurable.
- Every deliberate latest-frame replacement is explicit, bounded, and measured;
  no source or encoded FIFO accumulates several frames of visual latency.
- Physical gaming performance remains within measurement error.
- KScreen failure returns within its timeout and teardown does not wait minutes.
- Full CI is green, the artifact matches the tested commit, and hardware testing
  uses the installed systemd service.
