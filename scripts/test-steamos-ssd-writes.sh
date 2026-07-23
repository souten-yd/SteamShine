#!/usr/bin/env bash
# Measure SteamShine write counters over a streaming interval without adding stream-time I/O.
set -euo pipefail
seconds="${1:-60}"
[[ "${seconds}" =~ ^[0-9]+$ ]] || { echo 'Usage: test-steamos-ssd-writes.sh [seconds]' >&2; exit 2; }
out_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${HOME}/.local/state/steamshine/hardware-tests}"
mkdir -p "${out_dir}"
report="${out_dir}/ssd-writes.log"
pid=$(pgrep -n steamshine || true); [[ -n "${pid}" ]] || { echo 'SteamShine is not running.' >&2; exit 1; }
before=$(awk '/write_bytes/ {print $2}' "/proc/${pid}/io")
sleep "${seconds}"
after=$(awk '/write_bytes/ {print $2}' "/proc/${pid}/io")
{
  echo "SteamShine write_bytes delta over ${seconds}s: $((after - before))"
  echo 'Expected during streaming: no high-frequency writes attributable to management or telemetry.'
} | tee "${report}"
