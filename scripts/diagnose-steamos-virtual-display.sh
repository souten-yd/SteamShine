#!/usr/bin/env bash
# Read-only SteamOS virtual-display diagnosis; safe to run during an outage.
# shellcheck disable=SC1091,SC2009
set -euo pipefail
echo "timestamp=$(date --iso-8601=seconds)"; uname -a
. /etc/os-release 2>/dev/null && printf 'os=%s\n' "${PRETTY_NAME:-unknown}"
printf 'runtime=%s\n' "${XDG_RUNTIME_DIR:-unset}"
gamescope --version 2>&1 || true
# Gamescope 3.x exposes headless mode through `--backend headless`; older
# builds may advertise `--headless`. Record the actual supported interface
# instead of assuming either spelling during an on-device diagnosis.
gamescope --help 2>&1 | grep -E -- '--backend|headless|--nested-(width|height|refresh)|--expose-wayland|--prefer-vk-device|--hdr-enabled' || true
find /sys/class/drm -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
ls -l /dev/dri /dev/uinput 2>/dev/null || true
if command -v vainfo >/dev/null 2>&1; then vainfo 2>&1 || echo 'VAAPI_PROBE_FAILED'; else echo 'VAAPI_PROBE_SKIPPED: vainfo unavailable'; fi
vulkaninfo --summary 2>&1 || true
# ROCm is optional on SteamOS.  It is queried only during this explicit diagnostic,
# never from a streaming thread or normal service startup.
if command -v rocminfo >/dev/null 2>&1; then rocminfo 2>&1 || true; else echo 'rocminfo=not-installed'; fi
if command -v rocm-smi >/dev/null 2>&1; then rocm-smi 2>&1 || true; else echo 'rocm-smi=not-installed'; fi
pw-cli info 0 2>&1 || true; pw-cli list-objects Node 2>&1 || true
find "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" -maxdepth 1 -type s -name 'wayland-*' -print 2>/dev/null || true
ps -eo pid,pgid,ppid,cmd | grep -E '[s]teamshine|[g]amescope' || true
find "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamshine" -maxdepth 3 -print 2>/dev/null || true
