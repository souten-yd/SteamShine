#!/usr/bin/env bash
# Hold the stock SteamOS Game Mode launcher while every physical DRM connector
# is absent, preventing SDDM's Relogin loop from repeatedly rebuilding it.
set -Eeuo pipefail

readonly vendor_launcher="${1:-/usr/lib/steamos/gamescope-session}"
readonly drm_root="${STEAMSHINE_DRM_ROOT:-/sys/class/drm}"
readonly poll_seconds="${STEAMSHINE_CONNECTOR_POLL_SECONDS:-1}"

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

[[ -x "${vendor_launcher}" ]] || {
  printf 'steamshine-gamescope-session-guard: vendor launcher is not executable: %s\n' "${vendor_launcher}" >&2
  exit 1
}

if ! physical_connector_connected; then
  if command -v logger >/dev/null; then
    logger -t steamshine-gamescope-guard 'No physical DRM connector; holding the stock Game Mode session for the SteamShine headless path' || true
  fi
  until physical_connector_connected; do
    sleep "${poll_seconds}"
  done
  if command -v logger >/dev/null; then
    logger -t steamshine-gamescope-guard 'Physical DRM connector detected; resuming the stock Game Mode launcher' || true
  fi
fi

exec "${vendor_launcher}"
