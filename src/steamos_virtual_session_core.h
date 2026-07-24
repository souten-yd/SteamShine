/**
 * @file src/steamos_virtual_session_core.h
 * @brief Pure SteamOS virtual-display request and Gamescope command helpers.
 */
#pragma once

#include <string>
#include <vector>

namespace steamos_virtual_session {
  /**
   * @brief A validated nested Gamescope display request.
   */
  struct display_request_t {
    int width;  ///< Nested display width in pixels.
    int height;  ///< Nested display height in pixels.
    int fps;  ///< Nested display refresh rate.
  };

  /**
   * @brief Clamp a client display request to SteamOS virtual-session bounds.
   *
   * @param requested_width Client-provided width, or zero when unavailable.
   * @param requested_height Client-provided height, or zero when unavailable.
   * @param requested_fps Client-provided frame rate, or zero when unavailable.
   * @param default_width Configured width used for a missing request.
   * @param default_height Configured height used for a missing request.
   * @param default_fps Configured frame rate used for a missing request.
   * @return A request constrained to 640x480 through 7680x4320 at 30 through 240 FPS.
   */
  display_request_t normalize_display_request(int requested_width, int requested_height, int requested_fps, int default_width, int default_height, int default_fps);

  /**
   * @brief Build a Gamescope command from options advertised by its own help text.
   *
   * The result never uses an option absent from @p help_text. This keeps the
   * virtual-display provider compatible with the Gamescope version installed on
   * the SteamOS host.
   *
   * @param help_text Output captured from `gamescope --help`.
   * @param width Normalized nested display width.
   * @param height Normalized nested display height.
   * @param fps Normalized nested display refresh rate.
   * @param enable_hdr Whether the client requested HDR output.
   * @param gpu_device PCI vendor/device identifier accepted by Gamescope, if selected.
   * @param error Receives a reason when the advertised option set is insufficient.
   * @return Arguments after the executable, or an empty vector on failure.
   */
  std::vector<std::string> gamescope_arguments(const std::string &help_text, int width, int height, int fps, bool enable_hdr, const std::string &gpu_device, std::string &error);
}  // namespace steamos_virtual_session
