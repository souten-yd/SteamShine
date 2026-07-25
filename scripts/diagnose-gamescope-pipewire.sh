#!/usr/bin/env bash
# @file scripts/diagnose-gamescope-pipewire.sh
# @brief Collect non-secret Gamescope and host PipeWire ownership diagnostics.
set -Eeuo pipefail

[[ "${STEAMSHINE_GAMESCOPE_PIPEWIRE_DIAG:-}" == "1" ]] || {
  printf '%s\n' 'Set STEAMSHINE_GAMESCOPE_PIPEWIRE_DIAG=1 to collect Gamescope PipeWire diagnostics.' >&2
  exit 2
}

runtime_dir="${STEAMSHINE_PIPEWIRE_RUNTIME:-${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}}"
remote_name="${STEAMSHINE_PIPEWIRE_REMOTE:-${PIPEWIRE_REMOTE:-pipewire-0}}"
socket_path="${runtime_dir}/${remote_name}"

section() { printf '\n=== %s ===\n' "$1"; }

section 'Host PipeWire endpoint'
printf 'runtime=%s\nremote=%s\nsocket=%s\n' "${runtime_dir}" "${remote_name}" "${socket_path}"
stat "${socket_path}" 2>&1 || true

section 'Owned Gamescope processes'
pgrep -a gamescope 2>&1 || true
for process in $(pgrep gamescope 2>/dev/null || true); do
  printf '\n--- pid=%s ---\n' "${process}"
  tr '\0' '\n' <"/proc/${process}/environ" 2>/dev/null | grep -E '^(XDG_RUNTIME_DIR|PIPEWIRE_RUNTIME_DIR|PIPEWIRE_REMOTE|PULSE_RUNTIME_PATH)=' || true
done

section 'PipeWire nodes'
pw-cli ls Node 2>&1 || true

section 'PipeWire registry dump'
pw-dump 2>&1 || true

section 'Recent SteamShine and Gamescope evidence'
journalctl --user -u steamshine.service --since '-15 min' --no-pager 2>/dev/null |
  grep -Ei 'gamescope|pipewire|virtual display|capture|render node|error|failed' || true
