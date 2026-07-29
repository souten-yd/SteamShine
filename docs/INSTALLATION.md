# Installation

For SteamOS, run `./steamshine.sh install` to download and install the latest published, CI-built
immutable artifact from this repository. Then use `start`, `stop`, `status`, `logs`, and `diagnose`
for service control and prerequisite checks. Use `--dry-run` before any modifying command. For testing
an unpublished PR artifact, use `./steamshine.sh install --channel pr --pr NUMBER` instead.

The SteamOS artifact installer downloads the release archive and detached checksum into
`~/.cache/steamshine/releases`, verifies its checksum, path entries, link entries, and declared x86_64
architecture before extraction, then installs immutable versions below
`~/.local/share/steamshine/versions`. It writes only to `~/.local`, `~/.config/steamshine`,
`~/.local/state/steamshine`, `~/.cache/steamshine`, and the user systemd directory. For
`--channel pr`, it skips newer successful runs that contain only timing or documentation artifacts and
selects the newest successful run containing the SteamOS delivery archive; cached archives from a
different run are never substituted. It does not invoke `pacman`, install compilers, require `sudo`,
disable read-only mode, or write to `/usr` or `/etc`. A repeated install of the same archive is
idempotent; installing a later archive retains the prior version for `./steamshine.sh rollback`. The
interactive development menu can install verified build packages and build into
`cmake-build-steamos` on supported development hosts (SteamOS/Arch, Debian/Ubuntu, and Fedora); this
is not part of normal SteamOS deployment.

Normal installation atomically creates the systemd user unit below
`~/.config/systemd/user`, reloads the user manager, enables `steamshine.service` for
`default.target`, starts it, and verifies the loaded unit, enablement link, active state, `MainPID`,
`ExecStart`, and live executable identity before reporting success. `install --no-start` performs the
same installation and enablement but leaves an inactive service inactive; `install --no-service`
does not create, enable, or start the unit. No system service, Desktop Autostart entry, Steam shortcut,
linger setting, `sudo`, or read-only filesystem change is used.

The user unit sets `XDG_RUNTIME_DIR=%t`, `PIPEWIRE_RUNTIME_DIR=%t`, and
`DBUS_SESSION_BUS_ADDRESS=unix:path=%t/bus`. `WAYLAND_DISPLAY`, `DISPLAY`, and
`GAMESCOPE_WAYLAND_DISPLAY` are deliberately absent: they are session endpoints discovered after
startup and are not prerequisites for the control and streaming servers to remain resident. Run
`./steamshine.sh autostart-status` to inspect the installed unit and current-boot process identity.
When a physical KDE desktop is selected, the per-stream display helper refreshes Plasma's current
`WAYLAND_DISPLAY`, `DISPLAY`, and related desktop values from the systemd user-manager environment
before invoking KScreen. This allows the resident service to configure a monitor even when it began
before Plasma published its session endpoint; non-KDE manager endpoints are ignored.
KScreen operations have a bounded timeout so a missing or transitioning desktop cannot hold the
stream launch path indefinitely.
When SteamOS virtual-display support is enabled, the resident server omits the optional system tray.
Qt's Wayland and X11 platform backends are tied to the current desktop compositor and can terminate
their process when that compositor disappears. Omitting the tray allows Plasma to stop during a
Desktop Mode to Game Mode transition without terminating the control and streaming servers. Normal
tray behavior is unchanged when SteamOS virtual-display support is disabled, and a platform
event-loop exit is not treated as a SteamShine shutdown request.
