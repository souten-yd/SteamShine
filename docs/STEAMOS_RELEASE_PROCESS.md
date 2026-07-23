# SteamOS artifact release process

The `SteamOS Runtime Build` GitHub Actions workflow builds an x86_64 artifact in an Arch Linux base container whose packages are pinned to Valve's public SteamOS 3.8 repositories. This avoids publishing an artifact that needs a newer glibc, libstdc++, or Qt ABI than SteamOS provides. The pinned Valve CI builder key is stored as ASCII armor in `ci/steamos/steam-os-ci-builder.asc`; the workflow verifies its full primary fingerprint before locally trusting it. `ci/steamos/Containerfile` mirrors this ephemeral build environment for reproduction. It is intentionally limited to the SteamOS delivery path: focused formatting, ShellCheck, workflow validation, the SteamOS virtual-session GTest filter, installer smoke tests, runtime dependency inspection, and packaging.

It does not run Windows, macOS, FreeBSD, Flatpak, AppImage, Docker, or broad upstream Sunshine test matrices for a normal SteamOS pull request. CUDA and NVIDIA dependencies are disabled for this AMD-focused artifact. ROCm is not bundled; when available on the host, it is reported only by the explicit diagnostic command.

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains only user-space application files, scripts, a systemd user-service template, license, build metadata, and runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons. The CI environment also does not start Gamescope, PipeWire, Steam, or a GPU driver.

The build verifies the staged executable with `ldd`, dynamic-section inspection, and a symbol-version ceiling for the SteamOS 3.8 glibc, libstdc++, and Qt baselines. A build that references a newer ABI is rejected before it can be uploaded.

The starting `archlinux:base-devel` image is used only to bootstrap the CI job. Before compilation, one package transaction replaces its split GCC runtime and toolchain packages with their SteamOS 3.8 counterparts; no partially removed runtime is ever executed. This occurs only in the disposable CI container; it never runs on a SteamOS host.

SteamOS installation verifies the detached SHA-256 file and archive paths before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; rollback is `ln -sfn ~/.local/share/steamshine/versions/<previous> ~/.local/share/steamshine/current` followed by `systemctl --user restart steamshine`.
