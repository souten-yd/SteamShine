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

Supported commands are `menu`, `check`, `install`, `build`, `configure`, `start`, `stop`, `restart`, `status`, `logs`, `diagnose`, `update`, `repair`, `uninstall`, `bootstrap`, and `rollback`. All modifying commands accept `--dry-run`.

The script supports SteamOS/Arch (`pacman`), Debian/Ubuntu (`apt`), and Fedora (`dnf`). It verifies each candidate package exists in the active repository before requesting installation, uses the official dependency inventory from `scripts/linux_build.sh`, and installs with `--needed` or the manager equivalent. It requires normal sudo authorization but never disables SteamOS read-only mode. User-local files remain under `~/.local`, `~/.config/steamshine`, `~/.local/state/steamshine`, and `$XDG_RUNTIME_DIR/steamshine`; uninstall never removes shared packages automatically.

Exit statuses: 0 success; 1 general error; 2 usage; 3 unsupported OS; 4 missing dependency; 6 build failure; 7 test failure; 8 service failure; 9 configuration failure; 10 uninstall failure.
