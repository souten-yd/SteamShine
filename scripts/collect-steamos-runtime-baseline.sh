#!/usr/bin/env bash
# @file scripts/collect-steamos-runtime-baseline.sh
# @brief Collect read-only SteamOS runtime ABI and AMD graphics capability evidence.
set -Eeuo pipefail

say() { printf '%s\n' "$*"; }
version_from_os_release() { local key="$1"; awk -F= -v key="${key}" '$1 == key { gsub(/"/, "", $2); print $2 }' /etc/os-release; }

say '{'
printf '  "version_id": "%s",\n' "$(version_from_os_release VERSION_ID)"
printf '  "build_id": "%s",\n' "$(version_from_os_release BUILD_ID)"
printf '  "architecture": "%s",\n' "$(uname -m)"
printf '  "glibc": "%s",\n' "$(ldd --version | head -1 | sed 's/"/\\"/g')"
printf '  "qt6_core": "%s",\n' "$(pkg-config --modversion Qt6Core 2>/dev/null || true)"
printf '  "qt6_svg": "%s",\n' "$(pkg-config --modversion Qt6Svg 2>/dev/null || true)"
printf '  "gamescope": "%s",\n' "$(gamescope --version 2>/dev/null | head -1 | sed 's/"/\\"/g' || true)"
printf '  "packages": "%s",\n' "$(pacman -Q glibc gcc-libs qt6-base qt6-svg mesa vulkan-radeon 2>/dev/null | tr '\n' ';' | sed 's/"/\\"/g')"
printf '  "max_glibcxx": "%s",\n' "$(strings /usr/lib/libstdc++.so.6 2>/dev/null | grep '^GLIBCXX_' | sort -V | tail -1 || true)"
printf '  "drm": "%s",\n' "$(find /dev/dri/by-path -maxdepth 1 -type l -printf '%f->%l;' 2>/dev/null | sed 's/"/\\"/g')"
printf '  "vulkan_video_extensions": "%s"\n' "$(vulkaninfo --summary 2>/dev/null | grep -E 'VK_KHR_video_(queue|encode_queue|encode_h264)' | tr '\n' ';' | sed 's/"/\\"/g' || true)"
say '}'
