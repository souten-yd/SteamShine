# SteamOS artifact release process

The `build steamos artifact` GitHub Actions workflow builds in an Arch Linux container, runs formatting, ShellCheck, CTest, integration smoke tests, runtime dependency inspection, and creates:

```text
steamshine-steamos-x86_64-<commit>.tar.zst
steamshine-steamos-x86_64-<commit>.tar.zst.sha256
```

The archive contains only user-space application files, scripts, a systemd user-service template, license, build metadata, and runtime dependency report. It deliberately does not bundle graphics drivers, Mesa, Vulkan ICDs, VAAPI drivers, or PipeWire daemons.

SteamOS installation verifies the detached SHA-256 file and archive paths before atomically switching `~/.local/share/steamshine/current`. A failed install keeps the prior `current` version; rollback is `ln -sfn ~/.local/share/steamshine/versions/<previous> ~/.local/share/steamshine/current` followed by `systemctl --user restart steamshine`.
