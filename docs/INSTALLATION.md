# Installation

For SteamOS/Arch hosts, run `./steamshine.sh bootstrap --non-interactive --yes`. Use `./steamshine.sh` for the interactive menu, `start`, `stop`, `status`, and `logs` for service control, and `diagnose` for prerequisite checks. Use `--dry-run` before any modifying command.

The script supports SteamOS/Arch, Debian/Ubuntu, and Fedora. It installs only verified missing official packages when approved, builds into `cmake-build-steamos`, installs the binary under `~/.local/bin`, writes an opt-in configuration, and installs a systemd user service. On SteamOS, first authenticate in an interactive terminal with `sudo -v`, then run `./steamshine.sh bootstrap --non-interactive --yes`.
