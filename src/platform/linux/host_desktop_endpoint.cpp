/**
 * @file src/platform/linux/host_desktop_endpoint.cpp
 * @brief Immutable host desktop endpoint implementation.
 */
#include "host_desktop_endpoint.h"

#include <cstdlib>
#include <mutex>

namespace host_desktop_endpoint {
  namespace {
    std::mutex endpoint_mutex;  ///< Serializes the one-time endpoint capture.
    endpoint_t endpoint;  ///< Immutable host endpoint after capture.
    bool captured {};  ///< Whether the host endpoint was already captured.

    /**
     * @brief Read an environment variable without retaining its process-owned pointer.
     *
     * @param name Environment variable name.
     * @return Owned value, or an empty string when absent.
     */
    std::string environment_value(const char *name) {
      const auto *const value {std::getenv(name)};
      return value ? value : "";
    }
  }  // namespace

  void capture() {
    std::scoped_lock lock {endpoint_mutex};
    if (captured) {
      return;
    }
    endpoint = {
      .xdg_runtime_directory = environment_value("XDG_RUNTIME_DIR"),
      .wayland_display = environment_value("WAYLAND_DISPLAY"),
      .x11_display = environment_value("DISPLAY"),
    };
    captured = true;
  }

  endpoint_t current() {
    std::scoped_lock lock {endpoint_mutex};
    return endpoint;
  }
}  // namespace host_desktop_endpoint
