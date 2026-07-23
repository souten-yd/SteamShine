# Implementation status

| Item | Status | Notes |
| --- | --- | --- |
| SteamOS virtual display lifecycle | Implementing | Feature flag, normalized request, dynamically validated Gamescope command, owned runtime/process cleanup, app Wayland environment, and GameStream launch hook added. |
| Gamescope/Wayland same-GPU capture | Implementing | The owned socket is attached through existing Wayland DMA-BUF capture and its AMD render node reaches VA-API/Vulkan device resolution. Hardware validation remains required. |
| Setup lifecycle script | Implemented | User-local install/service lifecycle and dry-run support added. |
| Fake Gamescope lifecycle GTest | Implementing | Readiness, actual UNIX socket, argument generation, early crash, timeout, cleanup, and duplicate-session coverage are present; current commit awaits CI. |
| Fake PipeWire readiness | Not started | The provider has no PipeWire-node adapter yet, so a truthful fake readiness test cannot be added. |
| Dependency bootstrap | Implemented | Script verifies repository package availability for pacman, apt, or dnf before installation. |
| Automated configure/build/format/lint | CI Tested | SteamOS-only Arch workflow has completed the focused checks, configure, build, targeted GTest, installer smoke, linkage, package, and Artifact upload. |
| Hardware latency/SSD comparison | Blocked: requires hardware test | Collection scripts added; acceptance values must be measured on a monitorless SteamOS host. |
