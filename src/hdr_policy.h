/**
 * @file src/hdr_policy.h
 * @brief Pure HDR10 policy evaluation shared by negotiation and runtime gates.
 */
#pragma once

#include <optional>
#include <string_view>

namespace hdr_policy {
  /**
   * @brief Administrator policy for client HDR requests.
   */
  enum class policy_e {
    off,  ///< Always select the safe SDR path.
    auto_select,  ///< Select HDR only when every required gate passes.
    require,  ///< Reject the stream when HDR cannot be selected.
  };

  /**
   * @brief Capability facts required for one HDR10 transaction.
   */
  struct gates_t {
    bool client_requested;  ///< Whether the launch request asks for HDR.
    bool client_capable;  ///< Whether RTSP requests a 10-bit dynamic range.
    bool source_capable;  ///< Whether the selected source can produce HDR.
    bool display_active;  ///< Whether the display or Gamescope canvas is in HDR mode.
    bool capture_10bit;  ///< Whether capture can preserve a 10-bit format.
    bool capture_metadata;  ///< Whether source HDR10 metadata is available.
    bool conversion_10bit;  ///< Whether the conversion path supports a 10-bit output.
    bool encoder_10bit;  ///< Whether the selected codec and encoder support 10-bit video.
    bool signaling_available;  ///< Whether GameStream HDR signaling is available.
  };

  /**
   * @brief Result of applying one HDR policy to capability gates.
   */
  struct result_t {
    bool accepted;  ///< Whether stream startup may continue.
    bool selected;  ///< Whether the resulting stream is HDR10 rather than SDR.
    unsigned bit_depth;  ///< Selected encoded bit depth.
    std::string_view reason;  ///< Stable selection, fallback, or rejection reason.
  };

  /**
   * @brief Parse a documented HDR policy value.
   *
   * @param value Configuration value to parse.
   * @return Parsed policy, or no value when the text is unsupported.
   */
  constexpr std::optional<policy_e> parse_policy(const std::string_view value) {
    if (value == "off") {
      return policy_e::off;
    }
    if (value == "auto") {
      return policy_e::auto_select;
    }
    if (value == "require") {
      return policy_e::require;
    }
    return std::nullopt;
  }

  /**
   * @brief Convert an HDR policy into its stable configuration spelling.
   *
   * @param policy Policy to convert.
   * @return Stable configuration spelling.
   */
  constexpr std::string_view to_string(const policy_e policy) {
    switch (policy) {
      case policy_e::off:
        return "off";
      case policy_e::auto_select:
        return "auto";
      case policy_e::require:
        return "require";
    }
    return "off";
  }

  /**
   * @brief Evaluate HDR10 gates without opening displays or encoders.
   *
   * Auto mode returns an accepted SDR result for a failed gate. Required mode
   * returns a rejection with the same stable gate reason.
   *
   * @param policy Administrator HDR policy.
   * @param gates Runtime capability facts.
   * @return HDR selection, SDR fallback, or startup rejection.
   */
  constexpr result_t evaluate(const policy_e policy, const gates_t &gates) {
    if (policy == policy_e::off) {
      return {true, false, 8, "hdr_disabled_by_policy"};
    }
    if (!gates.client_requested) {
      return {policy != policy_e::require, false, 8, policy == policy_e::require ? "hdr_required_but_not_requested" : "hdr_not_requested"};
    }

    const auto reject_or_fallback = [policy](const std::string_view reason) constexpr {
      return result_t {policy != policy_e::require, false, 8, reason};
    };
    if (!gates.client_capable) {
      return reject_or_fallback("hdr_client_not_capable");
    }
    if (!gates.source_capable) {
      return reject_or_fallback("hdr_source_not_capable");
    }
    if (!gates.display_active) {
      return reject_or_fallback("hdr_display_not_active");
    }
    if (!gates.capture_10bit) {
      return reject_or_fallback("hdr_capture_not_10bit");
    }
    if (!gates.capture_metadata) {
      return reject_or_fallback("hdr_capture_metadata_missing");
    }
    if (!gates.conversion_10bit) {
      return reject_or_fallback("hdr_conversion_not_10bit");
    }
    if (!gates.encoder_10bit) {
      return reject_or_fallback("hdr_encoder_profile_unavailable");
    }
    if (!gates.signaling_available) {
      return reject_or_fallback("hdr_signaling_unavailable");
    }
    return {true, true, 10, "hdr_ready"};
  }
}  // namespace hdr_policy
