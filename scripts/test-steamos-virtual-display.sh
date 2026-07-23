#!/usr/bin/env bash
# Headless SteamOS acceptance harness. Start it before connecting Moonlight.
set -euo pipefail
runtime_dir="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}/steamshine"
state_dir="${HOME}/.local/state/steamshine"
mkdir -p "${state_dir}/hardware-tests"
report="${state_dir}/hardware-tests/virtual-display-$(date +%Y%m%d-%H%M%S).log"
collect() {
  {
    echo "== $1 $(date --iso-8601=seconds) =="
    . /etc/os-release 2>/dev/null && printf 'OS=%s\n' "${PRETTY_NAME:-unknown}"
    uname -a; gamescope --version 2>&1 || true
    find /sys/class/drm -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
    vainfo 2>&1 || true; vulkaninfo --summary 2>&1 || true
    pw-cli list-objects Node 2>&1 || true; find "${XDG_RUNTIME_DIR}" -maxdepth 1 -type s -name 'wayland-*' -print 2>/dev/null || true
    pgrep -a steamshine || true; pgrep -a gamescope || true; ps -eo pid,pgid,ppid,cmd | grep -E '[s]teamshine|[g]amescope' || true
    find "${runtime_dir}" -maxdepth 3 -print 2>/dev/null || true
  } | tee -a "${report}"
}
echo "Disconnect physical displays, start SteamShine, then connect Moonlight ten times. Report: ${report}"
collect before
for attempt in $(seq 1 10); do
  echo "Attempt ${attempt}: connect Moonlight now; press Enter after video/audio/input work, then disconnect and press Enter after cleanup."
  read -r
  collect "connected-${attempt}"
  read -r
  collect "disconnected-${attempt}"
  if pgrep -f "gamescope.*${runtime_dir}" >/dev/null; then echo "FAIL: owned Gamescope remains" | tee -a "${report}"; exit 1; fi
  if find "${runtime_dir}" -mindepth 1 -name 'session-*' -print -quit | grep -q .; then echo "FAIL: owned runtime session remains" | tee -a "${report}"; exit 1; fi
done
echo "PASS: ten operator-verified connect/disconnect cycles completed" | tee -a "${report}"
