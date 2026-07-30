# Stream negotiation profiles and UI

SteamShine exposes a read-only four-stage stream view at the authenticated
`/steamshine/stream` route. It reuses the existing status facade and polls every
two seconds; closing the page or a browser failure cannot stop or own the media
path. The upstream Sunshine configuration remains available at
`/sunshine/config` as the rollback route.

The stages are:

- Requested: client request and advertised capability fields known to the core.
- Selected: source, geometry/content rectangle, codec/color/rate decisions, and reasons.
- Active: observed capture and encoder backend state.
- Observed: bounded FPS, queue, and latency aggregates.

Profiles are stored in the user state directory as `stream-profiles.json`. The
document is schema-versioned, atomically replaced, owner-readable/writable only,
and limited to 64 entries. An entry matches only the exact paired-client ID,
network class, and current capability signature. A friendly device name never
selects a profile, and a changed capability signature disables the saved entry
instead of forcing unsupported codec or HDR behavior.

The user explicitly marks one network class per client for the next connection;
SteamShine never guesses LAN, Wi-Fi, or overlay-network state from an IP address.
RTSP derives the capability signature from the current codec, dynamic-range,
chroma, and HDR request. A matching active profile can only lower the FPS and
bitrate envelope or turn HDR off. A conflicting codec or HDR requirement yields
to current client facts and is recorded as a stable fallback reason. The final
bounded adaptive-bitrate aggregate updates only that exact active profile.

Profile fields are bounded geometry and FPS policy, codec and HDR policy,
bitrate ceiling, existing-setting quality preset, orientation, safe area, and a
learned next-session bitrate. Samples are not persisted; the adaptive controller
provides only the final aggregate learned value.

Hardware acceptance remains required on the final integrated Artifact: open the
page through localhost and LAN, run a 1080p60 stream, verify all four sections,
save and reset two network-class profiles for one client, change the capability
signature, and confirm the upstream Sunshine UI remains usable.
