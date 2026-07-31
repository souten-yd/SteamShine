#!/usr/bin/env bash
# @file scripts/test-steamos-web-http.sh
# @brief Smoke-test the packaged upstream Web UI through a running HTTPS server.
set -Eeuo pipefail

binary="${1:?installed SteamShine binary is required}"
report_dir="${2:?report directory is required}"
port="${STEAMSHINE_WEB_SMOKE_BASE_PORT:-48989}"
web_port="$((port + 1))"
timeout_seconds="${STEAMSHINE_WEB_SMOKE_TIMEOUT_SECONDS:-30}"
work_dir="$(mktemp -d)"
log_file="${report_dir}/steamshine-web-http.log"
report_file="${report_dir}/web-http-smoke-report.json"
process_id=""
mkdir -p "${report_dir}" "${work_dir}/home/run"

cleanup() {
  local status="$?"
  if [[ -n "${process_id}" ]] && kill -0 "${process_id}" 2>/dev/null; then
    kill "${process_id}" 2>/dev/null || true
    wait "${process_id}" 2>/dev/null || true
  fi
  rm -rf -- "${work_dir}"
  exit "${status}"
}
trap cleanup EXIT INT TERM

[[ -x "${binary}" ]] || { echo "Missing installed SteamShine binary: ${binary}" >&2; exit 1; }
command -v curl >/dev/null || { echo 'curl is required for Web HTTP smoke testing.' >&2; exit 1; }

config_file="${work_dir}/home/sunshine.conf"
cat >"${config_file}" <<EOF
port = ${port}
address_family = ipv4
bind_address = 127.0.0.1
origin_web_ui_allowed = pc
system_tray = disabled
EOF

HOME="${work_dir}/home" XDG_RUNTIME_DIR="${work_dir}/home/run" \
  "${binary}" "${config_file}" >"${log_file}" 2>&1 &
process_id="$!"
base_url="https://127.0.0.1:${web_port}"
deadline="$((SECONDS + timeout_seconds))"
while ! curl --insecure --silent --show-error --output /dev/null "${base_url}/welcome/"; do
  if (( SECONDS >= deadline )); then
    echo "Timed out waiting for the HTTPS Web UI on ${base_url}." >&2
    exit 1
  fi
  if ! kill -0 "${process_id}" 2>/dev/null; then
    echo 'SteamShine exited before its HTTPS Web UI became ready.' >&2
    exit 1
  fi
  sleep 1
done

page_file="${work_dir}/welcome.html"
headers_file="${work_dir}/welcome.headers"
upstream_root_status="$(curl --insecure --silent --show-error --location --output /dev/null --write-out '%{http_code}' "${base_url}/")"
[[ "${upstream_root_status}" == 200 ]] || { echo "Expected the default upstream root to reach welcome with HTTP 200, got ${upstream_root_status}." >&2; exit 1; }
status_code="$(curl --insecure --silent --show-error --dump-header "${headers_file}" --output "${page_file}" --write-out '%{http_code}' "${base_url}/welcome/")"
[[ "${status_code}" == 200 ]] || { echo "Expected /welcome/ to return HTTP 200, got ${status_code}." >&2; exit 1; }
# EJS markers must be expanded before delivery. Vue interpolation remains in
# the generated document by design and is verified after hydration by browser
# E2E, rather than being mistaken for an unprocessed server-side template.
if grep -Fq '<%-' "${page_file}"; then
  echo 'The served welcome page contains an unresolved EJS template marker.' >&2
  exit 1
fi

asset_count=0
while IFS= read -r reference; do
  [[ -n "${reference}" ]] || continue
  case "${reference}" in
    http://*|https://*|//*|\#*) continue ;;
  esac
  asset_path="${reference#./}"
  [[ "${asset_path}" != *'..'* ]] || { echo "Unsafe asset reference in served page: ${reference}" >&2; exit 1; }
  if [[ "${reference}" == /* ]]; then
    asset_url="${base_url}${reference}"
  else
    asset_url="${base_url}/${asset_path}"
  fi
  asset_headers="${work_dir}/asset-${asset_count}.headers"
  asset_status="$(curl --insecure --silent --show-error --dump-header "${asset_headers}" --output /dev/null --write-out '%{http_code}' "${asset_url}")"
  [[ "${asset_status}" == 200 ]] || { echo "Expected asset ${reference} to return HTTP 200, got ${asset_status}." >&2; exit 1; }
  case "${asset_path}" in
    *.js) grep -Eqi '^content-type: (application|text)/javascript' "${asset_headers}" || { echo "JavaScript asset has an invalid Content-Type: ${reference}" >&2; exit 1; } ;;
    *.css) grep -Eqi '^content-type: text/css' "${asset_headers}" || { echo "CSS asset has an invalid Content-Type: ${reference}" >&2; exit 1; } ;;
  esac
  asset_count="$((asset_count + 1))"
done < <(grep -oE '(src|href)="[^"]+"' "${page_file}" | sed -E 's/^[^="]+="([^"]+)"$/\1/')

missing_status="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}/assets/steamshine-web-smoke-missing.js")"
[[ "${missing_status}" == 404 ]] || { echo "Expected a missing static asset to return HTTP 404, got ${missing_status}." >&2; exit 1; }
upstream_after_steamshine_failure="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}/welcome/")"
[[ "${upstream_after_steamshine_failure}" == 200 ]] || { echo 'A missing upstream asset prevented the upstream welcome route from loading.' >&2; exit 1; }
steamshine_page_file="${work_dir}/steamshine.html"
steamshine_status="$(curl --insecure --silent --show-error --output "${steamshine_page_file}" --write-out '%{http_code}' "${base_url}/steamshine/monitor")"
[[ "${steamshine_status}" == 200 ]] || { echo "Expected /steamshine/monitor to return HTTP 200, got ${steamshine_status}." >&2; exit 1; }
for steamshine_route in /steamshine/setup /steamshine/login /steamshine/monitor /steamshine/stream /steamshine/applications /steamshine/gpu /steamshine/config /steamshine/pairing /steamshine/clients /steamshine/terminal; do
  steamshine_route_status="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}${steamshine_route}")"
  [[ "${steamshine_route_status}" == 200 ]] || { echo "Expected SteamShine route ${steamshine_route} to return HTTP 200, got ${steamshine_route_status}." >&2; exit 1; }
done
grep -Fq '/steamshine/app.css' "${steamshine_page_file}" || { echo 'SteamShine page is missing its stylesheet reference.' >&2; exit 1; }
grep -Fq '/steamshine/app.js' "${steamshine_page_file}" || { echo 'SteamShine page is missing its JavaScript reference.' >&2; exit 1; }
for steamshine_asset in /steamshine/app.css /steamshine/app.js; do
  steamshine_headers="${work_dir}/$(basename "${steamshine_asset}").headers"
  steamshine_asset_status="$(curl --insecure --silent --show-error --dump-header "${steamshine_headers}" --output /dev/null --write-out '%{http_code}' "${base_url}${steamshine_asset}")"
  [[ "${steamshine_asset_status}" == 200 ]] || { echo "Expected SteamShine asset ${steamshine_asset} to return HTTP 200, got ${steamshine_asset_status}." >&2; exit 1; }
done
grep -Eqi '^content-type: text/css' "${work_dir}/app.css.headers" || { echo 'SteamShine stylesheet has an invalid Content-Type.' >&2; exit 1; }
grep -Eqi '^content-type: (application|text)/javascript' "${work_dir}/app.js.headers" || { echo 'SteamShine JavaScript has an invalid Content-Type.' >&2; exit 1; }
steamshine_missing_status="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}/steamshine/missing.js")"
[[ "${steamshine_missing_status}" == 404 ]] || { echo "Expected a missing SteamShine asset to return HTTP 404, got ${steamshine_missing_status}." >&2; exit 1; }
steamshine_after_upstream_failure="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}/steamshine/monitor")"
[[ "${steamshine_after_upstream_failure}" == 200 ]] || { echo 'A missing SteamShine asset prevented the SteamShine route from loading.' >&2; exit 1; }
unauthorized_api_status="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${base_url}/api/steamshine/v1/status")"
[[ "${unauthorized_api_status}" == 401 || "${unauthorized_api_status}" == 403 ]] || { echo "Expected unauthenticated SteamShine status API to return 401 or 403, got ${unauthorized_api_status}." >&2; exit 1; }
if grep -Eqi 'asset not found|template error' "${log_file}"; then
  echo 'SteamShine logged an asset or template error during Web HTTP smoke testing.' >&2
  exit 1
fi

# Verify the configuration guard never leaves both interfaces unavailable. The
# original process is stopped first because Sunshine owns additional service
# ports besides the Web listener.
kill "${process_id}" 2>/dev/null || true
wait "${process_id}" 2>/dev/null || true
process_id=""
rollback_port="$((port + 10))"
rollback_web_port="$((rollback_port + 1))"
rollback_home="${work_dir}/rollback-home"
mkdir -p "${rollback_home}/run"
rollback_config="${rollback_home}/sunshine.conf"
cat >"${rollback_config}" <<EOF
port = ${rollback_port}
address_family = ipv4
bind_address = 127.0.0.1
origin_web_ui_allowed = pc
system_tray = disabled
steamshine_web_ui_enabled = disabled
steamshine_web_ui_default = enabled
upstream_web_ui_enabled = disabled
upstream_web_ui_visible = disabled
EOF
HOME="${rollback_home}" XDG_RUNTIME_DIR="${rollback_home}/run" \
  "${binary}" "${rollback_config}" >>"${log_file}" 2>&1 &
process_id="$!"
rollback_base_url="https://127.0.0.1:${rollback_web_port}"
deadline="$((SECONDS + timeout_seconds))"
while ! curl --insecure --silent --show-error --output /dev/null "${rollback_base_url}/welcome/"; do
  if (( SECONDS >= deadline )); then
    echo "Timed out waiting for the guarded rollback Web UI on ${rollback_base_url}." >&2
    exit 1
  fi
  if ! kill -0 "${process_id}" 2>/dev/null; then
    echo 'SteamShine exited before the guarded rollback Web UI became ready.' >&2
    exit 1
  fi
  sleep 1
done
both_disabled_root_status="$(curl --insecure --silent --show-error --location --output /dev/null --write-out '%{http_code}' "${rollback_base_url}/")"
both_disabled_steamshine_status="$(curl --insecure --silent --show-error --output /dev/null --write-out '%{http_code}' "${rollback_base_url}/steamshine/")"
[[ "${both_disabled_root_status}" == 200 ]] || { echo "Both-disabled guard did not restore upstream root, got ${both_disabled_root_status}." >&2; exit 1; }
[[ "${both_disabled_steamshine_status}" == 404 ]] || { echo "Disabled SteamShine route returned ${both_disabled_steamshine_status}, expected 404." >&2; exit 1; }

python3 - "${report_file}" "${base_url}" "${asset_count}" "${unauthorized_api_status}" "${upstream_root_status}" "${both_disabled_root_status}" "${both_disabled_steamshine_status}" <<'PY'
"""Write the non-secret upstream Web HTTP smoke test report."""
import json
import os
import sys
from pathlib import Path

Path(sys.argv[1]).write_text(json.dumps({
    "tested_url": sys.argv[2],
    "commit_sha": os.environ.get("STEAMSHINE_COMMIT_SHA", "local"),
    "artifact_sha256": os.environ.get("STEAMSHINE_ARTIFACT_SHA256"),
    "welcome_status": 200,
    "upstream_default_root_status": int(sys.argv[5]),
    "both_disabled_guard_root_status": int(sys.argv[6]),
    "both_disabled_guard_steamshine_status": int(sys.argv[7]),
    "asset_count": int(sys.argv[3]),
    "missing_asset_status": 404,
    "cross_tree_failure_isolation": True,
    "unresolved_template_markers": False,
    "steamshine_monitor_status": 200,
    "steamshine_assets": ["/steamshine/app.css", "/steamshine/app.js"],
    "steamshine_missing_asset_status": 404,
    "steamshine_unauthenticated_status_api": int(sys.argv[4]),
}, indent=2) + "\n", encoding="utf-8")
PY
printf 'Upstream Web HTTP smoke test passed: %s\n' "${report_file}"
