/**
 * @file src/steamos_virtual_session_core.cpp
 * @brief Pure SteamOS virtual-display request and Gamescope command helpers.
 */
#include "steamos_virtual_session_core.h"

#include <algorithm>
#include <string_view>

namespace steamos_virtual_session {
  namespace {
    /**
     * @brief Use a fallback for a missing request and clamp it to a safe range.
     *
     * @param value Requested value.
     * @param fallback Value used when @p value is not positive.
     * @param minimum Inclusive lower bound.
     * @param maximum Inclusive upper bound.
     * @return The normalized value.
     */
    int normalize_value(const int value, const int fallback, const int minimum, const int maximum) {
      return std::clamp(value > 0 ? value : fallback, minimum, maximum);
    }
  }  // namespace

  display_request_t normalize_display_request(const int requested_width, const int requested_height, const int requested_fps, const int default_width, const int default_height, const int default_fps) {
    return {
      normalize_value(requested_width, default_width, 640, 7680),
      normalize_value(requested_height, default_height, 480, 4320),
      normalize_value(requested_fps, default_fps, 30, 240),
    };
  }

  std::vector<std::string> gamescope_arguments(const std::string &help_text, const int width, const int height, const int fps, const bool enable_hdr, const std::string &gpu_device, std::string &error) {
    const auto has_option {[&help_text](const std::string_view option) {
      return help_text.find(option) != std::string::npos;
    }};
    if (!has_option("--nested-width") || !has_option("--nested-height") || !has_option("--nested-refresh") || !has_option("--expose-wayland")) {
      error = "Installed Gamescope does not advertise nested Wayland display options";
      return {};
    }
    std::vector<std::string> arguments;
    if (has_option("--backend") && help_text.find("headless") != std::string::npos) {
      arguments.emplace_back("--backend");
      arguments.emplace_back("headless");
    } else if (has_option("--headless")) {
      arguments.emplace_back("--headless");
    } else {
      error = "Installed Gamescope does not advertise a headless backend";
      return {};
    }
    arguments.emplace_back("--nested-width");
    arguments.emplace_back(std::to_string(width));
    arguments.emplace_back("--nested-height");
    arguments.emplace_back(std::to_string(height));
    arguments.emplace_back("--nested-refresh");
    arguments.emplace_back(std::to_string(fps));
    arguments.emplace_back("--expose-wayland");
    if (has_option("--scaler")) {
      arguments.emplace_back("--scaler");
      arguments.emplace_back("fit");
    }
    if (enable_hdr) {
      if (!has_option("--hdr-enabled")) {
        error = "Client requested HDR but installed Gamescope does not advertise HDR output";
        return {};
      }
      arguments.emplace_back("--hdr-enabled");
    }
    if (!gpu_device.empty()) {
      if (!has_option("--prefer-vk-device")) {
        error = "Installed Gamescope does not advertise AMD Vulkan device selection";
        return {};
      }
      arguments.emplace_back("--prefer-vk-device");
      arguments.emplace_back(gpu_device);
    }
    return arguments;
  }
}  // namespace steamos_virtual_session
