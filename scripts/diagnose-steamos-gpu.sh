#!/usr/bin/env bash
# @file scripts/diagnose-steamos-gpu.sh
# @brief Collect non-sensitive SteamOS DRM, GPU, permission, and service diagnostics.
set -Eeuo pipefail

service_name="${STEAMSHINE_SERVICE_NAME:-steamshine.service}"
config_path="${STEAMSHINE_CONFIG:-}"

say_section() {
  printf '\n=== %s ===\n' "$1"
}

read_attribute() {
  local path="$1"
  if [[ -r "${path}" ]]; then
    tr -d '\n' <"${path}"
  fi
}

say_section 'DRM nodes'
ls -la /dev/dri 2>&1 || true

say_section 'Current user and groups'
id || true
getent group video || true
getent group render || true

say_section 'DRM render-node map'
for render_node in /dev/dri/renderD*; do
  [[ -e "${render_node}" ]] || continue
  render_name="${render_node##*/}"
  device_path="/sys/class/drm/${render_name}/device"
  device_realpath="$(readlink -f "${device_path}" 2>/dev/null || true)"
  driver="$(basename "$(readlink -f "${device_path}/driver" 2>/dev/null || true)")"
  card_node=""
  if [[ -n "${device_realpath}" ]]; then
    for card_path in /sys/class/drm/card[0-9]*; do
      [[ -e "${card_path}/device" ]] || continue
      [[ "$(readlink -f "${card_path}/device" 2>/dev/null || true)" == "${device_realpath}" ]] || continue
      card_node="/dev/dri/${card_path##*/}"
      break
    done
  fi
  readable=false
  writable=false
  open_errno=0
  test -r "${render_node}" && readable=true
  test -w "${render_node}" && writable=true
  if ! exec {render_fd}<>"${render_node}"; then
    open_errno=$?
  else
    exec {render_fd}>&-
  fi
  printf 'render_node=%s\ncard_node=%s\npci_bdf=%s\ndriver=%s\nvendor=%s\ndevice_id=%s\nvram_bytes=%s\nreadable=%s\nwritable=%s\nopen_errno=%s\n' \
    "${render_node}" "${card_node}" "${device_realpath##*/}" "${driver}" \
    "$(read_attribute "${device_path}/vendor")" "$(read_attribute "${device_path}/device")" \
    "$(read_attribute "${device_path}/mem_info_vram_total")" "${readable}" "${writable}" "${open_errno}"
  udevadm info --query=property --name="${render_node}" 2>/dev/null |
    grep -E '^(DEVNAME|DEVPATH|ID_VENDOR|ID_MODEL|PCI_ID|ID_PATH|DRIVER|MAJOR|MINOR)=' || true
  printf '\n'
done

say_section 'Systemd user service'
systemctl --user show "${service_name}" \
  -p ExecStart -p Environment -p FragmentPath -p DropInPaths -p User -p Group \
  -p SupplementaryGroups -p PrivateDevices -p DevicePolicy -p DeviceAllow \
  -p ProtectSystem -p ProtectHome -p RestrictAddressFamilies || true

say_section 'Systemd user DRM access'
# shellcheck disable=SC2016 # The quoted command is evaluated by systemd-run's Bash.
systemd-run --user --wait --pipe /bin/bash -lc '
  set -u
  id
  for node in /dev/dri/renderD*; do
    [[ -e "${node}" ]] || continue
    readable=false; writable=false; open_errno=0
    test -r "${node}" && readable=true
    test -w "${node}" && writable=true
    if ! exec {fd}<>"${node}"; then open_errno=$?; else exec {fd}>&-; fi
    printf "%s readable=%s writable=%s open_errno=%s\\n" "${node}" "${readable}" "${writable}" "${open_errno}"
  done
' || true

if [[ -z "${config_path}" ]]; then
  config_path="$(systemctl --user show "${service_name}" -p ExecStart --value 2>/dev/null | sed -nE 's/.*steamshine[[:space:]]+([^ ;}]+).*/\1/p' | head -n 1)"
fi
say_section 'SteamShine GPU configuration'
printf 'config_path=%s\n' "${config_path:-unresolved}"
if [[ -n "${config_path}" && -r "${config_path}" ]]; then
  grep -nE '^(steamos_.*gpu|adapter_name|output_name)[[:space:]]*=' "${config_path}" || true
fi

say_section 'Vulkan summary'
vulkaninfo --summary 2>&1 || true

say_section 'Recent SteamShine GPU diagnostics'
journalctl --user -u "${service_name}" --since '-15 min' --no-pager 2>/dev/null |
  grep -Ei 'game gpu|capture gpu|encoder gpu|render node|card node|drm|amdgpu|pci|vulkan|virtual session|gamescope|error|failed' || true
