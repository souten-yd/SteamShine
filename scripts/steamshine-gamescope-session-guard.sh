#!/usr/bin/env bash
# Hold the stock SteamOS Game Mode launcher while every physical DRM connector
# is absent or a live SteamShine handoff lease owns the graphical session.
set -Eeuo pipefail

readonly vendor_launcher="${1:-/usr/lib/steamos/gamescope-session}"
readonly drm_root="${STEAMSHINE_DRM_ROOT:-/sys/class/drm}"
readonly poll_seconds="${STEAMSHINE_CONNECTOR_POLL_SECONDS:-1}"
readonly runtime_root="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
readonly lease_path="${STEAMSHINE_STOCK_HANDOFF_LEASE:-${runtime_root}/steamshine/stock-session-handoff.lease}"
readonly proc_root="${STEAMSHINE_PROC_ROOT:-/proc}"
readonly boot_id_path="${STEAMSHINE_BOOT_ID_PATH:-/proc/sys/kernel/random/boot_id}"

physical_connector_connected() {
  local connector name status
  shopt -s nullglob
  for connector in "${drm_root}"/card*-*; do
    name="${connector##*/}"
    [[ "${name}" != *Writeback* && -r "${connector}/status" ]] || continue
    IFS= read -r status <"${connector}/status" || true
    [[ "${status}" == connected ]] && return 0
  done
  return 1
}

live_handoff_lease() {
  local key value version='' boot_id='' owner_pid='' owner_start_time=''
  local current_boot_id proc_stat proc_tail proc_uid lease_mode
  local -a proc_fields

  [[ ! -L "${lease_path}" && -f "${lease_path}" && -O "${lease_path}" ]] || return 1
  lease_mode="$(stat -c '%a' -- "${lease_path}" 2>/dev/null || true)"
  [[ "${lease_mode}" == '600' ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  while IFS='=' read -r key value; do
    case "${key}" in
      version) version="${value}" ;;
      boot_id) boot_id="${value}" ;;
      owner_pid) owner_pid="${value}" ;;
      owner_start_time) owner_start_time="${value}" ;;
    esac
  done <"${lease_path}"
  [[ "${version}" == '1' && "${owner_pid}" =~ ^[1-9][0-9]*$ && "${owner_start_time}" =~ ^[1-9][0-9]*$ ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  IFS= read -r current_boot_id <"${boot_id_path}" || current_boot_id=''
  [[ -n "${current_boot_id}" && "${boot_id}" == "${current_boot_id}" ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  proc_uid="$(stat -c '%u' -- "${proc_root}/${owner_pid}" 2>/dev/null || true)"
  [[ "${proc_uid}" == "$(id -u)" ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  IFS= read -r proc_stat <"${proc_root}/${owner_pid}/stat" || proc_stat=''
  [[ "${proc_stat}" == *') '* ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  proc_tail="${proc_stat##*) }"
  read -r -a proc_fields <<<"${proc_tail}"
  [[ "${#proc_fields[@]}" -ge 20 && "${proc_fields[19]}" == "${owner_start_time}" ]] || {
    rm -f -- "${lease_path}"
    return 1
  }
  return 0
}

[[ -x "${vendor_launcher}" ]] || {
  printf 'steamshine-gamescope-session-guard: vendor launcher is not executable: %s\n' "${vendor_launcher}" >&2
  exit 1
}

if ! physical_connector_connected || live_handoff_lease; then
  if command -v logger >/dev/null; then
    logger -t steamshine-gamescope-guard 'Holding the stock Game Mode launcher for the SteamShine headless path' || true
  fi
  until physical_connector_connected && ! live_handoff_lease; do
    sleep "${poll_seconds}"
  done
  if command -v logger >/dev/null; then
    logger -t steamshine-gamescope-guard 'Connector and handoff lease permit stock Game Mode; resuming the vendor launcher' || true
  fi
fi

exec "${vendor_launcher}"
