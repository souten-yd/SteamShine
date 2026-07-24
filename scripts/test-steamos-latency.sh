#!/usr/bin/env bash
# Collect optional host counters without making pidstat a SteamOS requirement.
set -euo pipefail
out_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${HOME}/.local/state/steamshine/hardware-tests}"
mkdir -p "${out_dir}"
report="${out_dir}/latency.log"
proc_snapshot() {
  local pid="$1" root stat status io
  root="${PROC_ROOT:-/proc}"
  stat="${root}/${pid}/stat"
  status="${root}/${pid}/status"
  io="${root}/${pid}/io"
  [[ -r "${stat}" ]] || { echo "pid=${pid} unavailable"; return 0; }
  local ticks; ticks="$(getconf CLK_TCK 2>/dev/null || echo 100)"
  awk -v pid="${pid}" -v ticks="${ticks}" 'NR == 1 {print "pid=" pid " utime=" $14 " stime=" $15 " clk_tck=" ticks}' "${stat}"
  awk '$1 == "VmRSS:" || $1 == "voluntary_ctxt_switches:" || $1 == "nonvoluntary_ctxt_switches:" {printf "%s=%s ", $1, $2} END {print ""}' "${status}" 2>/dev/null || echo 'status=unavailable'
  awk '$1 == "read_bytes:" || $1 == "write_bytes:" || $1 == "syscr:" || $1 == "syscw:" {printf "%s=%s ", $1, $2} END {print ""}' "${io}" 2>/dev/null || echo 'io=unavailable'
}
{
  echo 'Use identical game, scene, resolution, FPS, codec, bitrate, client, and network for both runs.'
  echo 'Record Moonlight statistics (RTT/decoder/render/dropped frames) during each 60-second run.'
  echo "timestamp=$(date --iso-8601=seconds)"
  pid="$(pgrep -n steamshine 2>/dev/null | head -n1 || true)"
  if command -v pidstat >/dev/null 2>&1 && [[ -n "${pid}" ]]; then
    pidstat -rud -p "${pid}" 1 60 || echo 'DIAGNOSTIC_WARN: pidstat collection failed'
  elif [[ -n "${pid}" ]]; then
    echo 'DIAGNOSTIC_WARN: pidstat unavailable; using /proc snapshot'
    proc_snapshot "${pid}"
  else
    echo 'DIAGNOSTIC_WARN: SteamShine PID unavailable'
  fi
  command -v mangohud >/dev/null 2>&1 && echo 'Capture game frametime with MangoHud logging.' || echo 'DIAGNOSTIC_WARN: MangoHud unavailable'
  command -v amd-smi >/dev/null 2>&1 && amd-smi metric -g 2>&1 || echo 'DIAGNOSTIC_WARN: optional AMD telemetry unavailable'
} | tee "${report}"
