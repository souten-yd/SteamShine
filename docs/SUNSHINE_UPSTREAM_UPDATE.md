# Sunshine stable update: v2026.914.233613

## Scope

Integrate upstream Sunshine stable tag `v2026.914.233613`
(`63d35f702ee9e362e43263742981836ec0710384`) into SteamShine v1.7
(`52b49a2a7f25d78fc94123a87e2d1a536fe09eee`). Preserve SteamOS session
management, Gamescope/PipeWire capture, Vulkan Video encoding, input routing,
the SteamShine management UI, and the verified Gamescope HDR socket-identity fix.

The common ancestor is `ae45c61535bb3029e9a61590266289e217cb907e`
(2026-07-22). The upstream stable release adds 174 commits and changes 268 files.
An initial merge preview reports conflicts in 41 files, concentrated in Linux
input, capture/encoding, shared input/video interfaces, Web UI, and packaging.

## Integration and validation

1. Resolve conflicts by retaining SteamShine behavior while incorporating upstream
   runtime fixes, including Linux startup hardening and FFmpeg Vulkan queue handling.
2. Review the upstream Linux input backend change against SteamShine's existing
   Gamescope input routing and user-space input implementation.
3. Reconcile dependency pins with the immutable SteamOS build image. Preserve
   the host GLIBC/GLIBCXX/Qt ABI ceilings; do not deploy a rolling-distribution build.
4. Run focused compilation and relevant gtest filters before the release build.
5. Run the broader regression, installer, Web UI, packaging, and ABI checks.
6. Validate HDR and input behavior on the SteamOS host using the normal immutable
   artifact installer, retaining the prior version for rollback.
7. Push validation results to the draft pull request, then merge after required
   validation passes.

## Integration decisions

- Upstream libvirtualhid replaces inputtino. The verified Gamescope EIS route, fail-closed input behavior, combined touch/pen state, lazy desktop mouse creation, seat naming, and route diagnostics are retained.
- Pending pairing IDs are required in both management interfaces. Requests are listed behind the existing authentication boundary; PINs are applied to the selected request only.
- Bounded coalescing input and blocking video queues remain available alongside the upstream nonblocking rejection policy. Retained input queues resume on client reconnect.
- Gamescope capture retains producer-driven pacing and buffer leases. Desktop PipeWire receives the upstream KWin pacing policy and memory-buffer fixes. Vulkan Wayland capture retains the DMA-BUF route.
- Steam launch commands enable Gamescope's Steam focus/overlay integration. Ordinary applications retain normal window focus. The mode is included in retained-session compatibility and preserves the HDR flag.
- The pinned SteamOS image and prepared FFmpeg remain unchanged; the upstream Vulkan queue API is version guarded. Non-English locale files remain unchanged per repository policy.

## Steam overlay behavior

Owned Gamescope sessions enable `--steam` when the launch command, detached command,
or preparation command starts Steam. Gamescope uses this mode to publish Steam app
IDs and focused-app properties consumed by the Big Picture overlay. Ordinary programs
keep the existing window-focus behavior. Retained sessions cannot be reused across
these two modes. HDR and the socket-identity WSI fix are independent of this selection.

## Validation and deployment

The integration passes the 53-test standalone SteamOS core suite, 56-test lifecycle
suite, and broad headless regression (770 passed, two platform/display-dependent
skips). Hardware audio, encoder, gamepad, and visual tray suites require a real session;
the 60-second input producer test is run separately during focused validation.
Installer/virtual-display fixtures and both management interfaces' real Chromium checks
cover setup, authentication, selected pending pairing requests, terminals, and session
invalidation. The new virtual input refresh tests preserve deferred desktop devices.

Build deployable artifacts only in the image pinned by `ci/steamos/image.lock`, then
package with `scripts/package-steamos-artifact.sh`. Validate the packaged binaries
with host `ldd` before using the normal installer; keep its prior immutable version
for rollback. Final CI and host HDR/overlay observations are recorded in the pull request.

The privileged input helper now prints and installs the upstream libvirtualhid rules,
including HID parent properties, while retaining legacy inputtino rules for rollback.
Updating an existing root-owned helper uses the installer's existing sudo authorization
path. The read-only `print-input-rules` command allows validation without root access.
Existing SteamOS controller rules can also grant access; verify the resulting event and
hidraw nodes during host acceptance rather than assuming a particular permission rule.
