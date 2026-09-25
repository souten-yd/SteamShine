/**
 * @file src/platform/linux/gamescope_wsi_socket.h
 * @brief Identify Gamescope socket aliases inside Steam's pressure-vessel runtime.
 */
#pragma once

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace gamescope_wsi {
  /**
   * @brief Preserve Gamescope WSI detection while accepting aliases of the same UNIX socket.
   *
   * Mirrors the upstream Gamescope socket-identity fix. A different live socket
   * remains a nested compositor and must not receive Gamescope's HDR formats.
   * An absent Wayland alias is accepted only after verifying the Gamescope socket.
   *
   * @param gamescope_display Gamescope socket name or absolute path, possibly null.
   * @param wayland_display Native Wayland socket name or absolute path, possibly null.
   * @param runtime_directory Base directory for relative socket names, possibly null.
   * @param inherited_socket Whether WAYLAND_SOCKET specifies an inherited connection.
   * @return True when the existing detection rules or socket identity establish Gamescope.
   */
  inline bool is_gamescope_session(const char *gamescope_display, const char *wayland_display, const char *runtime_directory, const bool inherited_socket) {
    if (!gamescope_display || !*gamescope_display) {
      return false;
    }
    if (!wayland_display || !*wayland_display || std::strcmp(gamescope_display, wayland_display) == 0) {
      return true;
    }
    if (inherited_socket) {
      return false;
    }
    std::array<std::string, 2> paths {gamescope_display, wayland_display};
    for (auto &path : paths) {
      if (path.front() != '/') {
        if (!runtime_directory || !*runtime_directory) {
          return false;
        }
        path = std::string {runtime_directory} + "/" + path;
      }
    }
    struct stat gamescope_stat {}, wayland_stat {};
    if (stat(paths[0].c_str(), &gamescope_stat) != 0 || !S_ISSOCK(gamescope_stat.st_mode)) {
      return false;
    }
    if (stat(paths[1].c_str(), &wayland_stat) != 0) {
      return errno == ENOENT || errno == ENOTDIR;
    }
    return S_ISSOCK(wayland_stat.st_mode) && gamescope_stat.st_dev == wayland_stat.st_dev && gamescope_stat.st_ino == wayland_stat.st_ino;
  }
}  // namespace gamescope_wsi
