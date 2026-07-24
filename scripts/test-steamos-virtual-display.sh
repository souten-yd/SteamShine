#!/usr/bin/env bash
# Headless SteamOS acceptance harness. Start it before connecting Moonlight.
# shellcheck disable=SC1091,SC2009
set -euo pipefail
runtime_dir="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is required}/steamshine"
state_dir="${STATE_ROOT:-${HOME}/.local/state}/steamshine"
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
if [[ -n "${COMMAND_PATH:-}" ]]; then
  PATH="${COMMAND_PATH}:${PATH}"
fi

write_summary() {
  local result="$1" capture_event=false streaming_event=false cleanup_event=false metrics packets bytes idr captured_frames
  grep -Rqs 'SteamOS virtual display capture attached' "${report_dir}"/service-*.log 2>/dev/null && capture_event=true
  grep -Rqs 'SteamOS virtual display streaming started' "${report_dir}"/service-*.log 2>/dev/null && streaming_event=true
  grep -Rqs 'SteamOS virtual display stopping owned Gamescope session' "${report_dir}"/service-*.log 2>/dev/null && cleanup_event=true
  metrics="$(grep -Rh 'SteamOS virtual display encoded packets=' "${report_dir}"/service-*.log 2>/dev/null | tail -n 1 || true)"
  packets="$(sed -n 's/.*encoded packets=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  bytes="$(sed -n 's/.* bytes=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  idr="$(sed -n 's/.* idr=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  captured_frames="$(sed -n 's/.* captured_frames=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  [[ -n "${packets}" ]] || packets=null
  [[ -n "${bytes}" ]] || bytes=null
  [[ -n "${idr}" ]] || idr=null
  [[ -n "${captured_frames}" ]] || captured_frames=null
  cat >"${report_dir}/hardware-report.json" <<EOF
{
  "started_at": "${test_started}",
  "completed_at": "$(date --iso-8601=seconds)",
  "result": "${result}",
  "connect_disconnect_cycles": ${completed_attempts},
  "capture_attached_evidence": ${capture_event},
  "streaming_started_evidence": ${streaming_event},
  "cleanup_started_evidence": ${cleanup_event},
  "encoded_packet_count": ${packets},
  "encoded_bytes": ${bytes},
  "idr_frame_count": ${idr},
  "captured_frame_count": ${captured_frames},
  "note": "Frame and packet counters are emitted once during owned-session cleanup; null means no completed live stream was observed."
}
EOF
}
trap 'result=$?; write_summary "$( ((result == 0)) && printf pass || printf fail )"' EXIT

# Verify that a completed owned virtual session emitted actual GameStream
# packet evidence. The counters are produced in the stream send path and
# written once when the owned session stops, so a ready Wayland socket alone
# can never satisfy the hardware acceptance test. $1 is the post-disconnect
# journal label. Returns zero only when packets, bytes, and an IDR are present.
require_encoded_stream_evidence() {
  local label="$1" journal metrics packets bytes idr captured_frames
  journal="${report_dir}/service-${label}.log"
  [[ -r "${journal}" ]] || {
    echo "FAIL: no SteamShine service evidence for ${label}" | tee -a "${report}"
    return 1
  }
  metrics="$(grep 'SteamOS virtual display encoded packets=' "${journal}" | tail -n 1 || true)"
  packets="$(sed -n 's/.*encoded packets=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  bytes="$(sed -n 's/.* bytes=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  idr="$(sed -n 's/.* idr=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  captured_frames="$(sed -n 's/.* captured_frames=\([0-9][0-9]*\).*/\1/p' <<<"${metrics}")"
  if [[ ! "${packets}" =~ ^[1-9][0-9]*$ || ! "${bytes}" =~ ^[1-9][0-9]*$ || ! "${idr}" =~ ^[1-9][0-9]*$ || ! "${captured_frames}" =~ ^[1-9][0-9]*$ ]]; then
    echo "FAIL: ${label} has no captured-frame, packet, byte, and IDR evidence" | tee -a "${report}"
    return 1
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "${label}" "${captured_frames}" "${packets}" "${bytes}" "${idr}" >>"${report_dir}/encoded-stream-evidence.tsv"
  printf 'encoded_stream_evidence=%s captured_frames=%s packets=%s bytes=%s idr=%s\n' "${label}" "${captured_frames}" "${packets}" "${bytes}" "${idr}" | tee -a "${report}"
}

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
  local directory pid_file pid environment value proc_root recorded_process=false
  proc_root="${PROC_ROOT:-/proc}"
  # SteamShine records the Gamescope group leader at spawn time. Prefer that
  # ownership record over a broad /proc scan, then retain the scan as a
  # diagnostic fallback for children in the same private runtime.
  while IFS= read -r directory; do
    pid_file="${directory}/gamescope.pid"
    [[ -r "${pid_file}" && ! -L "${pid_file}" ]] || continue
    pid="$(<"${pid_file}")"
    [[ "${pid}" =~ ^[1-9][0-9]*$ && -r "${proc_root}/${pid}/environ" ]] || continue
    while IFS= read -r -d '' environment; do
      value="${environment#XDG_RUNTIME_DIR=}"
      if [[ "${environment}" == XDG_RUNTIME_DIR=* && "${value}" == "${directory}" ]]; then
        printf '%s\n' "${pid}"
        recorded_process=true
        break
      fi
    done <"${proc_root}/${pid}/environ" 2>/dev/null || true
  done < <(owned_session_directories)
  if "${recorded_process}"; then
    return 0
  fi
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
    if [[ -r "${PROC_ROOT:-/proc}/${pid}/cmdline" ]]; then
      command="$(tr '\0' ' ' <"${PROC_ROOT:-/proc}/${pid}/cmdline" 2>/dev/null || true)"
    else
      command=unknown
    fi
    printf 'owned_gamescope_pid=%s pgid=%s command=%s\n' "${pid}" "${pgid:-unknown}" "${command:-unknown}"
  done < <(owned_gamescope_processes)
}

# Require the private runtime/socket/process evidence while Moonlight remains
# connected. Journal messages alone can be stale or refer to a different
# desktop session, so they are not sufficient proof that SteamShine owns the
# headless display currently being streamed.
require_owned_virtual_session_evidence() {
  local label="$1" directory socket proc_root pid session_count=0 process_count=0
  proc_root="${PROC_ROOT:-/proc}"
  while IFS= read -r directory; do
    [[ -n "${directory}" ]] || continue
    session_count=$((session_count + 1))
    socket="${directory}/gamescope-0"
    if [[ ! -S "${socket}" ]]; then
      echo "FAIL: ${label} owned runtime has no private Gamescope Wayland socket: ${socket}" | tee -a "${report}"
      return 1
    fi
    printf 'owned_runtime=%s owned_wayland_socket=%s\n' "${directory}" "${socket}" >>"${report_dir}/owned-session-evidence.tsv"
  done < <(owned_session_directories)
  if ((session_count == 0)); then
    echo "FAIL: ${label} has no marker-owned SteamShine virtual runtime" | tee -a "${report}"
    return 1
  fi
  while IFS= read -r pid; do
    [[ -n "${pid}" && -d "${proc_root}/${pid}" ]] || continue
    process_count=$((process_count + 1))
    printf 'owned_gamescope_pid=%s\n' "${pid}" >>"${report_dir}/owned-session-evidence.tsv"
  done < <(owned_gamescope_processes)
  if ((process_count == 0)); then
    echo "FAIL: ${label} has no live process using the owned virtual runtime" | tee -a "${report}"
    return 1
  fi
  printf 'owned_session_evidence=%s sessions=%s processes=%s\n' "${label}" "${session_count}" "${process_count}" | tee -a "${report}"
}

# The hook is intentionally inert in normal installations. Shell integration
# tests set both variables to model an owned socket appearing at connect time
# and disappearing at disconnect time without a GPU or Gamescope daemon.
run_test_event_hook() {
  local event="$1"
  [[ "${STEAMSHINE_TEST_MODE:-}" == 1 && -n "${STEAMSHINE_TEST_EVENT_HOOK:-}" ]] || return 0
  [[ -x "${STEAMSHINE_TEST_EVENT_HOOK}" ]] || {
    echo "FAIL: STEAMSHINE_TEST_EVENT_HOOK is not executable" | tee -a "${report}"
    return 1
  }
  "${STEAMSHINE_TEST_EVENT_HOOK}" "${event}" "${runtime_dir}" "${PROC_ROOT:-/proc}"
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
    find "${SYS_ROOT:-/sys}/class/drm" -name uevent -exec sh -c 'echo ---$1; grep -E "PCI_SLOT_NAME|DRIVER" "$1"' _ {} \; 2>/dev/null || true
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
  run_test_event_hook "connected-${attempt}"
  collect "connected-${attempt}"
  require_owned_virtual_session_evidence "connected-${attempt}"
  if [[ "${attempt}" -eq 1 ]]; then
    echo "Keep Moonlight connected for ${sample_seconds} seconds while latency and SteamShine write counters are collected."
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-latency.sh" "${sample_seconds}" &
    latency_process=$!
    STEAMSHINE_HARDWARE_REPORT_DIR="${report_dir}" "${script_dir}/test-steamos-ssd-writes.sh" "${sample_seconds}" &
    writes_process=$!
    wait "${latency_process}"
    wait "${writes_process}"
  fi
  echo "Attempt ${attempt}: disconnect Moonlight now, then press Enter after cleanup."
  read -r
  run_test_event_hook "disconnected-${attempt}"
  collect "disconnected-${attempt}"
  require_encoded_stream_evidence "disconnected-${attempt}"
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
