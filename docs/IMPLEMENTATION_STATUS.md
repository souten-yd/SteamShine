# Implementation status

| Item | Status | Notes |
| --- | --- | --- |
| SteamOS virtual display lifecycle | CI Tested | Feature flag, normalized request, dynamically validated Gamescope command, owned runtime/process cleanup, app Wayland environment, and GameStream launch hook are covered by the focused CI workflow. Monitorless hardware validation remains required. |
| Gamescope/Wayland same-GPU capture | Implementing | The owned socket is attached through existing Wayland DMA-BUF capture. PCI BDF, card node, and render node are resolved from sysfs; ambiguous Gamescope vendor/device selectors are rejected. DMA-BUF device identity needs hardware validation. |
| Setup lifecycle script | CI Tested | Immutable user-local install/service lifecycle, checksum/link/path/architecture validation, idempotent version install, rollback, and dry-run support are covered by the installer integration test. |
| Fake Gamescope lifecycle GTest | CI Tested | Readiness, actual UNIX socket, delayed and invalid socket handling, argument generation, early crash, timeout, cleanup, owned child reaping, and duplicate-session coverage passed in run 30056309566. |
| Fake PipeWire readiness | Not started | The provider has no PipeWire-node adapter yet, so a truthful fake readiness test cannot be added. |
| Dependency bootstrap | Implemented | Script verifies repository package availability for pacman, apt, or dnf before installation. |
| Automated configure/build/format/lint | CI Tested | Run 30056309566 passed focused lint, configure, build, targeted GTest, installer smoke, linkage/ABI gate, package, checksum, and Artifact upload. The pinned container provides CI ABI ceilings; SteamOS-host ABI confirmation remains a hardware-test requirement. |
| Hardware latency/SSD comparison | Blocked: requires hardware test | Collection scripts added; acceptance values must be measured on a monitorless SteamOS host. |
