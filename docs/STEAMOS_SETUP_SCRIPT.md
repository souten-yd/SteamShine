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
startup entirely, and it does not provision the privileged runtime or Decky helpers.

Normal installation and repair provision a separate root-owned runtime helper. It validates bounded
profile values against driver-reported limits and writes only fixed AMD GPU/CPU sysfs attributes,
allowing GPU power profiles to survive artifact updates without making the service privileged. The
selected GPU profile is verified against live sysfs on activation and is reapplied after every service
start. Interactive authorization is required only when the helper is first installed or replaced; its
command-specific sudoers default prevents a later broad SteamOS rule from restoring password prompts
for this helper. Repair tests that policy while explicitly ignoring any temporary sudo timestamp, so
a recently entered password cannot be mistaken for persistent non-interactive authorization.

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

### GPU profile detection and administrator authentication

The GPU page probes power limits through the fixed `probe-gpu` runtime-helper operation.
The helper temporarily resumes an AMD GPU, validates its minimum, maximum, and default
power limits, and restores its previous runtime-power policy on exit. Applying a profile
verifies the power limit before releasing that temporary hold. A requested limit that
cannot be applied is reported as a failure and does not replace the saved active selection.

If the helper needs authorization or updating, the GPU page displays an administrator
password dialog. The HTTPS endpoint requires an authenticated session and CSRF token,
limits authentication attempts, and passes the transient password to sudo on stdin.
Passwords are not saved or included in command arguments or logs. The packaged provisioner
installs only the fixed GPU and Decky helper operations after sudo authentication.

A new GPU profile requires detected power bounds. Editing an existing profile preserves
values whose controls are unavailable instead of replacing them with zero. A missing
live power reading is displayed as unavailable, independently of the saved selection.
