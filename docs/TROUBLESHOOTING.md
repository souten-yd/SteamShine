# Troubleshooting

Run `./steamshine.sh diagnose` and `scripts/diagnose-steamos-virtual-display.sh`. Confirm that `XDG_RUNTIME_DIR` exists, `/dev/dri` is accessible, PipeWire is reachable, and Gamescope is installed. For headless validation run `scripts/test-steamos-virtual-display.sh` and inspect only `$XDG_RUNTIME_DIR/steamshine/session-*` for owned leftovers.
