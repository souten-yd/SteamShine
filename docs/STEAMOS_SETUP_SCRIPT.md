# SteamShine SteamOS setup script

`./steamshine.sh` is the user-facing lifecycle command. With a TTY and no arguments it opens a menu; without a TTY it prints usage and makes no change.

Examples:

```bash
./steamshine.sh check
./steamshine.sh install
./steamshine.sh bootstrap --non-interactive --yes
./steamshine.sh diagnose
./steamshine.sh autostart-status
./steamshine.sh uninstall
./steamshine.sh uninstall --purge --yes --non-interactive
```

Supported commands are `menu`, `check`, `compatibility-check`, `install`, `build`, `configure`,
`start`, `stop`, `restart`, `status`, `logs`, `diagnose`, `autostart-status`, `update`, `repair`,
`uninstall`, `bootstrap`, `rollback`, and `hardware-test`. All modifying commands accept `--dry-run`.

`install` is the one-command Game Mode path. It updates only
`steamos_virtual_display_enabled=true`, `steamos_virtual_display_mode=auto`, and
`steamos_session_source=auto`, enables the systemd user service, and starts it. Unrelated settings
are preserved, the pre-change file is backed up once under the configuration `backups` directory,
and repeated runs are idempotent. `--no-start` installs, creates the unit, and enables it for the next
login without starting it in the current session. `--no-service` omits unit creation, enablement, and
startup entirely.

`install` downloads the newest published SteamShine GitHub Release when no artifact-selection option
is supplied. It requires exactly one `steamshine-steamos-x86_64-<commit>.tar.zst` asset and its
matching `.sha256`, stores both under `~/.cache/steamshine/releases`, and then uses the normal
checksum, archive-path, architecture, and immutable-version validation before installation. `-a` and
`--latest-release` remain accepted as compatibility aliases but are no longer required.

`install --artifact` and `install --channel pr` are immutable SteamOS user-space installs: they neither use a package manager nor require local build tools, and never disable SteamOS read-only mode. The separate interactive `menu` package-install option supports development hosts on SteamOS/Arch (`pacman`), Debian/Ubuntu (`apt`), and Fedora (`dnf`); it verifies each candidate package before requesting installation. User-local files remain under `~/.local`, `~/.config/steamshine`, `~/.local/state/steamshine`, and `$XDG_RUNTIME_DIR/steamshine`; normal uninstall removes only generated binaries, versions, cache, runtime files, and the user service, never shared packages or retained user configuration.

The generated unit is an unprivileged user unit at
`~/.config/systemd/user/steamshine.service`, enabled through
`default.target.wants/steamshine.service`. It uses `%t` for `XDG_RUNTIME_DIR`,
`PIPEWIRE_RUNTIME_DIR`, and the user D-Bus socket, and `%h` for the installed executable and
configuration. It does not require a graphical-session target or inherit a hard-coded
`WAYLAND_DISPLAY`, `DISPLAY`, or Gamescope endpoint. Session discovery happens when streaming is
requested, so an early Game Mode login can keep the HTTP/NVHTTP/RTSP server resident while PipeWire
and Gamescope appear later. The installer never runs `loginctl enable-linger`; normal Game Mode login
starts the user manager and its `default.target`.

`update` restarts a service that was active before the Artifact change, but preserves an enabled
inactive service as inactive. `repair` rewrites and reloads the unit, restores its enablement link,
and restarts an active service only when its process does not resolve to the installed binary.
`uninstall` disables and stops the service, removes the unit and wants link, reloads the user manager,
and clears its failed state. `autostart-status` reports unit state, `MainPID`, `ExecStart`, the live
executable path, Artifact commit, runtime/login/linger state, wants link, latest boot journal result,
and stable failure reasons. A separately launched SteamShine process is reported but never killed.

Run `./steamshine.sh compatibility-check` after a SteamOS update. It verifies the measured SteamOS 3.8.16 baseline, ABI floors, Gamescope headless/device-selection options, and the expected RX 9070 XT render node. When the virtual-display feature is enabled, `start` runs the same gate before creating the user service. `hardware-test --interactive` records the gate first and stops the user service if its acceptance harness fails.

Exit statuses: 0 success; 1 general error; 2 usage; 3 unsupported OS; 4 missing dependency; 6 build failure; 7 test failure; 8 service failure; 9 configuration failure; 10 uninstall failure.
