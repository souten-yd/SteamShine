#!/usr/bin/env bash
# @file scripts/collect-steamos-runtime-baseline.sh
# @brief Collect read-only SteamOS runtime ABI and AMD graphics capability evidence.
set -Eeuo pipefail

say() { printf '%s\n' "$*"; }
version_from_os_release() { local key="$1"; awk -F= -v key="${key}" '$1 == key { gsub(/"/, "", $2); print $2 }' /etc/os-release; }
json_escape() { sed 's/\\/\\\\/g; s/"/\\"/g'; }
qt_version() { local package="$1"; pacman -Q "${package}" 2>/dev/null | awk 'NR == 1 {print $2}'; }
gamescope_version() { gamescope --version 2>&1 | grep -oE '[0-9]+(\.[0-9]+){2,3}' | head -1; }
vulkan_video_extensions() {
  command -v vulkaninfo >/dev/null 2>&1 || return 0
  vulkaninfo 2>/dev/null | grep -oE 'VK_(KHR|EXT|VALVE)_video_[A-Za-z0-9_]+' | sort -u | paste -sd ';' -
}

say '{'
printf '  "version_id": "%s",\n' "$(version_from_os_release VERSION_ID)"
printf '  "build_id": "%s",\n' "$(version_from_os_release BUILD_ID)"
printf '  "architecture": "%s",\n' "$(uname -m)"
printf '  "glibc": "%s",\n' "$(ldd --version | head -1 | sed 's/"/\\"/g')"
printf '  "qt6_core": "%s",\n' "$(qt_version qt6-base | json_escape)"
printf '  "qt6_svg": "%s",\n' "$(qt_version qt6-svg | json_escape)"
printf '  "gamescope": "%s",\n' "$(gamescope_version | json_escape)"
printf '  "packages": "%s",\n' "$(pacman -Q glibc gcc-libs qt6-base qt6-svg mesa vulkan-radeon 2>/dev/null | tr '\n' ';' | sed 's/"/\\"/g')"
printf '  "max_glibcxx": "%s",\n' "$(strings /usr/lib/libstdc++.so.6 2>/dev/null | grep -E '^GLIBCXX_[0-9]+(\.[0-9]+)+$' | sort -V | tail -1 || true)"
printf '  "drm": "%s",\n' "$(find /dev/dri/by-path -maxdepth 1 -type l -printf '%f->%l;' 2>/dev/null | sed 's/"/\\"/g')"
printf '  "vulkan_video_extensions": "%s"\n' "$(vulkan_video_extensions | json_escape)"
say '}'
