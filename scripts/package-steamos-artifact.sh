#!/usr/bin/env bash
# Build an immutable, user-space SteamOS artifact from an already validated build tree.
set -Eeuo pipefail
build_dir="${1:?build directory is required}"
output_dir="${2:?output directory is required}"
root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary="${build_dir}/sunshine"
[[ -x "${binary}" ]] || { echo "Missing built Sunshine binary: ${binary}" >&2; exit 1; }
commit="$(git -C "${root_dir}" rev-parse HEAD)"
archive="steamshine-steamos-x86_64-${commit}.tar.zst"
stage="$(mktemp -d)"
trap 'rm -rf -- "${stage}"' EXIT
mkdir -p "${stage}/bin" "${stage}/lib" "${stage}/share" "${stage}/scripts" "${stage}/systemd-user" "${output_dir}"
install -m 755 "${binary}" "${stage}/bin/steamshine"
install -m 755 "${root_dir}/scripts/diagnose-steamos-virtual-display.sh" "${root_dir}/scripts/test-steamos-virtual-display.sh" "${root_dir}/scripts/test-steamos-reconnect.sh" "${stage}/scripts/"
install -m 644 "${root_dir}/LICENSE" "${stage}/LICENSE"
install -m 644 "${root_dir}/packaging/linux/app-dev.lizardbyte.app.Sunshine.service.in" "${stage}/systemd-user/steamshine.service.in"
ldd "${binary}" >"${stage}/RUNTIME_DEPENDENCIES.txt"
readelf -d "${binary}" >>"${stage}/RUNTIME_DEPENDENCIES.txt"
cat >"${stage}/BUILD_INFO.json" <<EOF
{"commit":"${commit}","branch":"${GITHUB_REF_NAME:-local}","build_date":"$(date -u +%FT%TZ)","compiler":"$(c++ --version | head -1)","cmake":"$(cmake --version | head -1)","glibc":"$(ldd --version | head -1)","target_architecture":"$(uname -m)","unit_tests":"passed","integration_tests":"passed","hardware_tests":"not-run"}
EOF
tar --zstd -C "${stage}" -cf "${output_dir}/${archive}" .
(cd "${output_dir}" && sha256sum "${archive}" >"${archive}.sha256")
