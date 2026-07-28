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
verified Gamescope -> host PipeWire capture-output node -> SteamShine PipeWire
consumer -> same-GPU DMA-BUF -> Vulkan Video/VA-API encoder -> Moonlight
```

## Implementation status

The original first sprint described here is implemented. SteamShine discovers
the current `Stream/Output/Video` node or compatible `Video/Source` node published by owned or attached Gamescope, joins it to the PipeWire
Client PID/UID, verifies the producer process identity and start time, and
opens a dedicated PipeWire core connection for capture. The capture consumer
queries importable DMA-BUF formats through GBM/EGL on the selected render node
and feeds the Vulkan Video or VA-API encoder on that same GPU. This discovery
does not consult `WAYLAND_DISPLAY`; a resident service may therefore cross
from Desktop Mode to Game Mode without retaining a capture dependency on the
vanished Desktop compositor.

Gamescope can pause and resume an active PipeWire stream when the private
requested capture size changes. That transition rebuilds the capture and
encoder dimensions against the same verified Gamescope PID/start-time identity;
it is not classified as node disappearance and cannot select a Desktop fallback.

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
force + verified Gamescope PipeWire capture-output node
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

1. `media.class` is `Stream/Output/Video` or the compatible `Video/Source` spelling.
2. The producer PID equals the selected Gamescope PID.
3. The producer UID equals the SteamShine user.
4. The candidate belongs to the currently connected host PipeWire core.
5. The live producer PID, UID, start time, and executable still match discovery.
6. Any producer render-node property matches the selected GPU; omission is allowed only because the selected GPU is verified independently.
7. Exactly one candidate matches.

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
launch -> resolve GPU -> attach or create Gamescope -> resolve host PipeWire
endpoint -> wait for verified node -> open an independent consumer descriptor
-> bind DMA-BUF discovery to that descriptor's render node -> negotiate format
-> first frame -> same-GPU hardware encode -> GameStream packet -> Moonlight
```

Node disappearance during streaming calls `mark_capture_lost()`. The existing
teardown path stops and cleans only a SteamShine-owned session; an attached
vendor Game Mode session is detached and never signaled. Capture callbacks do
not wait for child processes or delete files.

Before PipeWire connects, the consumer opens the verified `/dev/dri/renderD*`
path with `O_NOFOLLOW`, confirms it is still a character device, and creates a
GBM/EGL capability display. It advertises only formats with explicit modifiers
accepted by that GPU, plus the existing memory-buffer alternative on the same
verified PipeWire stream. Failure never falls back to Desktop Wayland, KMS,
X11, another PipeWire node, another GPU, or a software encoder.

## Observability

Game Mode acceptance must reject an audio-only/input-only session whose video
diagnostics contain no captured frame ages and whose encoded-frame count is
entirely composed of duplicates. For the stock Gamescope producer, the
consumer supplies Gamescope's private requested-size property in addition to
the standard PipeWire size range so capture is bounded to the Moonlight
request. The first delivered buffer is logged with its data type, descriptor,
chunk size, stride, offset, flags, and plane count. If the producer reaches the
streaming state without delivering any buffer, SteamShine emits
`PIPEWIRE_FIRST_FRAME_TIMEOUT reason=no_producer_buffer`; this distinguishes
producer starvation from DMA-BUF import or encoder failures.

Gamescope 3.16.23.4 is edge-triggered: it retains the last focus and override
commit IDs across PipeWire consumer connections and does not publish an
unchanged frame to a later connection. Encoder capability validation uses a
dummy image and the independently verified render node, so SteamShine defers
the Gamescope PipeWire stream during those probes. Desktop KWin and portal
sources retain live probe negotiation. The first Gamescope producer
connection is consequently the real capture display rather than a disposable
H.264, HEVC, or AV1 probe, preserving the initial frame for Moonlight.

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
