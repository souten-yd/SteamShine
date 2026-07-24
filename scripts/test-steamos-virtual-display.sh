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
steamshine_config="${STEAMSHINE_CONFIG:-${HOME}/.config/steamshine/sunshine.conf}"
test_started="$(date --iso-8601=seconds)"
completed_attempts=0
sample_seconds="${STEAMSHINE_HARDWARE_SAMPLE_SECONDS:-60}"
[[ "${sample_seconds}" =~ ^[0-9]+$ ]] || { echo 'FAIL: STEAMSHINE_HARDWARE_SAMPLE_SECONDS must be a non-negative integer' >&2; exit 2; }

write_summary() {
  local result="$1" capture_event=false streaming_event=false cleanup_event=false
  grep -Rqs 'SteamOS virtual display capture attached' "${report_dir}"/service-*.log 2>/dev/null && capture_event=true
  grep -Rqs 'SteamOS virtual display streaming started' "${report_dir}"/service-*.log 2>/dev/null && streaming_event=true
  grep -Rqs 'SteamOS virtual display stopping owned Gamescope session' "${report_dir}"/service-*.log 2>/dev/null && cleanup_event=true
  cat >"${report_dir}/hardware-report.json" <<EOF
{
  "started_at": "${test_started}",
  "completed_at": "$(date --iso-8601=seconds)",
  "result": "${result}",
  "connect_disconnect_cycles": ${completed_attempts},
  "capture_attached_evidence": ${capture_event},
  "streaming_started_evidence": ${streaming_event},
  "cleanup_started_evidence": ${cleanup_event},
  "encoded_packet_count": null,
  "idr_frame_count": null,
  "note": "Packet and IDR counters are not yet exported by the streaming backend; null is intentional."
}
EOF
}
trap 'result=$?; write_summary "$( ((result == 0)) && printf pass || printf fail )"' EXIT

owned_session_directories() {
  local directory marker
  for directory in "${runtime_dir}"/session-*; do
    [[ -d "${directory}" && ! -L "${directory}" ]] || continue
    marker="${directory}/steamshine-owner"
    [[ -f "${marker}" && ! -L "${marker}" ]] || continue
    [[ "$(<"${marker}")" == 'steamshine-steamos-virtual-session-v1' ]] || continue
    printf '%s\n' "${directory}"
  done
}

owned_gamescope_processes() {
  local environment pid value proc_root
  proc_root="${PROC_ROOT:-/proc}"
  for pid in "${proc_root}"/[0-9]*; do
    pid="${pid##*/}"
    [[ -r "${proc_root}/${pid}/environ" ]] || continue
    # hidepid may report the file as readable but reject the actual open.
    head -c 1 "${proc_root}/${pid}/environ" >/dev/null 2>&1 || continue
    while IFS= read -r -d '' environment; do
      value="${environment#XDG_RUNTIME_DIR=}"
      if [[ "${environment}" == XDG_RUNTIME_DIR=* && "${value}" == "${runtime_dir}"/session-* ]]; then
        printf '%s\n' "${pid}"
        break
      fi
    done <"${proc_root}/${pid}/environ" 2>/dev/null || true
  done
}

owned_gamescope_report() {
  local pid pgid command
  while IFS= read -r pid; do
    pgid="$(ps -o pgid= -p "${pid}" 2>/dev/null | tr -d ' ' || true)"
    command="$(tr '\0' ' ' <"${PROC_ROOT:-/proc}/${pid}/cmdline" 2>/dev/null || true)"
    printf 'owned_gamescope_pid=%s pgid=%s command=%s\n' "${pid}" "${pgid:-unknown}" "${command:-unknown}"
  done < <(owned_gamescope_processes)
}

collect_service_evidence() {
  local label journal
  label="$1"
  journal="${report_dir}/service-${label}.log"
  if ! command -v journalctl >/dev/null 2>&1; then
    echo 'SERVICE_EVIDENCE_SKIPPED: journalctl unavailable'
    return 0
  fi
  journalctl --user --unit=steamshine --since "${test_started}" --no-pager >"${journal}" 2>&1 || {
    echo 'DIAGNOSTIC_WARN: unable to collect SteamShine user-service journal'
    return 0
  }
  printf 'service_journal=%s\n' "${journal}"
  grep -E 'SteamOS virtual display (capture attached|streaming started|stopping owned Gamescope session)' "${journal}" || true
}

collect() {
  {
    echo "== $1 $(date --iso-8601=seconds) =="
    "${script_dir}/collect-steamos-runtime-baseline.sh" 2>&1 || true
    . /etc/os-release 2>/dev/null && printf 'OS=%s\n' "${PRETTY_NAME:-unknown}"
    uname -a; gamescope --version 2>&1 || true
    gamescope --help 2>&1 | grep -E -- '--backend|headless|--nested-(width|height|refresh)|--expose-wayland|--prefer-vk-device|--hdr-enabled' || true
    find /sys/class/drm -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
    if command -v vainfo >/dev/null 2>&1; then vainfo 2>&1 || echo 'DIAGNOSTIC_WARN: vainfo failed'; else echo 'VAAPI_PROBE_SKIPPED: vainfo unavailable'; fi
    vulkaninfo --summary 2>&1 || true
    if command -v rocminfo >/dev/null 2>&1; then rocminfo 2>&1 || true; fi
    if command -v rocm-smi >/dev/null 2>&1; then rocm-smi 2>&1 || true; fi
    if command -v amd-smi >/dev/null 2>&1; then amd-smi metric -g 2>&1 || true; fi
    if command -v pw-dump >/dev/null 2>&1; then
      pw-dump >"${report_dir}/pipewire-$1.json" 2>"${report_dir}/pipewire-$1.stderr" || echo 'DIAGNOSTIC_WARN: pw-dump failed'
      printf 'pipewire_dump=%s\n' "${report_dir}/pipewire-$1.json"
    else
      echo 'PIPEWIRE_PROBE_SKIPPED: pw-dump unavailable'
    fi
    find "${XDG_RUNTIME_DIR}" -maxdepth 1 -type s -name 'wayland-*' -print 2>/dev/null || true
    pgrep -a steamshine || true; pgrep -a gamescope || true; ps -eo pid,pgid,ppid,cmd | grep -E '[s]teamshine|[g]amescope' || true
    printf 'owned_session_directories='; owned_session_directories | paste -sd, - || true
    owned_gamescope_report
    collect_service_evidence "$1"
    find "${runtime_dir}" -maxdepth 3 -print 2>/dev/null || true
  } | tee -a "${report}"
}
echo "Disconnect physical displays, start SteamShine, then connect Moonlight ten times. Report: ${report}"
collect before
if [[ ! -x "${steamshine_binary}" ]]; then
  echo "FAIL: installed SteamShine binary is unavailable: ${steamshine_binary}" | tee -a "${report}"
  exit 1
fi
if ! "${steamshine_binary}" "${steamshine_config}" --vulkan-video-probe 2>&1 | tee -a "${report}"; then
  echo 'FAIL: Vulkan Video H.264 preflight failed; refusing Moonlight acceptance cycles' | tee -a "${report}"
  exit 1
fi
echo 'Vulkan Video H.264 preflight passed; verify actual IDR/bitstream output during the following Moonlight stream acceptance cycles.' | tee -a "${report}"
for attempt in $(seq 1 10); do
  echo "Attempt ${attempt}: connect Moonlight now, then press Enter once the stream is established."
  read -r
  collect "connected-${attempt}"
  if [[ "${attempt}" -eq 1 ]]; then
    echo "Keep Moonlight connected for ${sample_seconds} seconds while latency and SteamShine write counters are collected."
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-latency.sh" &
    latency_process=$!
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-ssd-writes.sh" "${sample_seconds}" &
    writes_process=$!
    wait "${latency_process}"
    wait "${writes_process}"
  fi
  echo "Attempt ${attempt}: disconnect Moonlight now, then press Enter after cleanup."
  read -r
  collect "disconnected-${attempt}"
  if owned_gamescope_processes | grep -q .; then echo "FAIL: owned Gamescope remains" | tee -a "${report}"; exit 1; fi
  if owned_session_directories | grep -q .; then echo "FAIL: owned runtime session remains" | tee -a "${report}"; exit 1; fi
  completed_attempts="${attempt}"
done
for capability in video audio keyboard mouse gamepad; do
  read -r -p "Did ${capability} work during the acceptance cycles? [y/N] " answer
  if [[ ! "${answer}" =~ ^[Yy]$ ]]; then
    echo "FAIL: operator did not confirm ${capability}" | tee -a "${report}"
    exit 1
  fi
done
echo "PASS: ten operator-verified connect/disconnect cycles completed" | tee -a "${report}"
