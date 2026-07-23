# SteamOS artifact release process

The `SteamOS Runtime Build` GitHub Actions workflow builds an x86_64 artifact in an Arch Linux container. It is intentionally limited to the SteamOS delivery path: focused formatting, ShellCheck, workflow validation, the SteamOS virtual-session GTest filter, installer smoke tests, runtime dependency inspection, and packaging.

It does not run Windows, macOS, FreeBSD, Flatpak, AppImage, Docker, or broad upstream Sunshine test matrices for a normal SteamOS pull request. CUDA and NVIDIA dependencies are disabled for this AMD-focused artifact. ROCm is not bundled; when available on the host, it is reported only by the explicit diagnostic command.

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains only user-space application files, scripts, a systemd user-service template, license, build metadata, and runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons.

SteamOS installation verifies the detached SHA-256 file and archive paths before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; rollback is `ln -sfn ~/.local/share/steamshine/versions/<previous> ~/.local/share/steamshine/current` followed by `systemctl --user restart steamshine`.
