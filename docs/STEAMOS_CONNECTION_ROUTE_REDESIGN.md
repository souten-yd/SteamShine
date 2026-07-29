# SteamOS connection-route redesign

## Scope

This design fixes source selection for repeated Moonlight launches while the
physical output and stock Game Mode Gamescope can change independently. VRR is
disabled for implementation and acceptance. VRR-on compatibility is deferred
until the rest of the streaming product is complete or SteamOS ships and
validates the upstream Gamescope vblank fix.

The design does not migrate an active stream between capture sources. Every
Moonlight application launch takes a fresh observation, selects exactly one
route, and keeps that route until disconnect or a bounded capture failure.

## Route model

`select_session_route()` is the only policy function. It returns one of five
routes:

| Route | Common meaning | Source-specific owner |
| --- | --- | --- |
| `physical_desktop` | Use a currently capturable physical compositor output | KWin/Portal capture and display configuration |
| `attached_existing` | Attach to a newly verified stock Game Mode source | Gamescope discovery, resident Steam endpoint, PipeWire and EIS identity |
| `retained_owned_private` | Reuse a compatible owned source | Owned lifecycle and identity revalidation |
| `new_owned_private` | Create a private headless Gamescope source | Owned process/runtime/bootstrap lifecycle |
| `reject` | Fail closed without switching to another source | Common launch error reporting |

Automatic mode uses this strict priority at every launch:

```text
verified stock Gamescope
  -> capturable physical Desktop
  -> compatible retained owned Gamescope
  -> new owned Gamescope
  -> reject when the host cannot create one
```

`force` and explicit `owned_private` select only retained or new owned
Gamescope. Explicit `existing_gamescope` selects only a verified stock source
and otherwise rejects. Feature-disabled and mode-off configurations preserve
the normal physical capture path.

## Common and individual responsibilities

The common launch path owns only:

1. current DRM connector and active-CRTC observation;
2. current compositor-capture capability observation;
3. current verified stock Gamescope presence;
4. retained-owned compatibility observation;
5. pure route selection and one stable reason;
6. release of a previously selected source when the route changes;
7. common diagnostics, failure propagation, and disconnect accounting.

The physical route does not read Gamescope help, create a runtime, inspect a
Steam endpoint, or open a Gamescope PipeWire node. The attached route never
creates or signals Gamescope and never removes its runtime. The owned routes
alone read the installed Gamescope option set, create private runtime state,
start a process group, and perform owned cleanup.

Capture, encoder, network, audio, and input pipelines remain shared after a
source has been selected. Source-specific code supplies a verified display and
input endpoint; it must not duplicate the downstream streaming pipeline.

## Required scenarios

The following table is the acceptance contract for `auto`/`auto` with VRR off.
“Stock appears” means a current-user `gamescope-session.service` PipeWire
source passes executable, PID/start-time, cgroup, UID, GPU, and node identity
validation before the second launch.

| Scenario | Observation at launch | Expected route |
| --- | --- | --- |
| 1a. Boot physical OFF, first Moonlight launch | no physical capture, no stock, no retained owned | `new_owned_private` |
| 1b. Disconnect, stock appears, second launch | verified stock plus retained owned | `attached_existing`; release only the owned source |
| 2a. Boot physical ON, first launch | physical capture, no stock | `physical_desktop` |
| 2b. Disconnect, second launch unchanged | physical capture, no stock | `physical_desktop` |
| 3. Boot physical OFF, turn it ON, then launch | physical capture available at request time | `physical_desktop` |
| 4a. Boot physical OFF, launch and disconnect | no physical capture or stock | `new_owned_private`, then retained |
| 4b. Turn physical ON, second launch | physical capture plus retained owned, no stock | `physical_desktop`; stop only the owned source |

If stock Gamescope is also verified in scenario 4b, automatic priority selects
`attached_existing`; that is scenario 1b rather than a physical-route failure.
The evidence must record this distinction instead of forcing an expected
physical result against a different observed state.

## Minimum hardware trials

Use three cold boots rather than one boot per matrix row:

1. **OFF boot A:** run scenario 1a, disconnect, wait for stock Gamescope, then
   run 1b. Stop if stock does not become verifiably available; record that
   prerequisite failure instead of repeating Moonlight.
2. **ON boot:** run 2a and 2b in one boot.
3. **OFF boot B:** turn the display on before Moonlight and run scenario 3.
   Disconnect, turn the display off and wait for KWin/DRM to settle, run 4a,
   disconnect, turn the display on, and run 4b. This combines scenarios 3 and
   4 because route selection deliberately has no boot-history input.

One connection per required row is sufficient. Repeat only after the first
failure bundle identifies a transient external prerequisite rather than a
deterministic product failure.

Before the matrix, record one SteamShine PID, start timestamp, binary digest,
configuration snapshot, and VRR-off observation. At each row record only the
delta snapshot, selected route event, newest session diagnostic, final
video/audio/input observation, and service identity. This avoids repeatedly
collecting identical static evidence.

## Failure handling

On the first black screen, disconnect, daemon PID change, missing audio/input,
wrong route, or source disappearance:

1. do not retry, restart SteamShine, recreate an owned runtime, or change the
   physical output again;
2. capture service identity and journal, coredump metadata, DRM state,
   Gamescope identities, PipeWire graph, selected endpoint, and the newest
   session diagnostic;
3. classify the failure as observation, route selection, source preparation,
   shared capture/encode, audio/input, or teardown;
4. write a corrective design and focused automated regression before another
   hardware attempt.

The SteamShine daemon must remain alive across every successful row. A
fail-closed route rejection is safer than capturing an ambiguous source, but
it is still a scenario failure.

## VRR-related cleanup decision

The temporary patched-Gamescope service override, guard, collector, and
rollback timer are not part of this design and remain disabled. No route input
or branch depends on VRR state. Low-latency latest-frame handoff, immediate
unique-frame encoding, bounded queues, and duplicate keepalive pacing are
shared pipeline behavior with physical VRR-off measurements behind them; they
are retained unless a VRR-off regression demonstrates that one is unnecessary.

Owned Gamescope help probing is source-specific and occurs only after
`new_owned_private` is selected. Physical, attached, and retained launches do
not perform it.

## Automated verification

GTest covers every policy branch and models all four scenario sequences as
successive immutable observations. Lifecycle tests continue to cover owned
creation, retention, compatible reuse, explicit non-retention, endpoint
generation, and owned-only cleanup. Hardware acceptance remains necessary for
real DRM hotplug, KWin readiness, stock Gamescope publication, PipeWire
DMA-BUF, encoder, audio, and input behavior.
