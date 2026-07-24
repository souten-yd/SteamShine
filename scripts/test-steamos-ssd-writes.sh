#!/usr/bin/env bash
# Measure SteamShine process write counters without requiring optional tools.
set -euo pipefail
seconds="${1:-60}"
[[ "${seconds}" =~ ^[0-9]+$ ]] || { echo 'Usage: test-steamos-ssd-writes.sh [seconds]' >&2; exit 2; }
out_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${HOME}/.local/state/steamshine/hardware-tests}"
mkdir -p "${out_dir}"
report="${out_dir}/ssd-writes.log"

steamshine_pids() { pgrep -x steamshine 2>/dev/null || true; }
sum_proc_field() {
  local field="$1" pid file
  shift
  for pid in "$@"; do
    file="${PROC_ROOT:-/proc}/${pid}/io"
    [[ -r "${file}" ]] && awk -v key="${field}:" '$1 == key {sum += $2} END {printf "%.0f\n", sum + 0}' "${file}"
  done | awk '{sum += $1} END {printf "%.0f\n", sum + 0}'
}
report_size() { du -sb "${out_dir}" 2>/dev/null | awk 'NR == 1 {print $1}' || printf '0\n'; }
read_counters() {
  local -a pids=(); mapfile -t pids < <(steamshine_pids)
  printf 'pids=%s write_bytes=%s cancelled_write_bytes=%s syscw=%s report_bytes=%s\n' \
    "$(IFS=,; echo "${pids[*]:-}")" "$(sum_proc_field write_bytes "${pids[@]}")" \
    "$(sum_proc_field cancelled_write_bytes "${pids[@]}")" "$(sum_proc_field syscw "${pids[@]}")" "$(report_size)"
}
before="$(read_counters)"
sleep "${seconds}"
after="$(read_counters)"
{
  echo "before ${before}"
  echo "after ${after}"
  echo 'Expected during streaming: no high-frequency writes attributable to management or telemetry.'
} | tee "${report}"
