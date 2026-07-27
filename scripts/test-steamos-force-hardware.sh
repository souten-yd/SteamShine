#!/usr/bin/env bash
# @file scripts/test-steamos-force-hardware.sh
# @brief Run an explicitly requested, temporary SteamOS force-mode hardware test.
set -Eeuo pipefail

# This test intentionally requires a positive opt-in because it temporarily
# stops the user service and asks the operator to connect Moonlight.
[[ "${STEAMSHINE_FORCE_HARDWARE_TEST:-}" == "1" ]] || {
  printf '%s\n' 'Set STEAMSHINE_FORCE_HARDWARE_TEST=1 to run the disruptive force-mode hardware test.' >&2
  exit 2
}

config_file="${STEAMSHINE_CONFIG:-${HOME}/.config/steamshine/sunshine.conf}"
binary="${STEAMSHINE_BINARY:-${HOME}/.local/bin/steamshine}"
runtime_root="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}"
report_root="${STEAMSHINE_FORCE_REPORT_DIR:-${HOME}/.local/state/steamshine/force-mode-report}"
report_dir="${report_root}/$(date -u +%Y%m%dT%H%M%SZ)"
temporary_root=""
test_pid=""
service_was_active=false

die() { printf 'force-hardware-test: %s\n' "$*" >&2; exit 1; }

redact_config() {
  sed -E '/^[[:space:]]*(credentials_file|password|pkey|cert|pin)[[:space:]]*=/Id' "$1"
}

cleanup() {
  local status="$?"
  trap - EXIT INT TERM
  if [[ -n "${test_pid}" ]] && kill -0 "${test_pid}" 2>/dev/null; then
    kill "${test_pid}" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "${test_pid}" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "${test_pid}" 2>/dev/null; then
      kill -KILL "${test_pid}" 2>/dev/null || true
    fi
    wait "${test_pid}" 2>/dev/null || true
  fi
  if "${service_was_active}"; then
    systemctl --user start steamshine.service || true
  fi
  if [[ -n "${temporary_root}" && -d "${temporary_root}" ]]; then
    rm -rf -- "${temporary_root}"
  fi
  exit "${status}"
}

command -v systemctl >/dev/null || die 'systemctl --user is required.'
command -v curl >/dev/null || die 'curl is required.'
[[ -r "${config_file}" ]] || die "Configuration is unreadable: ${config_file}"
[[ -x "${binary}" ]] || die "SteamShine binary is not executable: ${binary}"
umask 077
mkdir -p "${report_dir}"
# Keep the runtime short enough for Qt/KIO to append its worker socket name
# without exceeding Linux's UNIX-domain socket path limit.
temporary_root="$(mktemp -d "${runtime_root}/ss-fh.XXXXXX")"
temporary_config="${temporary_root}/sunshine.conf"
temporary_runtime="${temporary_root}/runtime"
mkdir -p "${temporary_runtime}"
trap cleanup EXIT INT TERM

# Do not modify the original file. Drop only this test's keys, then append the
# force policy and a session-owned runtime base.
sed -E '/^[[:space:]]*steamos_(virtual_display_enabled|virtual_display_mode|runtime_directory)[[:space:]]*=/d' "${config_file}" >"${temporary_config}"
cat >>"${temporary_config}" <<EOF
steamos_virtual_display_enabled = true
steamos_virtual_display_mode = force
steamos_runtime_directory = ${temporary_runtime}
EOF
redact_config "${temporary_config}" >"${report_dir}/config-redacted.txt"

if systemctl --user is-active --quiet steamshine.service; then
  service_was_active=true
  systemctl --user stop steamshine.service
fi

printf '%s\n' 'Hardware test mode: FORCE'
printf '%s\n' 'Physical displays may remain connected for this first force-mode test.'
printf '%s\n' 'Connect Moonlight and start Desktop or a game. Press Enter only after video is visible or a connection error is displayed.'
"${binary}" "${temporary_config}" >"${report_dir}/service.log" 2>&1 &
test_pid="$!"

web_ready=false
for _ in $(seq 1 30); do
  if curl --insecure --silent --output /dev/null "https://localhost:47990/"; then
    web_ready=true
    break
  fi
  kill -0 "${test_pid}" 2>/dev/null || die 'Temporary force-mode SteamShine process exited before its Web UI was ready.'
  sleep 1
done
"${web_ready}" || die 'Temporary force-mode SteamShine Web UI did not become ready within 30 seconds.'

find "${temporary_runtime}" -maxdepth 3 -printf '%y %p\n' >"${report_dir}/runtime-tree.txt" 2>/dev/null || true
ps -eo pid=,pgid=,ppid=,args= >"${report_dir}/process-tree.txt"
{ find /sys/class/drm -maxdepth 2 -name status -exec sh -c 'printf "%s=" "$1"; cat "$1"' _ {} \;; } >"${report_dir}/drm-connectors.txt" 2>/dev/null || true
vulkaninfo --summary >"${report_dir}/vulkan-summary.txt" 2>&1 || true

read -r -p 'Press Enter after the Moonlight result is visible: ' _
find "${temporary_runtime}" -maxdepth 3 -printf '%y %p\n' >"${report_dir}/runtime-tree-after.txt" 2>/dev/null || true
python3 - "${report_dir}/result.json" "${temporary_runtime}" "${test_pid}" <<'PY'
"""Write non-sensitive force-mode hardware-test evidence."""
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

Path(sys.argv[1]).write_text(json.dumps({
    "timestamp": datetime.now(timezone.utc).isoformat(),
    "mode": "force",
    "runtime_directory": sys.argv[2],
    "steamshine_pid": int(sys.argv[3]),
    "operator_confirmed_result": True,
    "video_confirmed": None,
    "audio_confirmed": None,
    "input_confirmed": None,
    "note": "Inspect service.log and Moonlight evidence; operator confirmation alone is not acceptance.",
}, indent=2) + "\n", encoding="utf-8")
PY
printf 'Force-mode hardware report written to: %s\n' "${report_dir}"
