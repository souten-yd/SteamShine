/**
 * @file src/steamos_virtual_session_core.cpp
 * @brief Pure SteamOS virtual-display request and Gamescope command helpers.
 */
#include "steamos_virtual_session_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <string_view>

namespace steamos_virtual_session {
  namespace {
    /**
     * @brief Reduce an integer or hundredths-of-Hz refresh request.
     *
     * @param integer_fps Integer refresh supplied by the launch request.
     * @param refresh_x100 Optional refresh in hundredths of Hz.
     * @return Reduced positive refresh, or zero when neither input is valid.
     */
    display_refresh_t make_refresh(const int integer_fps, const int refresh_x100) {
      std::uint32_t numerator {0};
      std::uint32_t denominator {1};
      if (refresh_x100 > 0) {
        numerator = static_cast<std::uint32_t>(refresh_x100);
        denominator = 100;
      } else if (integer_fps > 0) {
        numerator = static_cast<std::uint32_t>(integer_fps);
      }
      if (numerator > 0) {
        const auto divisor {std::gcd(numerator, denominator)};
        numerator /= divisor;
        denominator /= divisor;
      }
      return {numerator, denominator};
    }

    /**
     * @brief Round a positive dimension upward to its required alignment.
     *
     * @param value Dimension to align.
     * @param alignment Required positive alignment.
     * @return Aligned value, or no value when integer overflow would occur.
     */
    std::optional<int> align_dimension(const int value, const std::uint32_t alignment) {
      if (value <= 0 || alignment == 0 || alignment > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
      }
      const auto wide_value {static_cast<std::uint64_t>(value)};
      const auto wide_alignment {static_cast<std::uint64_t>(alignment)};
      const auto remainder {wide_value % wide_alignment};
      const auto adjustment {remainder == 0 ? 0 : wide_alignment - remainder};
      if (wide_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) - adjustment) {
        return std::nullopt;
      }
      return static_cast<int>(wide_value + adjustment);
    }

    /**
     * @brief Multiply unsigned values while rejecting overflow.
     *
     * @param left Left operand.
     * @param right Right operand.
     * @param result Receives the product on success.
     * @return True when the product is representable.
     */
    bool checked_multiply(const std::uint64_t left, const std::uint64_t right, std::uint64_t &result) {
      if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
      }
      result = left * right;
      return true;
    }

    /**
     * @brief Compare two reduced refresh values.
     *
     * @param left First refresh.
     * @param right Second refresh.
     * @return True when both exact rational values are equal.
     */
    bool refresh_equal(const display_refresh_t &left, const display_refresh_t &right) {
      return left.numerator == right.numerator && left.denominator == right.denominator;
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

  std::optional<geometry_alignment_policy_e> parse_geometry_alignment_policy(const std::string_view value) {
    if (value == "auto") {
      return geometry_alignment_policy_e::auto_align;
    }
    if (value == "require_exact") {
      return geometry_alignment_policy_e::require_exact;
    }
    return std::nullopt;
  }

  std::string_view to_string(const geometry_alignment_policy_e policy) {
    switch (policy) {
      case geometry_alignment_policy_e::auto_align:
        return "auto";
      case geometry_alignment_policy_e::require_exact:
        return "require_exact";
    }
    return "auto";
  }

  std::optional<margin_input_policy_e> parse_margin_input_policy(const std::string_view value) {
    if (value == "clamp") {
      return margin_input_policy_e::clamp;
    }
    if (value == "reject") {
      return margin_input_policy_e::reject;
    }
    return std::nullopt;
  }

  std::string_view to_string(const margin_input_policy_e policy) {
    switch (policy) {
      case margin_input_policy_e::clamp:
        return "clamp";
      case margin_input_policy_e::reject:
        return "reject";
    }
    return "clamp";
  }

  std::string_view to_string(const session_route_e route) {
    switch (route) {
      case session_route_e::physical_desktop:
        return "physical_desktop";
      case session_route_e::attached_existing:
        return "attached_existing";
      case session_route_e::retained_owned_private:
        return "retained_owned_private";
      case session_route_e::new_owned_private:
        return "new_owned_private";
      case session_route_e::reject:
        return "reject";
    }
    return "reject";
  }

  session_route_decision_t select_session_route(const session_route_input_t &input) {
    if (!input.feature_enabled) {
      return {session_route_e::physical_desktop, "feature_disabled"};
    }
    if (input.mode == virtual_display_mode_e::off) {
      return {session_route_e::physical_desktop, "mode_off"};
    }
    if (input.mode == virtual_display_mode_e::force) {
      if (input.retained_owned_session) {
        return {session_route_e::retained_owned_private, "retained_owned_private"};
      }
      return {
        input.host_supported ? session_route_e::new_owned_private : session_route_e::reject,
        input.host_supported ? "config_force" : "config_force_host_unsupported"
      };
    }

    if (input.source_policy == session_source_policy_e::owned_private) {
      if (input.retained_owned_session) {
        return {session_route_e::retained_owned_private, "retained_owned_private"};
      }
      return {
        input.host_supported ? session_route_e::new_owned_private : session_route_e::reject,
        input.host_supported ? "owned_private_required" : "owned_private_host_unsupported"
      };
    }

    if (input.verified_existing_gamescope_present) {
      return {session_route_e::attached_existing, "verified_existing_gamescope"};
    }
    if (input.source_policy == session_source_policy_e::existing_gamescope) {
      return {session_route_e::reject, "existing_gamescope_unavailable"};
    }
    if (input.capturable_output_present) {
      return {session_route_e::physical_desktop, "capturable_output_present"};
    }
    if (input.retained_owned_session) {
      return {session_route_e::retained_owned_private, "retained_owned_private"};
    }
    return {
      input.host_supported ? session_route_e::new_owned_private : session_route_e::reject,
      input.host_supported ? "no_capturable_output" : "no_capturable_output_host_unsupported"
    };
  }

  bool route_uses_gamescope_capture(const session_route_e route) {
    return route != session_route_e::physical_desktop;
  }

  bool physical_desktop_capturable(
    const bool physical_output_connected,
    const bool active_crtc_present,
    const bool compositor_capture_available
  ) {
    return physical_output_connected && (active_crtc_present || compositor_capture_available);
  }

  bool use_virtual_capture_backend(
    const bool virtual_display_required,
    const bool physical_output_connected,
    const bool compositor_capture_available,
    const bool wlr_capture_forced
  ) {
    return virtual_display_required &&
           (wlr_capture_forced || !physical_output_connected || !compositor_capture_available);
  }

  bool should_probe_physical_portal(
    const bool automatic_capture,
    const bool portal_explicitly_requested,
    const bool physical_output_connected,
    const bool higher_priority_available
  ) {
    return portal_explicitly_requested ||
           (automatic_capture && physical_output_connected && !higher_priority_available);
  }

  std::optional<std::filesystem::path> gamescope_eis_socket_path(
    const std::filesystem::path &runtime_directory,
    const std::string_view gamescope_wayland_display
  ) {
    if (runtime_directory.empty() || !runtime_directory.is_absolute() ||
        gamescope_wayland_display.empty() || gamescope_wayland_display == "." || gamescope_wayland_display == ".." ||
        gamescope_wayland_display.find('/') != std::string_view::npos || gamescope_wayland_display.find('\0') != std::string_view::npos) {
      return std::nullopt;
    }
    return runtime_directory.lexically_normal() / (std::string {gamescope_wayland_display} + "-ei");
  }

  display_request_t select_display_request(
    const int requested_width,
    const int requested_height,
    const int requested_fps,
    const int requested_refresh_x100,
    const int default_width,
    const int default_height,
    const int default_fps,
    const geometry_alignment_policy_e alignment_policy,
    const display_constraints_t &constraints
  ) {
    display_request_t result;
    const bool request_missing {requested_width == 0 && requested_height == 0 && requested_fps == 0 && requested_refresh_x100 == 0};
    result.requested_width = requested_width;
    result.requested_height = requested_height;
    result.requested_refresh = make_refresh(requested_fps, requested_refresh_x100);
    if ((requested_width <= 0 || requested_height <= 0 || (requested_fps <= 0 && requested_refresh_x100 <= 0)) && !request_missing) {
      result.reason = "invalid_nonpositive_geometry";
      return result;
    }

    if (request_missing) {
      result.requested_width = default_width;
      result.requested_height = default_height;
      result.requested_refresh = make_refresh(default_fps, 0);
    }
    if (result.requested_refresh.numerator == 0 || result.requested_refresh.denominator == 0) {
      result.reason = "invalid_refresh";
      return result;
    }
    if (result.requested_width < constraints.minimum_width || result.requested_width > constraints.maximum_width ||
        result.requested_height < constraints.minimum_height || result.requested_height > constraints.maximum_height) {
      result.reason = "geometry_out_of_bounds";
      return result;
    }

    const long double refresh_hz {static_cast<long double>(result.requested_refresh.numerator) / result.requested_refresh.denominator};
    if (refresh_hz < constraints.minimum_fps || refresh_hz > constraints.maximum_fps) {
      result.reason = "refresh_out_of_bounds";
      return result;
    }

    const auto aligned_width {align_dimension(result.requested_width, constraints.width_alignment)};
    const auto aligned_height {align_dimension(result.requested_height, constraints.height_alignment)};
    if (!aligned_width || !aligned_height) {
      result.reason = "geometry_alignment_overflow";
      return result;
    }
    result.adjusted = request_missing || *aligned_width != result.requested_width || *aligned_height != result.requested_height;
    if (result.adjusted && !request_missing && alignment_policy == geometry_alignment_policy_e::require_exact) {
      result.reason = "exact_geometry_unaligned";
      return result;
    }
    result.width = *aligned_width;
    result.height = *aligned_height;
    result.refresh = result.requested_refresh;
    result.fps = static_cast<int>(std::llround(refresh_hz));
    if (result.width > constraints.maximum_width || result.height > constraints.maximum_height) {
      result.reason = "aligned_geometry_out_of_bounds";
      return result;
    }

    std::uint64_t frame_pixels {};
    if (!checked_multiply(static_cast<std::uint64_t>(result.width), static_cast<std::uint64_t>(result.height), frame_pixels)) {
      result.reason = "frame_pixel_overflow";
      return result;
    }
    if (frame_pixels > constraints.maximum_frame_pixels) {
      result.reason = "coded_extent_exceeded";
      return result;
    }
    const long double pixel_rate {static_cast<long double>(frame_pixels) * refresh_hz};
    if (pixel_rate > constraints.maximum_pixel_rate) {
      result.reason = "pixel_rate_exceeded";
      return result;
    }
    std::uint64_t frame_bytes {};
    std::uint64_t buffer_bytes {};
    if (!checked_multiply(frame_pixels, constraints.bytes_per_pixel, frame_bytes) ||
        !checked_multiply(frame_bytes, constraints.buffer_count, buffer_bytes)) {
      result.reason = "buffer_size_overflow";
      return result;
    }
    if (buffer_bytes > constraints.maximum_buffer_bytes) {
      result.reason = "buffer_budget_exceeded";
      return result;
    }

    result.valid = true;
    result.reason = request_missing ? "default_geometry_selected" :
                    result.adjusted ? "geometry_minimally_aligned" :
                                      "request_exact";
    return result;
  }

  display_request_t normalize_display_request(const int requested_width, const int requested_height, const int requested_fps, const int default_width, const int default_height, const int default_fps) {
    return select_display_request(
      requested_width,
      requested_height,
      requested_fps,
      0,
      default_width,
      default_height,
      default_fps,
      geometry_alignment_policy_e::auto_align
    );
  }

  bool retained_session_compatible(const retained_session_key_t &retained, const retained_session_key_t &requested) {
    return retained.width == requested.width &&
           retained.height == requested.height &&
           refresh_equal(retained.refresh, requested.refresh) &&
           retained.hdr == requested.hdr &&
           retained.render_node == requested.render_node &&
           retained.source_identity == requested.source_identity &&
           retained.capture_pixel_format == requested.capture_pixel_format;
  }

  content_rectangle_t fit_content_rectangle(const int source_width, const int source_height, const int output_width, const int output_height, const int alignment) {
    if (source_width <= 0 || source_height <= 0 || output_width <= 0 || output_height <= 0 || alignment <= 0) {
      return {};
    }
    const auto align_down {[alignment](const std::int64_t value) {
      return static_cast<int>(std::max<std::int64_t>(alignment, value - (value % alignment)));
    }};
    int fitted_width {output_width};
    int fitted_height {output_height};
    const auto source_cross {static_cast<std::int64_t>(source_width) * output_height};
    const auto output_cross {static_cast<std::int64_t>(output_width) * source_height};
    if (source_cross > output_cross) {
      fitted_height = align_down(static_cast<std::int64_t>(output_width) * source_height / source_width);
      fitted_width = align_down(output_width);
    } else if (source_cross < output_cross) {
      fitted_width = align_down(static_cast<std::int64_t>(output_height) * source_width / source_height);
      fitted_height = align_down(output_height);
    } else {
      fitted_width = align_down(output_width);
      fitted_height = align_down(output_height);
    }
    fitted_width = std::min(fitted_width, output_width);
    fitted_height = std::min(fitted_height, output_height);
    const int x {((output_width - fitted_width) / 2 / alignment) * alignment};
    const int y {((output_height - fitted_height) / 2 / alignment) * alignment};
    return {x, y, fitted_width, fitted_height};
  }

  content_coordinate_t map_content_coordinate(
    const double output_x,
    const double output_y,
    const content_rectangle_t &rectangle,
    const margin_input_policy_e policy
  ) {
    if (rectangle.width <= 0 || rectangle.height <= 0 || !std::isfinite(output_x) || !std::isfinite(output_y)) {
      return {};
    }
    const double right {static_cast<double>(rectangle.x + rectangle.width)};
    const double bottom {static_cast<double>(rectangle.y + rectangle.height)};
    const bool in_content {output_x >= rectangle.x && output_x <= right && output_y >= rectangle.y && output_y <= bottom};
    if (!in_content && policy == margin_input_policy_e::reject) {
      return {};
    }
    const double clamped_x {std::clamp(output_x, static_cast<double>(rectangle.x), right)};
    const double clamped_y {std::clamp(output_y, static_cast<double>(rectangle.y), bottom)};
    return {
      true,
      std::clamp((clamped_x - rectangle.x) / rectangle.width, 0.0, 1.0),
      std::clamp((clamped_y - rectangle.y) / rectangle.height, 0.0, 1.0),
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
