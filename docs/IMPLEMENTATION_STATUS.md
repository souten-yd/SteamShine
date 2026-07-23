# Implementation status

| Item | Status | Notes |
| --- | --- | --- |
| SteamOS virtual display lifecycle | Implementing | Feature flag, normalization, owned runtime/process cleanup, and GameStream launch hook added. |
| Gamescope/PipeWire same-GPU capture | Blocked: requires hardware test and adapter integration | Target Gamescope options and PipeWire node integration require SteamOS AMD hardware. |
| Setup lifecycle script | Implemented | User-local install/service lifecycle and dry-run support added. |
| Fake Gamescope lifecycle GTest | Implementing | Source added; not executed because this environment lacks CMake. |
| Fake PipeWire readiness | Not started | The provider has no PipeWire-node adapter yet, so a truthful fake readiness test cannot be added. |
| Dependency bootstrap | Implemented | Script verifies repository package availability for pacman, apt, or dnf before installation. |
| Automated configure/build/format/lint | Blocked: sudo authorization unavailable | The installation command was attempted but this non-interactive environment cannot authenticate sudo. |
| Hardware latency/SSD comparison | Blocked: requires hardware test | Collection scripts added; acceptance values must be measured on a monitorless SteamOS host. |
