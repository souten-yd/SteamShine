#!/usr/bin/env bash
# Read-only SteamOS virtual-display diagnosis; safe to run during an outage.
set -euo pipefail
echo "timestamp=$(date --iso-8601=seconds)"; uname -a
. /etc/os-release 2>/dev/null && printf 'os=%s\n' "${PRETTY_NAME:-unknown}"
printf 'runtime=%s\n' "${XDG_RUNTIME_DIR:-unset}"
gamescope --version 2>&1 || true; gamescope --help 2>&1 | grep -E -- '--headless|--nested-(width|height|refresh)' || true
find /sys/class/drm -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
ls -l /dev/dri /dev/uinput 2>/dev/null || true
vainfo 2>&1 || true; vulkaninfo --summary 2>&1 || true
pw-cli info 0 2>&1 || true; pw-cli list-objects Node 2>&1 || true
find "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" -maxdepth 1 -type s -name 'wayland-*' -print 2>/dev/null || true
ps -eo pid,pgid,ppid,cmd | grep -E '[s]teamshine|[g]amescope' || true
find "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamshine" -maxdepth 3 -print 2>/dev/null || true
