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
service_name="steamshine.service"

section() { printf '\n=== %s ===\n' "$1"; }

section 'Host PipeWire endpoint'
printf 'runtime=%s\nremote=%s\nsocket=%s\n' "${runtime_dir}" "${remote_name}" "${socket_path}"
printf 'canonical_runtime=%s\n' "$(realpath -e "${runtime_dir}" 2>/dev/null || printf 'unavailable')"
stat -c 'path=%n type=%F uid=%u gid=%g mode=%a' "${runtime_dir}" "${socket_path}" 2>&1 || true

section 'SteamShine service environment'
systemctl --user show "${service_name}" -p MainPID -p ExecStart -p Environment -p FragmentPath 2>&1 || true
service_pid="$(systemctl --user show "${service_name}" -p MainPID --value 2>/dev/null || true)"
if [[ "${service_pid}" =~ ^[0-9]+$ ]] && [[ "${service_pid}" -gt 0 ]] && [[ -r "/proc/${service_pid}/environ" ]]; then
  printf 'service_pid=%s\n' "${service_pid}"
  tr '\0' '\n' <"/proc/${service_pid}/environ" |
    grep -E '^(XDG_RUNTIME_DIR|PIPEWIRE_RUNTIME_DIR|PIPEWIRE_REMOTE|PULSE_RUNTIME_PATH)=' || true
fi

section 'Host PipeWire connection'
if PIPEWIRE_RUNTIME_DIR="${runtime_dir}" PIPEWIRE_REMOTE="${remote_name}" pw-cli info 0 >/dev/null 2>&1; then
  printf '%s\n' 'pipewire_socket_connected=true'
else
  connection_status=$?
  printf 'pipewire_socket_connected=false exit_status=%s\n' "${connection_status}"
fi

section 'Private Wayland runtimes'
find "${runtime_dir}/steamshine" -mindepth 1 -maxdepth 1 -type d -name 'session-*' -printf 'private_runtime=%p\n' 2>/dev/null || true

section 'Owned Gamescope processes'
pgrep -a gamescope 2>&1 || true
for process in $(pgrep gamescope 2>/dev/null || true); do
  printf '\n--- pid=%s ---\n' "${process}"
  tr '\0' '\n' <"/proc/${process}/environ" 2>/dev/null | grep -E '^(XDG_RUNTIME_DIR|PIPEWIRE_RUNTIME_DIR|PIPEWIRE_REMOTE|PULSE_RUNTIME_PATH)=' || true
done

section 'PipeWire nodes'
pw-cli ls Node 2>&1 || true

if [[ "${STEAMSHINE_GAMESCOPE_PIPEWIRE_DUMP:-}" == "1" ]]; then
  section 'PipeWire registry dump'
  pw-dump 2>&1 || true
else
  section 'PipeWire registry dump'
  printf '%s\n' 'Set STEAMSHINE_GAMESCOPE_PIPEWIRE_DUMP=1 to include the full pw-dump output.'
fi

section 'Recent SteamShine and Gamescope evidence'
journalctl --user -u steamshine.service --since '-15 min' --no-pager 2>/dev/null |
  grep -Ei 'gamescope|pipewire|virtual display|capture|render node|error|failed' || true
