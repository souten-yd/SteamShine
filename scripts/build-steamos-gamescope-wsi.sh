#!/usr/bin/env bash
# Build the SteamOS Gamescope WSI socket-identity backport inside the pinned CI image.
# Usage: build-steamos-gamescope-wsi.sh cmake-build-DIR [gamescope-source-directory]
set -Eeuo pipefail
root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="${1:?build directory is required}"
[[ "$(basename -- "${build_dir}")" == cmake-build-* ]] || { echo 'Build directory must start with cmake-build-' >&2; exit 1; }
mkdir -p "${build_dir}"
build_dir="$(cd -- "${build_dir}" && pwd -P)"
source_dir="${2:-${build_dir}/source}"

## @brief Fetch a commit-pinned upstream source archive into an empty cache directory.
## @param repo GitHub owner/repository.
## @param revision Full immutable commit ID.
## @param destination Cache directory to populate.
fetch_source() {
  local repo="$1" revision="$2" destination="$3" archive
  [[ -f "${destination}/.steamshine-source-revision" ]] && [[ "$(cat "${destination}/.steamshine-source-revision")" == "${revision}" ]] && return
  [[ ! -e "${destination}" ]] || { echo "Refusing to replace source directory: ${destination}" >&2; return 1; }
  archive="$(mktemp "${build_dir}/source.XXXXXX.tar.gz")"
  curl --fail --location --retry 3 "https://codeload.github.com/${repo}/tar.gz/${revision}" -o "${archive}"
  mkdir -p "${destination}"
  tar -xzf "${archive}" --strip-components=1 -C "${destination}"
  rm -- "${archive}"
  printf '%s\n' "${revision}" >"${destination}/.steamshine-source-revision"
}

if [[ $# -lt 2 ]]; then
  fetch_source ValveSoftware/gamescope 2b79e07b3da1723c7e5c5f44f18de36c6cb78b9e "${source_dir}"
  # Submodule archives contain empty directories; their pinned dependencies are separate.
  rmdir "${source_dir}/subprojects/vkroots" 2>/dev/null || true
  fetch_source Joshua-Ashton/vkroots 5106d8a0df95de66cc58dc1ea37e69c99afc9540 "${source_dir}/subprojects/vkroots"
  fetch_source g-truc/glm 0af55ccecd98d4e5a8d1fad7de25ba429d60e863 "${source_dir}/subprojects/glm"
fi
source_dir="$(cd -- "${source_dir}" && pwd -P)"
printf '%s  %s\n' 00d3e34c9829da5ee12edec97c3a136e390dbf6f86608c51b32164d622f2a52c "${source_dir}/layer/VkLayer_FROG_gamescope_wsi.cpp" | sha256sum --check --status

python3 - "${source_dir}" "${build_dir}" "${root_dir}" <<'PY'
"""Stage the pinned layer with the tested socket-identity helper and relocatable manifest."""
import json
import shutil
import sys
from pathlib import Path

source, build, root = map(Path, sys.argv[1:])
for directory, names in {
    'layer': ['VkLayer_FROG_gamescope_wsi.cpp', 'xcb_helpers.hpp', 'vulkan_operators.hpp'],
    'src': ['color_helpers.h', 'layer_defines.h', 'messagey.h'],
    'protocol': ['gamescope-swapchain.xml'],
}.items():
    (build / directory).mkdir(exist_ok=True)
    for name in names:
        shutil.copy2(source / directory / name, build / directory / name)
shutil.copy2(root / 'src/platform/linux/gamescope_wsi_socket.h', build / 'layer/steamshine_wsi_socket.h')
layer = build / 'layer/VkLayer_FROG_gamescope_wsi.cpp'
text = layer.read_text()
start = text.index('  static bool isRunningUnderGamescope() {')
end = text.index('  template <typename T>', start)
text = text[:start] + '''  /** @brief Apply the tested pressure-vessel socket-identity backport. */
  static bool isRunningUnderGamescope() {
    static const bool result = gamescope_wsi::is_gamescope_session(
      gamescopeWaylandSocket(), std::getenv("WAYLAND_DISPLAY"),
      std::getenv("XDG_RUNTIME_DIR"), std::getenv("WAYLAND_SOCKET") != nullptr);
    return result;
  }

''' + text[end:]
layer.write_text('#include "steamshine_wsi_socket.h"\n' + text)
install = build / 'install'
manifests = install / 'share/assets/vulkan/implicit_layer.d'
manifests.mkdir(parents=True, exist_ok=True)
(install / 'lib').mkdir(exist_ok=True)
template = (source / 'layer/VkLayer_FROG_gamescope_wsi.json.in').read_text()
manifest = json.loads(template.replace('@family@', 'x86_64').replace('@lib_dir@', '../../../../lib'))
(manifests / 'VkLayer_FROG_gamescope_wsi.x86_64.json').write_text(json.dumps(manifest, indent=2) + '\n')
licenses = install / 'share/licenses/gamescope-wsi'
licenses.mkdir(parents=True, exist_ok=True)
for src, name in [(source / 'LICENSE', 'gamescope-LICENSE'), (source / 'subprojects/vkroots/LICENSE', 'vkroots-LICENSE'), (source / 'subprojects/glm/copying.txt', 'glm-LICENSE')]:
    shutil.copy2(src, licenses / name)
PY

cd -- "${build_dir}"
wayland-scanner client-header protocol/gamescope-swapchain.xml protocol/gamescope-swapchain-client-protocol.h
wayland-scanner private-code protocol/gamescope-swapchain.xml protocol/gamescope-swapchain-protocol.c
cc -fPIC -c protocol/gamescope-swapchain-protocol.c -o protocol/gamescope-swapchain-protocol.o
c++ -std=c++20 -O2 -fPIC -fno-exceptions -shared -Wl,--no-undefined \
  -I"${source_dir}/subprojects/vkroots" -I"${source_dir}/subprojects/glm" \
  -I"${root_dir}/third-party/build-deps/third-party/FFmpeg/Vulkan-Headers/include" -Iprotocol \
  layer/VkLayer_FROG_gamescope_wsi.cpp protocol/gamescope-swapchain-protocol.o \
  -lxcb -lX11 -lX11-xcb -lwayland-client -o install/lib/libVkLayer_FROG_gamescope_wsi_x86_64.so
sha256sum layer/VkLayer_FROG_gamescope_wsi.cpp layer/steamshine_wsi_socket.h \
  "${source_dir}/subprojects/vkroots/vkroots.h" \
  install/lib/libVkLayer_FROG_gamescope_wsi_x86_64.so >install/share/licenses/gamescope-wsi/BUILD_INPUTS.sha256
