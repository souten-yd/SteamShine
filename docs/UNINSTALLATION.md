# Uninstallation

Run `./steamshine.sh uninstall` to stop and remove the SteamShine user service, installed binary, owned runtime state, and build directory. Configuration, pairing data, logs, and backups are retained.

Run `./steamshine.sh uninstall --purge` for complete user-data removal. In non-interactive mode it additionally requires `--yes`. Shared packages are not removed automatically.
