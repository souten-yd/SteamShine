# Stream page and client profiles

SteamShine exposes a read-only stream view at the authenticated
`/steamshine/stream` route. It reuses the existing status facade and polls every
two seconds; closing the page or a browser failure cannot stop or own the media
path. The upstream Sunshine configuration remains available at
`/sunshine/config` as the rollback route, linked from the page header.

## What the page shows

The page is written for the person holding the client, not for the person
debugging the negotiator. It presents four levels of detail, in this order:

- A banner: whether a client is streaming, the delivered geometry, codec and
  dynamic range, and the current adaptive-bitrate congestion state.
- Four tiles with rolling sparklines: bitrate, frame rate, network latency
  (p99), and client packet loss.
- `Delivering to Moonlight`: client, delivered geometry, the client request when
  it differs, codec and profile, dynamic range, colour depth, capture source,
  and encoder backend.
- `Connection quality`: link state, bitrate target and ceiling, the learned
  next-session rate, applied bitrate adjustments, queue depths, and capture age.
- `Adjustments`: the stable fallback reasons, shown only when the delivered
  stream differs from the request.

The canonical four-stage negotiation state (requested, selected, active,
observed) remains the source of every field above and is unchanged in the
`/api/steamshine/v1/status` facade. The page selects from it rather than
printing it; `STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md` remains authoritative
for the schema itself.

Sender recording lives on the same page: it stores the exact encoded video
already being sent to the client, within a user-set capacity, without opening a
second encoder.

## Profiles

Profiles are stored in the user state directory as `stream-profiles.json`. The
document is schema-versioned, atomically replaced, owner-readable/writable only,
and limited to 64 entries. An entry matches only the exact paired-client ID,
network class, and current capability signature. A friendly device name never
selects a profile, and a changed capability signature disables the saved entry
instead of forcing unsupported codec or HDR behavior.

The first time a paired client starts a stream, RTSP records it through
`StreamProfileService::ensure_registered`. Registration only ever *adds* an
entry, under the neutral `default` network class, with every policy left
automatic and no ceilings — so a recorded client streams exactly as it did
before it was recorded, and `apply_stream_profile` changes nothing. An existing
entry is never modified by registration, which preserves the rule that a changed
capability signature disables a saved profile rather than silently refreshing
it. Registration does not infer LAN, Wi-Fi, or overlay state from an address.

The user reaches profiles through the gear control in the page header, which
opens a floating editor listing the recorded clients. Selecting one exposes the
bounded policy form; the client ID and capability signature are carried from the
recorded entry rather than typed. The editor also names the network class, and
that remains the only way a network class other than `default` is chosen.

RTSP derives the capability signature from the current codec, dynamic-range,
chroma, and HDR request. A matching active profile can only lower the FPS and
bitrate envelope or turn HDR off. A conflicting codec or HDR requirement yields
to current client facts and is recorded as a stable fallback reason. The final
bounded adaptive-bitrate aggregate updates only that exact active profile.

Profile fields are bounded geometry and FPS policy, codec and HDR policy,
bitrate ceiling, existing-setting quality preset, orientation, safe area, and a
learned next-session bitrate. Samples are not persisted; the adaptive controller
provides only the final aggregate learned value.

## Acceptance

Hardware accepted. The operator verified the page in a browser against Artifact
`steamshine-steamos-x86_64-1801b0cdea131efeff23b8d1cb7e3dab400c10ca` from
SteamOS Runtime Build run `30594131028` (PR #17, merged as
`95f6a2f49589c6ffff113215c143f6fb39c521ca`), installed locally with a matching
`BUILD_INFO.json` commit, an active service, and `NRestarts=0`. The accepted
scope is the live page — banner, tiles, delivered settings, connection quality —
the floating profile editor, and automatic first-connection registration leaving
the stream unchanged.

Two things stay outside this acceptance and keep their own gates:

- Adaptive bitrate still needs its controlled-loss Ethernet and Wi-Fi runs;
  reading its state on this page is not the same as exercising the controller.
- Geometry, quality-preset, orientation and safe-area selections are stored and
  displayed but not yet applied to the pipeline, so nothing about them was
  accepted here.

Re-running acceptance on a later Artifact means repeating the same checks; an
earlier pass is evidence about the implementation, not acceptance of a new head.
