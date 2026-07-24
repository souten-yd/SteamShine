#!/usr/bin/env bash
# Collect optional host counters without making pidstat a SteamOS requirement.
set -euo pipefail
out_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${HOME}/.local/state/steamshine/hardware-tests}"
mkdir -p "${out_dir}"
report="${out_dir}/latency.log"
seconds="${1:-60}"
[[ "${seconds}" =~ ^[0-9]+$ ]] || { echo 'Usage: test-steamos-latency.sh [seconds]' >&2; exit 2; }

proc_ticks() {
  local pid="$1"
  local stat="${PROC_ROOT:-/proc}/${pid}/stat"
  [[ -r "${stat}" ]] || return 0
  awk 'NR == 1 {print ($14 + $15) + 0}' "${stat}"
}

proc_snapshot() {
  local phase="$1" pid="$2" root stat status io
  root="${PROC_ROOT:-/proc}"
  stat="${root}/${pid}/stat"
  status="${root}/${pid}/status"
  io="${root}/${pid}/io"
  [[ -r "${stat}" ]] || { echo "proc_sample phase=${phase} pid=${pid} unavailable"; return 0; }
  local ticks; ticks="$(getconf CLK_TCK 2>/dev/null || echo 100)"
  awk -v phase="${phase}" -v pid="${pid}" -v ticks="${ticks}" 'NR == 1 {print "proc_sample phase=" phase " pid=" pid " utime=" $14 " stime=" $15 " total_ticks=" ($14 + $15) " clk_tck=" ticks}' "${stat}"
  awk '$1 == "VmRSS:" || $1 == "voluntary_ctxt_switches:" || $1 == "nonvoluntary_ctxt_switches:" {printf "%s=%s ", $1, $2} END {print ""}' "${status}" 2>/dev/null || echo 'status=unavailable'
  awk '$1 == "read_bytes:" || $1 == "write_bytes:" || $1 == "syscr:" || $1 == "syscw:" {printf "%s=%s ", $1, $2} END {print ""}' "${io}" 2>/dev/null || echo 'io=unavailable'
}
{
  echo 'Use identical game, scene, resolution, FPS, codec, bitrate, client, and network for both runs.'
  echo 'Record Moonlight statistics (RTT/decoder/render/dropped frames) during each 60-second run.'
  echo "timestamp=$(date --iso-8601=seconds)"
  pid="$(pgrep -n steamshine 2>/dev/null | head -n1 || true)"
  if command -v pidstat >/dev/null 2>&1 && [[ -n "${pid}" ]]; then
    if ((seconds > 0)); then
      pidstat -rud -p "${pid}" 1 "${seconds}" || echo 'DIAGNOSTIC_WARN: pidstat collection failed'
    else
      echo 'DIAGNOSTIC_WARN: pidstat sample duration is zero; using /proc snapshot'
      proc_snapshot before "${pid}"
      proc_snapshot after "${pid}"
    fi
  elif [[ -n "${pid}" ]]; then
    local_before_ticks="$(proc_ticks "${pid}")"
    local_started_ns="$(date +%s%N)"
    echo 'DIAGNOSTIC_WARN: pidstat unavailable; using /proc snapshot'
    proc_snapshot before "${pid}"
    ((seconds > 0)) && sleep "${seconds}"
    local_finished_ns="$(date +%s%N)"
    local_after_ticks="$(proc_ticks "${pid}")"
    proc_snapshot after "${pid}"
    if [[ "${local_before_ticks}" =~ ^[0-9]+$ && "${local_after_ticks}" =~ ^[0-9]+$ ]]; then
      awk -v pid="${pid}" -v before="${local_before_ticks}" -v after="${local_after_ticks}" -v started="${local_started_ns}" -v finished="${local_finished_ns}" -v ticks="$(getconf CLK_TCK 2>/dev/null || echo 100)" '
        BEGIN {
          elapsed = (finished - started) / 1000000000;
          if (elapsed > 0 && ticks > 0) {
            printf "proc_cpu_delta pid=%s cpu_ticks=%d elapsed_seconds=%.3f cpu_percent=%.2f\\n", pid, after - before, elapsed, ((after - before) / ticks) * 100 / elapsed;
          } else {
            print "proc_cpu_delta unavailable";
          }
        }'
    else
      echo 'proc_cpu_delta unavailable'
    fi
  else
    echo 'DIAGNOSTIC_WARN: SteamShine PID unavailable'
  fi
  command -v mangohud >/dev/null 2>&1 && echo 'Capture game frametime with MangoHud logging.' || echo 'DIAGNOSTIC_WARN: MangoHud unavailable'
  command -v amd-smi >/dev/null 2>&1 && amd-smi metric -g 2>&1 || echo 'DIAGNOSTIC_WARN: optional AMD telemetry unavailable'
} | tee "${report}"
