#!/usr/bin/env bash
# Collect comparable Sunshine/SteamShine latency evidence; it does not fabricate metrics unavailable from the host.
set -euo pipefail
out_dir="${HOME}/.local/state/steamshine/hardware-tests"
mkdir -p "${out_dir}"
report="${out_dir}/latency-$(date +%Y%m%d-%H%M%S).log"
{
  echo 'Use identical game, scene, resolution, FPS, codec, bitrate, client, and network for both runs.'
  echo 'Record Moonlight statistics (RTT/decoder/render/dropped frames) during each 60-second run.'
  echo 'Host counters:'; date --iso-8601=seconds
  pid=$(pgrep -n steamshine || true); [[ -n "${pid}" ]] && pidstat -rud -p "${pid}" 1 60 || true
  command -v mangohud >/dev/null && echo 'Capture game frametime with MangoHud logging.' || true
  command -v amd-smi >/dev/null && amd-smi metric -g 2>&1 || true
} | tee "${report}"
echo "Acceptance: encode latency delta <= +0.3ms or +3%, no statistically significant frametime regression, dropped-frame delta <= 0.1 points, and zero additional encoder sessions. Report: ${report}"
