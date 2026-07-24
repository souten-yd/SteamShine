# Uninstallation

Run `./steamshine.sh uninstall` to stop and remove the SteamShine user service, installed binary and version store, artifact cache, owned runtime state, and build directory. It does not require CMake or any other local development tool. Configuration, pairing data, logs, and backups are retained.

Run `./steamshine.sh uninstall --purge` for complete user-data removal. In non-interactive mode it additionally requires `--yes`. Shared packages are not removed automatically.
