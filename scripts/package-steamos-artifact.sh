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
miniupnpc_library="$(ldconfig -p | awk '/libminiupnpc\.so\.21/ { print $NF; exit }')"
[[ -n "${miniupnpc_library}" && -f "${miniupnpc_library}" ]] || { echo "Missing libminiupnpc.so.21 required by SteamShine" >&2; exit 1; }
install -m 644 "${miniupnpc_library}" "${stage}/lib/"
patchelf --set-rpath "\$ORIGIN/../lib" "${stage}/bin/steamshine"
install -m 755 \
  "${root_dir}/scripts/collect-steamos-runtime-baseline.sh" \
  "${root_dir}/scripts/diagnose-steamos-virtual-display.sh" \
  "${root_dir}/scripts/test-steamos-virtual-display.sh" \
  "${root_dir}/scripts/test-steamos-reconnect.sh" \
  "${root_dir}/scripts/test-steamos-latency.sh" \
  "${root_dir}/scripts/test-steamos-ssd-writes.sh" \
  "${stage}/scripts/"
install -m 644 "${root_dir}/LICENSE" "${stage}/LICENSE"
install -m 644 "${root_dir}/ci/steamos/baselines/steamos-3.8.16-20260716.1.json" "${stage}/STEAMOS_BASELINE.json"
if [[ -f /usr/share/licenses/miniupnpc/LICENSE ]]; then
  mkdir -p "${stage}/share/licenses/miniupnpc"
  install -m 644 /usr/share/licenses/miniupnpc/LICENSE "${stage}/share/licenses/miniupnpc/LICENSE"
fi
install -m 644 "${root_dir}/packaging/linux/app-dev.lizardbyte.app.Sunshine.service.in" "${stage}/systemd-user/steamshine.service.in"
ldd "${stage}/bin/steamshine" >"${stage}/RUNTIME_DEPENDENCIES.txt"
readelf -d "${stage}/bin/steamshine" >>"${stage}/RUNTIME_DEPENDENCIES.txt"
cat >"${stage}/BUILD_INFO.json" <<EOF
{"commit":"${commit}","branch":"${GITHUB_REF_NAME:-local}","build_date":"$(date -u +%FT%TZ)","compiler":"$(c++ --version | head -1)","cmake":"$(cmake --version | head -1)","glibc":"$(ldd --version | head -1)","target_architecture":"$(uname -m)","target_steamos_version":"3.8.16","target_steamos_build_id":"20260716.1","max_glibc":"2.41","max_glibcxx":"3.4.34","max_qt":"6.9","tested_gamescope":"3.16.23.4","target_gpu":"AMD Radeon RX 9070 XT","target_gpu_family":"RDNA4/GFX1201","target_pci_bdf":"0000:03:00.0","target_render_node":"/dev/dri/renderD128","capture_backend":"wayland-dmabuf","encoder_backend":"vulkan-video","unit_tests":"passed","integration_tests":"passed","hardware_tested":false}
EOF
tar --zstd -C "${stage}" -cf "${output_dir}/${archive}" .
(cd "${output_dir}" && sha256sum "${archive}" >"${archive}.sha256")
