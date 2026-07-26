# SteamShine SteamOS setup script

`./steamshine.sh` is the user-facing lifecycle command. With a TTY and no arguments it opens a menu; without a TTY it prints usage and makes no change.

Examples:

```bash
./steamshine.sh check
./steamshine.sh install --non-interactive --yes
./steamshine.sh bootstrap --non-interactive --yes
./steamshine.sh diagnose
./steamshine.sh uninstall
./steamshine.sh uninstall --purge --yes --non-interactive
```

Supported commands are `menu`, `check`, `compatibility-check`, `install`, `build`, `configure`, `start`, `stop`, `restart`, `status`, `logs`, `diagnose`, `update`, `repair`, `uninstall`, `bootstrap`, `rollback`, and `hardware-test`. All modifying commands accept `--dry-run`.

`install --artifact` and `install --channel pr` are immutable SteamOS user-space installs: they neither use a package manager nor require local build tools, and never disable SteamOS read-only mode. The separate interactive `menu` package-install option supports development hosts on SteamOS/Arch (`pacman`), Debian/Ubuntu (`apt`), and Fedora (`dnf`); it verifies each candidate package before requesting installation. User-local files remain under `~/.local`, `~/.config/steamshine`, `~/.local/state/steamshine`, and `$XDG_RUNTIME_DIR/steamshine`; normal uninstall removes only generated binaries, versions, cache, runtime files, and the user service, never shared packages or retained user configuration.

Run `./steamshine.sh compatibility-check` after a SteamOS update. It verifies the measured SteamOS 3.8.16 baseline, ABI floors, Gamescope headless/device-selection options, and the expected RX 9070 XT render node. When the virtual-display feature is enabled, `start` runs the same gate before creating the user service. `hardware-test --interactive` records the gate first and stops the user service if its acceptance harness fails.

Exit statuses: 0 success; 1 general error; 2 usage; 3 unsupported OS; 4 missing dependency; 6 build failure; 7 test failure; 8 service failure; 9 configuration failure; 10 uninstall failure.
