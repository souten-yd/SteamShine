# SteamOS artifact release process

The `SteamOS Runtime Build` GitHub Actions workflow builds an x86_64 artifact from the official Arch Linux `base-devel-20250630.0.373922` image and package archive snapshot. That snapshot supplies glibc 2.41, GCC 15.1.1, and Qt 6.9.1: the SteamOS 3.8 ABI baseline. This avoids publishing an artifact that needs a newer glibc, libstdc++, or Qt ABI than SteamOS provides. The disposable container updates only the official `archlinux-keyring` before verifying packages from the fixed archive; it does not weaken package signature validation. `ci/steamos/Containerfile` mirrors this ephemeral build environment for reproduction. It is intentionally limited to the SteamOS delivery path: focused formatting, ShellCheck, workflow validation, the SteamOS virtual-session GTest filter, installer smoke tests, runtime dependency inspection, and packaging.

It does not run Windows, macOS, FreeBSD, Flatpak, AppImage, Docker, or broad upstream Sunshine test matrices for a normal SteamOS pull request. CUDA and NVIDIA dependencies are disabled for this AMD-focused artifact. ROCm is not bundled; when available on the host, it is reported only by the explicit diagnostic command.

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains only user-space application files, scripts, a systemd user-service template, license, build metadata, and runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons. The CI build installs PipeWire only because its `libpipewire-0.3` development interface is a required CMake dependency; it does not start PipeWire, Gamescope, Steam, or any GPU workload.

The build verifies the staged executable with `ldd`, dynamic-section inspection, and a symbol-version ceiling for the SteamOS 3.8 glibc, libstdc++, and Qt baselines. A build that references a newer ABI is rejected before it can be uploaded.

The starting image is already ABI-compatible, so CI does not downgrade or replace its C library at runtime. Package installation and compilation occur only in the disposable container; they never run on a SteamOS host.

SteamOS installation verifies the detached SHA-256 file and archive paths before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; rollback is `ln -sfn ~/.local/share/steamshine/versions/<previous> ~/.local/share/steamshine/current` followed by `systemctl --user restart steamshine`.
