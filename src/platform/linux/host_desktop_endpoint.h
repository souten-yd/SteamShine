/**
 * @file src/platform/linux/host_desktop_endpoint.h
 * @brief Immutable host desktop endpoint retained for local presentation.
 */
#pragma once

#include <string>

namespace host_desktop_endpoint {
  /**
   * @brief Host desktop connection values captured before private-session setup.
   */
  struct endpoint_t {
    std::string xdg_runtime_directory;  ///< Original host `XDG_RUNTIME_DIR`.
    std::string wayland_display;  ///< Original host `WAYLAND_DISPLAY`.
    std::string x11_display;  ///< Original host `DISPLAY`.
  };

  /**
   * @brief Capture the current process desktop endpoint exactly once.
   *
   * The result is intentionally independent from any child Gamescope
   * environment. Repeated calls preserve the first captured host endpoint.
   */
  void capture();

  /**
   * @brief Return the immutable host desktop endpoint.
   *
   * @return Values captured by @ref capture, or empty fields before capture.
   */
  endpoint_t current();
}  // namespace host_desktop_endpoint
