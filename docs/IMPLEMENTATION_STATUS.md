# Implementation status

| Item | Status | Notes |
| --- | --- | --- |
| SteamOS virtual display lifecycle | CI Tested | Feature flag, normalized request, dynamically validated Gamescope command, ownership-marked runtime/process cleanup including startup orphan recovery, app Wayland environment, and GameStream launch hook are covered by the focused CI workflow. Monitorless Moonlight validation remains required. |
| Gamescope/Wayland same-GPU capture | Implementing | The owned socket is attached through existing Wayland DMA-BUF capture, including the Vulkan Video memory type. PCI BDF, card node, and render node are resolved from sysfs; ambiguous Gamescope vendor/device selectors are rejected. DMA-BUF device identity needs hardware validation. |
| Setup lifecycle script | Hardware Tested | The immutable user-local Artifact was installed on SteamOS 3.8.16; packaged assets initialized `apps.json`, and the generated systemd user service remained active after the corrected positional config argument. No system package or read-only-root change was used. |
| Fake Gamescope lifecycle GTest | CI Tested | Readiness, actual UNIX socket, delayed and invalid socket handling, argument generation, early crash, timeout, cleanup, owned child reaping, duplicate-session protection, and ownership-marked orphan-runtime cleanup passed in run 30064048433. |
| Fake PipeWire readiness | Not started | The provider has no PipeWire-node adapter yet, so a truthful fake readiness test cannot be added. |
| Dependency bootstrap | Implemented | Script verifies repository package availability for pacman, apt, or dnf before installation. |
| Automated configure/build/format/lint | CI Tested | Run 30067001826 passed focused lint, configure, build, targeted GTest, installer smoke, linkage/ABI gate, package, checksum, and Artifact upload. The pinned container provides CI ABI ceilings; SteamOS-host ABI confirmation remains a hardware-test requirement. |
| Vulkan Video H.264 probe | Hardware Tested | The final Artifact passed `vulkan-video-probe` on the SteamOS 3.8.16 RX 9070 XT host after the diagnostic was changed to initialize the same Vulkan hardware-frames context required by `h264_vulkan`. This is a device/encoder-open test, not live stream acceptance. |
| Hardware latency/SSD comparison | Blocked: requires hardware test | Collection scripts added; acceptance values must be measured on a monitorless SteamOS host. |
