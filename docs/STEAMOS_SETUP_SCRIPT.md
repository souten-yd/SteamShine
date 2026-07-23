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

The script uses user locations (`~/.local`, `~/.config/steamshine`, `~/.local/state/steamshine`, and `$XDG_RUNTIME_DIR/steamshine`) and a systemd user service. It never disables SteamOS read-only mode. Package installation is explicit through pacman and records requested package names; uninstall never removes shared packages automatically.

Exit statuses: 0 success; 1 general error; 2 usage; 3 unsupported OS; 4 missing dependency; 6 build failure; 7 test failure; 8 service failure; 9 configuration failure; 10 uninstall failure.
