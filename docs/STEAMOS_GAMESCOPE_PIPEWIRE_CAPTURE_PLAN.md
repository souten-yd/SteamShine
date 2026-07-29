# Gamescope PipeWire capture plan

## Status and evidence

SteamShine can create an owned, headless Gamescope 3.16 session on the
SteamOS RX 9070 XT target.  The session resolves `/dev/dri/renderD128`, starts
Gamescope, and creates its private `gamescope-0` socket.  The existing WLR
capture provider cannot capture that socket because Gamescope does not expose
`zxdg_output_manager_v1` or `zwlr_export_dmabuf_manager_v1` there.  It must
not fall back to a desktop Wayland or desktop PipeWire source while an owned
session is active.

The intended path is therefore:

```text
owned Gamescope -> host PipeWire Video/Source node -> SteamShine PipeWire
consumer -> DMA-BUF -> Vulkan Video encoder -> Moonlight
```

## Implementation status

The original first sprint described here is implemented. SteamShine discovers
the `Video/Source` node published by owned Gamescope, joins it to the PipeWire
Client PID/UID, verifies the producer process identity and start time, and
opens a dedicated PipeWire core connection for capture. The capture consumer
requires DMA-BUF and feeds the Vulkan Video encoder without a CPU frame
readback.

On the SteamOS RX 9070 XT baseline this route produced positive captured-frame,
encoded-packet, encoded-byte, and IDR counters, and Moonlight video was
observed. Those observations remain historical acceptance evidence, not a
claim that every subsequent branch is accepted. In particular, the current
branch is being revalidated after fixes for PipeWire registry ordering, absent
Gamescope render-node metadata, and capability-restricted `/proc/<pid>/exe`
access. Local presentation and PipeWire two-consumer integration are separate
remaining work.

## Endpoint separation

The private Wayland runtime and the login user's PipeWire runtime have distinct
purposes and must remain distinct.

| Endpoint | Purpose | Typical value |
| --- | --- | --- |
| Private Wayland runtime | Owned Gamescope socket | `/run/user/<uid>/steamshine/session-<id>` |
| Host PipeWire runtime | User PipeWire daemon | `/run/user/<uid>` |
| PipeWire remote | User PipeWire socket name | `pipewire-0` |
| Host Pulse runtime | Audio client discovery | `/run/user/<uid>/pulse` |

SteamShine does not change its process-wide `XDG_RUNTIME_DIR`,
`PIPEWIRE_RUNTIME_DIR`, or `PIPEWIRE_REMOTE`.  Only the owned Gamescope and
application child environments receive the private Wayland runtime.  Their
PipeWire variables explicitly retain the host runtime and remote.

## Capture source policy

```text
force + verified owned Gamescope PipeWire Video/Source node
  -> gamescope_pipewire
force + no verified owned node
  -> fail closed
owned session + desktop capture source available
  -> never use desktop capture
normal desktop session
  -> retain existing backend selection
```

The node identity is `(PipeWire core, object serial)`.  Numeric node IDs are
used only to attach a stream after ownership is verified and are never a
persistent identity.

## Node verification

A candidate must satisfy all of the following:

1. `media.class` is `Video/Source`.
2. The producer PID equals the owned Gamescope PID.
3. The producer UID equals the SteamShine user.
4. The candidate belongs to the currently connected host PipeWire core.
5. Exactly one candidate matches.

`application.name`, `node.name`, and `node.description` containing
`gamescope` are diagnostic corroboration only.  They do not replace PID and
UID verification.

## Host PipeWire connection

The connection target is resolved in this order:

1. `steamos_pipewire_runtime` configuration;
2. SteamShine's original login runtime captured before the owned session;
3. `/run/user/<uid>`.

The remote name is resolved in this order:

1. `steamos_pipewire_remote` configuration;
2. original `PIPEWIRE_REMOTE`;
3. `pipewire-0`.

The runtime must be under `/run/user/<uid>`.  The resolved path must be a
current-user-owned UNIX socket.  SteamShine connects to it directly and passes
a duplicated connected descriptor to `pw_context_connect_fd()`.  After the
PipeWire core accepts that descriptor, PipeWire owns it; SteamShine must not
close it a second time.

## Lifecycle

```text
launch -> resolve GPU -> create private runtime -> resolve host PipeWire
endpoint -> start Gamescope -> private socket ready -> wait for verified node
-> attach PipeWire consumer -> first frame -> encode -> stream
```

Node disappearance during streaming calls `mark_capture_lost()` and lets the
existing teardown path stop the owned process group and clean the private
runtime.  Capture callbacks do not wait for child processes or delete files.

## Observability

The implementation emits structured, non-secret events for endpoint
resolution, node discovery, verification, PipeWire-core connection, stream
connection, format negotiation, first frame, and node disappearance.  It
records session ID, Gamescope PID, node ID, object serial, media class,
PipeWire runtime/remote, and render node.  It never records pairing PINs,
cookies, passwords, or credentials.

`scripts/diagnose-gamescope-pipewire.sh` collects the host PipeWire socket,
node properties, Gamescope process environment, private runtime state, and
`pw-cli` evidence. Set `STEAMSHINE_GAMESCOPE_PIPEWIRE_DUMP=1` to include the
full optional `pw-dump` evidence. It does not persist per-frame data.

## Delivery plan

### Sprint 1: endpoint separation and owned-node discovery

- Add configuration and session diagnostics for the host PipeWire endpoint.
- Supply host PipeWire variables to owned Gamescope and launched applications.
- Implement direct UNIX-socket connection and a PipeWire registry watcher.
- Verify candidate Video/Source nodes by owned PID, UID, and object serial.
- Add fake-registry and endpoint-resolution tests plus the diagnostic script.

Acceptance evidence is a verified Gamescope-owned Video/Source node with its
object serial recorded.  Frame capture is explicitly out of this sprint.

### Sprint 2: shared PipeWire consumer

- Split portal descriptor acquisition from the existing PipeWire consumer.
- Connect the verified node using the direct core descriptor.
- Negotiate formats and record the first received frame.
- Handle node removal and cleanup.

### Sprint 3: zero-copy video acceptance

- Require DMA-BUF, validate fourcc/modifier and same-GPU import.
- Produce Vulkan Video H.264 packets and an IDR frame.
- Verify Moonlight video, then audio/input, monitorless operation, and ten
  reconnect cycles.

## Test strategy

Unit tests cover endpoint validation, socket ownership/type/connect failures,
node matching, object-serial protection against node-ID reuse, source
selection, and descriptor ownership.  Integration tests use separate temporary
Wayland and PipeWire runtimes and a fake owned Video/Source node.  Hardware
tests require positive evidence for node ownership, first frame, DMA-BUF,
encoded packets, bytes, and IDR frames; an operator confirmation alone is not
acceptance evidence.

## Stock Game Mode producer remediation plan

### Scope and architecture decision

The current producer investigation retains the direct path:

```text
stock Game Mode Gamescope -> PipeWire DMA-BUF -> SteamShine
  -> Vulkan Video/VA-API -> Moonlight
```

The first implementation must not clone stock Gamescope into an owned
Gamescope, change the physical display mode, add a KMS capture path, introduce
a multi-frame FIFO, or automatically fall back based only on a low observed
source frame rate. Static content can legitimately produce very few unique
frames. The owned-private and KWin paths remain unchanged controls.

SteamShine already sends Gamescope's private requested-size property and keeps
at most one pending PipeWire DMA-BUF. These are regression requirements, not
new work in this investigation.

### Stage 0: freeze and record the baseline

Use PR #11 commit `cf8408a90e161739d340e5c6fbe697ef3ff9237b` as the
SteamShine baseline until the initial Gamescope experiment is complete. Record
the SteamShine binary and configuration, Gamescope package version, binary
digest, ELF build ID and capabilities, producer PID and command line, PipeWire
node identity, GPU, Mesa, PipeWire, kernel, output mode, physical refresh and
verified VRR state. Preserve successful KWin and owned-private session reports
as controls.

### Stage 1: measure the unpatched producer

Measure four 60-second intervals: VRR on with Steam menu, VRR on with a
continuously changing game, VRR off with Steam menu, and VRR off with the same
game. Record actual presented game FPS, negotiated capture maximum FPS,
physical refresh, verified VRR state, PipeWire buffer and unique-frame counts,
duplicate runs, first-buffer latency, frame ages, queue depths, input counts,
producer identity, and Gamescope build identity.

Only continuously changing intervals are eligible for the producer-rate gate:

```text
observed source FPS >= 0.90 *
  min(actual presented game FPS, negotiated capture maximum FPS)
```

Do not include static, loading, or application-limited intervals.

### Stage 2: reproduce the SteamOS Gamescope build

Resolve the exact SteamOS source revision, distribution patches, Meson
options, compiler and linked-library requirements. Build and smoke-test an
unmodified artifact before applying any functional patch. A behavior change
caused by the unmodified local build blocks the experiment until the build
difference is explained.

Verify the ELF interpreter, required symbol versions, dynamic dependencies,
runtime options, binary digest, build provenance and `cap_sys_nice=eip`
parity. Store a manifest beside every versioned artifact.

### Stage 3: inject a reversible test binary

Never overwrite `/usr/bin/gamescope` or edit
`/usr/lib/steamos/gamescope-session`. Install each artifact below a
root-owned, non-user-writable, persistent versioned directory after verifying
the mount and SteamOS update behavior. A candidate location is:

```text
/var/lib/steamshine/gamescope-builds/<build-id>/bin/gamescope
```

The stock launcher currently ends with `exec gamescope`, so a user-service
drop-in may prepend a directory containing only the verified test executable
to the PATH of `gamescope-session.service`. It must not change the global user
environment or shadow any other launcher command. Preserve the stock launcher,
systemd notification, startup socket, environment-file generation,
`drm_janitor` drop-in and service relationships.

Arm a one-shot rollback before entering Game Mode. A launch is healthy only
when the running executable, digest and build ID match the artifact, Gamescope
signals readiness, its expected PipeWire node appears, and the Steam launcher
starts. If the health marker is absent after the bounded startup interval,
atomically disable the override and make the next normal session transition or
reboot use stock Gamescope. Do not loop manual starts: the stock unit refuses
manual start and SteamOS tracks repeated short sessions.

### Stage 4: test the isolated upstream vblank fix

Apply only Valve commit
`b3ecd00c035fdcd6e99ee5aaa88211bdb6d3cd3c`, which drives PipeWire
capture from real vblank rather than the vblank timer. Repeat the Stage 1
matrix. Reaching the producer-rate gate in a VRR-on continuously changing game
is success even if menu-only, overlay-only, cursor-only, or consumer-reconnect
cases remain incomplete.

If it does not fix the continuously changing game, do not stack scene or
cursor changes. First use a diagnostic-only build with rate-limited counters
for PipeWire paint calls, missing buffers, missing focus, AppID mismatch,
unchanged signatures, publish attempts, publish successes, stream generation,
and first-publish latency.

### Stage 5: guarantee the first frame per consumer generation

Replace process-lifetime function-static commit tracking with state owned by
the PipeWire capture stream. Force one composition after a transition to
streaming, consumer-generation change, format or requested-size negotiation,
focus-AppID change, producer stream recreation, or output-geometry change.
After that first frame, unchanged content must not be recomposed every vblank.

Require an unchanged scene to deliver a real initial frame across at least 20
connect/disconnect cycles. Producer keepalive is deferred unless a separate
client-liveness failure proves it necessary.

### Stage 6: add the minimal capture signature

Keep the existing composition path and expand only its invalidation signature.
For every captured focus, override, or Steam overlay surface include a stable
window/surface identity, commit ID, mapped/visible state and opacity. Also
include focus AppID and requested-size, output-geometry and color/HDR
generations. A commit ID alone cannot distinguish replacement windows with the
same counter value.

Add notifications or external overlays only when they are also included in
the captured composition. A shared scene-builder refactor is separate later
work after these small patches are validated.

### Stage 7: embed the cursor

Capture one render-thread-safe cursor snapshot containing the image serial,
position, hotspot, visibility, relative-mode state, scale, rotation and
generation. Use the same snapshot for signature comparison and actual
composition into the capture texture. Validate cursor-only movement,
transforms, relative mode and suppression of any duplicate client cursor.

### Stage 8: improve SteamShine diagnostics

After producer behavior is established, add Gamescope binary path, package
version, build ID, digest, consumer generation, first-frame latency, and
specific producer failure reasons to session diagnostics. Do not infer patch
presence from a version string alone. Managed artifacts may use their manifest
and digest; unknown external binaries remain explicitly unknown.

Input-to-next-unique-frame timing is accepted only with a deterministic visual
test whose surface changes immediately for each injected event. Ordinary game
or Steam-menu input is not deterministic enough for that metric.

### Regression and stop gates

Every stage must preserve requested-size negotiation, the one-buffer
latest-frame policy, encoder and network latency, input delivery, the KWin
path, the owned-private path, stock-session ownership boundaries, and bounded
logging. With no PipeWire consumer, the patch must add no capture work; with a
consumer and a static scene, it must not force continuous recomposition.

Stop and diagnose rather than adding another patch when any of these occurs:

- the unmodified reproduced Gamescope differs from stock behavior;
- the isolated vblank patch does not improve a continuously changing game;
- injection changes the stock launcher contract or creates restart loops;
- artifact identity, capability, ABI or PipeWire producer identity is unclear;
- local presentation, HDR, VRR, KWin or owned-private behavior regresses.

Keep the eventual Gamescope changes as independent patches: vblank backport,
first-frame generation reset, overlay invalidation, and embedded cursor. This
plan does not authorize creating an external issue or pull request.
