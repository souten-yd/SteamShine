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

## Status

Initial integration assessment complete. Implementation and validation are in progress.
No upstream update has been installed on the host.
