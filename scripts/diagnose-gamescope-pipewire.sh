#!/usr/bin/env bash
# @file scripts/diagnose-gamescope-pipewire.sh
# @brief Collect non-secret Gamescope and host PipeWire ownership diagnostics.
set -Eeuo pipefail

[[ "${STEAMSHINE_GAMESCOPE_PIPEWIRE_DIAG:-}" == "1" ]] || {
  printf '%s\n' 'Set STEAMSHINE_GAMESCOPE_PIPEWIRE_DIAG=1 to collect Gamescope PipeWire diagnostics.' >&2
  exit 2
}

service_name="${STEAMSHINE_SERVICE_NAME:-steamshine.service}"
config_file="${STEAMSHINE_CONFIG_FILE:-}"
configured_runtime="${STEAMSHINE_PIPEWIRE_RUNTIME:-}"
configured_remote="${STEAMSHINE_PIPEWIRE_REMOTE:-}"

if [[ -n "${config_file}" && -r "${config_file}" ]]; then
  configured_runtime="${configured_runtime:-$(sed -nE 's/^[[:space:]]*steamos_pipewire_runtime[[:space:]]*=[[:space:]]*(.*)[[:space:]]*$/\1/p' "${config_file}" | tail -n 1)}"
  configured_remote="${configured_remote:-$(sed -nE 's/^[[:space:]]*steamos_pipewire_remote[[:space:]]*=[[:space:]]*(.*)[[:space:]]*$/\1/p' "${config_file}" | tail -n 1)}"
fi

runtime_dir="${configured_runtime:-${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}}}"
remote_name="${configured_remote:-${PIPEWIRE_REMOTE:-pipewire-0}}"
socket_path="${runtime_dir}/${remote_name}"

section() { printf '\n=== %s ===\n' "$1"; }

section 'Host PipeWire endpoint'
printf 'configured_runtime=%s\nconfigured_remote=%s\nruntime=%s\nremote=%s\nsocket=%s\n' \
  "${configured_runtime:-<unset>}" "${configured_remote:-<unset>}" "${runtime_dir}" "${remote_name}" "${socket_path}"
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
if pipewire_connect_output="$(PIPEWIRE_RUNTIME_DIR="${runtime_dir}" PIPEWIRE_REMOTE="${remote_name}" pw-cli info 0 2>&1)"; then
  printf '%s\n' 'pipewire_socket_connected=true'
else
  connection_status=$?
  printf 'pipewire_socket_connected=false exit_status=%s\n' "${connection_status}"
  printf 'connect_detail=%s\n' "${pipewire_connect_output}"
fi

section 'Private Wayland runtimes'
find "${runtime_dir}/steamshine" -mindepth 1 -maxdepth 1 -type d -name 'session-*' -printf 'private_runtime=%p\n' 2>/dev/null || true

section 'Owned Gamescope processes'
pgrep -a gamescope 2>&1 || true
for process in $(pgrep gamescope 2>/dev/null || true); do
  printf '\n--- pid=%s ---\n' "${process}"
  (
    tr '\0' '\n' <"/proc/${process}/environ"
  ) 2>/dev/null | grep -E '^(XDG_RUNTIME_DIR|PIPEWIRE_RUNTIME_DIR|PIPEWIRE_REMOTE|PULSE_RUNTIME_PATH)=' || true
done

section 'Application child environment'
if [[ "${service_pid}" =~ ^[0-9]+$ ]] && [[ "${service_pid}" -gt 0 ]]; then
  for process in $(pgrep -P "${service_pid}" 2>/dev/null || true); do
    printf '\n--- pid=%s ---\n' "${process}"
    (
      tr '\0' '\n' <"/proc/${process}/environ"
    ) 2>/dev/null |
      grep -E '^(SUNSHINE_APP_(ID|NAME)|XDG_RUNTIME_DIR|WAYLAND_DISPLAY|PIPEWIRE_RUNTIME_DIR|PIPEWIRE_REMOTE|PULSE_RUNTIME_PATH)=' || true
  done
fi

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
