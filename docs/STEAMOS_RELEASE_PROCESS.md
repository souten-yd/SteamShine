# SteamOS artifact release process

The `SteamOS Runtime Build` GitHub Actions workflow builds an x86_64 artifact from the official Arch Linux `base-devel-20250630.0.373922` image and package archive snapshot. That snapshot supplies glibc 2.41, GCC 15.1.1, and Qt 6.9.1, which are the CI ABI ceilings checked before upload. They are not a claim of measured SteamOS-host compatibility: `hardware-test` records the target host's glibc, libstdc++, Qt, and package inventory before a compatibility claim can be made. The disposable container updates only the official `archlinux-keyring` before verifying packages from the fixed archive; it does not weaken package signature validation. `ci/steamos/Containerfile` mirrors this ephemeral build environment for reproduction. It is intentionally limited to the SteamOS delivery path: focused formatting, ShellCheck, workflow validation, the SteamOS virtual-session GTest filter, installer smoke tests, runtime dependency inspection, and packaging.

It does not run Windows, macOS, FreeBSD, Flatpak, AppImage, Docker, or broad upstream Sunshine test matrices for a normal SteamOS pull request. CUDA and NVIDIA dependencies are disabled for this AMD-focused artifact. ROCm is not bundled; when available on the host, it is reported only by the explicit diagnostic command.

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains user-space application files, scripts, a systemd user-service template, licenses, build metadata, and a runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons. `libminiupnpc.so.21` is the sole bundled general-purpose library because stock SteamOS does not guarantee that ABI; the archive sets only a relative `$ORIGIN/../lib` RPATH and includes its license. The CI build installs PipeWire only because its `libpipewire-0.3` development interface is a required CMake dependency; it does not start PipeWire, Gamescope, Steam, or any GPU workload.

The build emits `ldd -v`, the dynamic section, symbol versions, and ELF version information, then verifies the staged executable against the CI glibc, libstdc++, C++ ABI, and Qt ceilings. A build that references a newer ABI is rejected before it can be uploaded, with each offending version reported in the CI log.

The starting image is already ABI-compatible, so CI does not downgrade or replace its C library at runtime. Package installation and compilation occur only in the disposable container; they never run on a SteamOS host.

SteamOS installation verifies the detached SHA-256 file, archive paths, absence of link entries, and x86_64 artifact metadata before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; a successful replacement records the prior version for `./steamshine.sh rollback`, followed by `systemctl --user restart steamshine` when the service is enabled.
