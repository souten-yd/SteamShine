#!/usr/bin/env bash
# @file scripts/test-steamos-web-hardware.sh
# @brief Collect truthful SteamOS Web UI hardware-acceptance evidence.
set -Eeuo pipefail

report_dir="${STEAMSHINE_HARDWARE_REPORT_DIR:-${HOME}/.local/state/steamshine/hardware-tests}"
port="${STEAMSHINE_WEB_PORT:-47990}"
base_url="https://localhost:${port}"
report_file="${report_dir}/web-hardware-report.json"

command -v curl >/dev/null || { echo 'curl is required for Web hardware acceptance.' >&2; exit 1; }
mkdir -p "${report_dir}"

probe_route() {
  local route="$1"
  curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}${route}"
}

upstream_status="$(probe_route /)"
steamshine_status="$(probe_route /steamshine/)"

python3 - "${report_file}" "${base_url}" "${upstream_status}" "${steamshine_status}" <<'PY'
"""Write the machine-readable preflight section of the Web hardware report."""
import json
import socket
import sys
from datetime import datetime, timezone
from pathlib import Path

Path(sys.argv[1]).write_text(json.dumps({
    "timestamp": datetime.now(timezone.utc).isoformat(),
    "host": socket.gethostname(),
    "localhost_url": sys.argv[2],
    "upstream_status": int(sys.argv[3]),
    "steamshine_status": int(sys.argv[4]),
    "hardware_execution_required": True,
    "required_manual_evidence": [
        "LAN browser access to both routes",
        "shared credential login and logout",
        "Moonlight PIN submission without recording the PIN",
        "client list synchronization and revoke/re-pair",
        "60-second stream with both dashboards open",
        "Moonlight video, audio, input, frame pacing, and dropped-frame evidence",
        "latency and SSD-write comparison with dashboards closed and open",
    ],
}, indent=2) + "\n", encoding="utf-8")
PY

printf 'Wrote hardware Web preflight report: %s\n' "${report_file}"
printf '%s\n' 'Run this on the SteamOS RX 9070 XT host, then attach browser logs, Moonlight evidence, and the latency/SSD reports before marking hardware acceptance complete.'
