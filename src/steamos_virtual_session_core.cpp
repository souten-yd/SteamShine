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

  std::string_view to_string(const session_origin_e origin) {
    switch (origin) {
      case session_origin_e::none:
        return "none";
      case session_origin_e::owned_private:
        return "owned_private";
      case session_origin_e::attached_existing:
        return "attached_existing";
    }
    return "none";
  }

  std::optional<session_source_policy_e> parse_session_source_policy(const std::string_view value) {
    if (value == "auto") {
      return session_source_policy_e::auto_select;
    }
    if (value == "existing_gamescope") {
      return session_source_policy_e::existing_gamescope;
    }
    if (value == "owned_private") {
      return session_source_policy_e::owned_private;
    }
    return std::nullopt;
  }

  std::string_view to_string(const session_source_policy_e policy) {
    switch (policy) {
      case session_source_policy_e::auto_select:
        return "auto";
      case session_source_policy_e::existing_gamescope:
        return "existing_gamescope";
      case session_source_policy_e::owned_private:
        return "owned_private";
    }
    return "auto";
  }

  std::optional<local_presentation_policy_e> parse_local_presentation_policy(const std::string_view value) {
    if (value == "auto") {
      return local_presentation_policy_e::auto_select;
    }
    if (value == "off") {
      return local_presentation_policy_e::off;
    }
    if (value == "mirror") {
      return local_presentation_policy_e::mirror;
    }
    return std::nullopt;
  }

  std::string_view to_string(const local_presentation_policy_e policy) {
    switch (policy) {
      case local_presentation_policy_e::auto_select:
        return "auto";
      case local_presentation_policy_e::off:
        return "off";
      case local_presentation_policy_e::mirror:
        return "mirror";
    }
    return "auto";
  }

  presentation_e decide_presentation(const local_presentation_policy_e policy, const session_origin_e origin, const bool host_wayland_available, const bool physical_output_connected) {
    if (policy == local_presentation_policy_e::off || origin != session_origin_e::owned_private || !host_wayland_available || !physical_output_connected) {
      return presentation_e::remote_only;
    }
    return presentation_e::remote_and_local;
  }

  std::string_view to_string(const presentation_e presentation) {
    switch (presentation) {
      case presentation_e::remote_only:
        return "remote_only";
      case presentation_e::remote_and_local:
        return "remote_and_local";
    }
    return "remote_only";
  }

  std::optional<virtual_display_mode_e> parse_virtual_display_mode(const std::string_view value) {
    if (value == "off") {
      return virtual_display_mode_e::off;
    }
    if (value == "auto") {
      return virtual_display_mode_e::auto_detect;
    }
    if (value == "force") {
      return virtual_display_mode_e::force;
    }
    return std::nullopt;
  }

  std::string_view to_string(const virtual_display_mode_e mode) {
    switch (mode) {
      case virtual_display_mode_e::off:
        return "off";
      case virtual_display_mode_e::auto_detect:
        return "auto";
      case virtual_display_mode_e::force:
        return "force";
    }
    return "auto";
  }

  virtual_display_decision_t decide_virtual_display(const virtual_display_decision_input_t &input) {
    if (!input.feature_enabled) {
      return {false, "feature_disabled"};
    }
    if (input.mode == virtual_display_mode_e::off) {
      return {false, "mode_off"};
    }
    if (input.mode == virtual_display_mode_e::force) {
      return {true, input.host_supported ? "config_force" : "config_force_host_unsupported"};
    }
    if (input.existing_owned_session) {
      return {true, "owned_session_active"};
    }
    if (input.capturable_output_present) {
      return {false, "capturable_output_present"};
    }
    return {true, input.host_supported ? "no_capturable_output" : "no_capturable_output_host_unsupported"};
  }

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
