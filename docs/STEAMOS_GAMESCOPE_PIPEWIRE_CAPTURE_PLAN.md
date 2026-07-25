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

## Scope

This document records the design before implementation.  The first sprint is
limited to discovering and verifying the PipeWire Video/Source node published
by the owned Gamescope process.  It does not claim frame capture, DMA-BUF
import, encoding, or Moonlight video success.

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

`scripts/diagnose-gamescope-pipewire.sh` will collect the host PipeWire socket,
node properties, Gamescope process environment, private runtime state, and
optional `pw-dump`/`pw-cli` evidence.  It does not persist per-frame data.

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
