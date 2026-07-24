#!/usr/bin/env bash
# Headless SteamOS acceptance harness. Start it before connecting Moonlight.
# shellcheck disable=SC1091,SC2009
set -euo pipefail
runtime_dir="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}/steamshine"
state_dir="${HOME}/.local/state/steamshine"
report_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${state_dir}/hardware-tests/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "${report_dir}"
report="${report_dir}/virtual-display.log"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
steamshine_binary="${STEAMSHINE_BINARY:-${HOME}/.local/bin/steamshine}"

owned_gamescope_processes() {
  local environment pid value
  while IFS= read -r pid; do
    [[ -r "/proc/${pid}/environ" ]] || continue
    while IFS= read -r -d '' environment; do
      value="${environment#XDG_RUNTIME_DIR=}"
      if [[ "${environment}" == XDG_RUNTIME_DIR=* && "${value}" == "${runtime_dir}"/session-* ]]; then
        printf '%s\n' "${pid}"
        break
      fi
    done <"/proc/${pid}/environ"
  done < <(pgrep -x gamescope || true)
}

collect() {
  {
    echo "== $1 $(date --iso-8601=seconds) =="
    "${script_dir}/collect-steamos-runtime-baseline.sh" 2>&1 || true
    . /etc/os-release 2>/dev/null && printf 'OS=%s\n' "${PRETTY_NAME:-unknown}"
    uname -a; gamescope --version 2>&1 || true
    gamescope --help 2>&1 | grep -E -- '--backend|headless|--nested-(width|height|refresh)|--expose-wayland|--prefer-vk-device|--hdr-enabled' || true
    find /sys/class/drm -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
    vainfo 2>&1 || true; vulkaninfo --summary 2>&1 || true
    if command -v rocminfo >/dev/null 2>&1; then rocminfo 2>&1 || true; fi
    if command -v rocm-smi >/dev/null 2>&1; then rocm-smi 2>&1 || true; fi
    if command -v amd-smi >/dev/null 2>&1; then amd-smi metric -g 2>&1 || true; fi
    pw-cli list-objects Node 2>&1 || true; find "${XDG_RUNTIME_DIR}" -maxdepth 1 -type s -name 'wayland-*' -print 2>/dev/null || true
    pgrep -a steamshine || true; pgrep -a gamescope || true; ps -eo pid,pgid,ppid,cmd | grep -E '[s]teamshine|[g]amescope' || true
    printf 'owned_gamescope_pids='; owned_gamescope_processes | paste -sd, - || true
    find "${runtime_dir}" -maxdepth 3 -print 2>/dev/null || true
  } | tee -a "${report}"
}
echo "Disconnect physical displays, start SteamShine, then connect Moonlight ten times. Report: ${report}"
collect before
if [[ ! -x "${steamshine_binary}" ]]; then
  echo "FAIL: installed SteamShine binary is unavailable: ${steamshine_binary}" | tee -a "${report}"
  exit 1
fi
if ! "${steamshine_binary}" vulkan-video-probe 2>&1 | tee -a "${report}"; then
  echo 'FAIL: Vulkan Video H.264 preflight failed; refusing Moonlight acceptance cycles' | tee -a "${report}"
  exit 1
fi
echo 'Vulkan Video H.264 preflight passed; verify actual IDR/bitstream output during the following Moonlight stream acceptance cycles.' | tee -a "${report}"
for attempt in $(seq 1 10); do
  echo "Attempt ${attempt}: connect Moonlight now, then press Enter once the stream is established."
  read -r
  collect "connected-${attempt}"
  if [[ "${attempt}" -eq 1 ]]; then
    echo 'Keep Moonlight connected for 60 seconds while latency and SteamShine write counters are collected.'
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-latency.sh" &
    latency_process=$!
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-ssd-writes.sh" 60 &
    writes_process=$!
    wait "${latency_process}"
    wait "${writes_process}"
  fi
  echo "Attempt ${attempt}: disconnect Moonlight now, then press Enter after cleanup."
  read -r
  collect "disconnected-${attempt}"
  if owned_gamescope_processes | grep -q .; then echo "FAIL: owned Gamescope remains" | tee -a "${report}"; exit 1; fi
  if find "${runtime_dir}" -mindepth 1 -name 'session-*' -print -quit | grep -q .; then echo "FAIL: owned runtime session remains" | tee -a "${report}"; exit 1; fi
done
for capability in video audio keyboard mouse gamepad; do
  read -r -p "Did ${capability} work during the acceptance cycles? [y/N] " answer
  if [[ ! "${answer}" =~ ^[Yy]$ ]]; then
    echo "FAIL: operator did not confirm ${capability}" | tee -a "${report}"
    exit 1
  fi
done
echo "PASS: ten operator-verified connect/disconnect cycles completed" | tee -a "${report}"
