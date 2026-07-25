#!/usr/bin/env bash
# @file tests/integration/test_web_asset_validation.sh
# @brief Verify generated Web UI asset package validation.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
test_root="$(mktemp -d)"
trap 'rm -rf -- "${test_root}"' EXIT
web_root="${test_root}/web"
steamshine_root="${test_root}/steamshine"
mkdir -p "${web_root}/assets/locale" "${web_root}/.vite"
mkdir -p "${steamshine_root}"

cat >"${web_root}/index.html" <<'EOF'
<!doctype html><html><head><link href="assets/app.css" rel="stylesheet"></head><body><script src="assets/app.js"></script></body></html>
EOF
printf 'console.log("web validation");\n' >"${web_root}/assets/app.js"
printf 'body { color: white; }\n' >"${web_root}/assets/app.css"
printf '{}\n' >"${web_root}/assets/locale/en.json"
printf '{"index.html":{"file":"assets/app.js","css":["assets/app.css"]}}\n' >"${web_root}/.vite/manifest.json"
printf '<!doctype html><link rel="stylesheet" href="/steamshine/app.css"><script type="module" src="/steamshine/app.js"></script>\n' >"${steamshine_root}/index.html"
printf 'body { color: white; }\n' >"${steamshine_root}/app.css"
printf 'console.log("steamshine validation");\n' >"${steamshine_root}/app.js"
printf '{"version":1,"files":{"index.html":{},"app.css":{},"app.js":{}}}\n' >"${steamshine_root}/manifest.json"

"${root_dir}/scripts/validate-web-assets.py" "${web_root}" --steamshine-root "${steamshine_root}" --report "${test_root}/web-static-report.json"
grep -Fq '"unresolved_template_markers": false' "${test_root}/web-static-report.json"

# Generated entry pages must never silently regress to source templates.
printf '<%%- header %%>\n' >>"${web_root}/index.html"
if "${root_dir}/scripts/validate-web-assets.py" "${web_root}" >/dev/null 2>&1; then
  echo 'Expected unresolved EJS template marker to fail validation.' >&2
  exit 1
fi

# SteamShine's generated manifest is a separate package gate.
rm "${steamshine_root}/manifest.json"
if "${root_dir}/scripts/validate-web-assets.py" "${web_root}" --steamshine-root "${steamshine_root}" >/dev/null 2>&1; then
  echo 'Expected missing SteamShine manifest to fail validation.' >&2
  exit 1
fi
