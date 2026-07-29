/**
 * @file src/codec_policy.h
 * @brief Pure SDR codec policy types and selection helpers.
 */
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace codec_policy {
  /**
   * @brief Video codecs exposed by the existing GameStream encoder pipeline.
   */
  enum class codec_e {
    h264 = 0,  ///< H.264/AVC.
    hevc = 1,  ///< H.265/HEVC.
    av1 = 2,  ///< AV1.
  };

  /**
   * @brief Administrator codec-selection policy.
   */
  enum class policy_e {
    automatic,  ///< Accept a client-selected codec only after host validation.
    h264,  ///< Require H.264 unless explicit recovery fallback applies.
    hevc,  ///< Require HEVC unless explicit recovery fallback applies.
    av1,  ///< Require AV1 unless explicit recovery fallback applies.
  };

  /**
   * @brief Behavior when a manually selected codec is unavailable.
   */
  enum class fallback_e {
    strict,  ///< Reject instead of changing codec.
    h264_recovery,  ///< Permit an explicitly advertised H.264 recovery request.
  };

  /**
   * @brief One codec's client, host, and measured acceptance gates.
   */
  struct capability_t {
    bool client_advertised {false};  ///< Client selected or advertised this codec.
    bool host_open {false};  ///< Existing encoder probe opened this codec.
    bool hardware {true};  ///< Codec is backed by the selected hardware encoder.
    bool geometry_supported {true};  ///< Selected extent and pixel rate are supported.
    bool latency_accepted {true};  ///< Measured latency is within policy budget.
    bool power_accepted {true};  ///< Measured power use is within policy budget.
    bool history_known {false};  ///< Previous-client success information is available.
    bool previous_success {false};  ///< Previous matching client/network use succeeded.
    bool supports_8bit {true};  ///< The probed codec supports an 8-bit SDR profile.
    bool supports_10bit {false};  ///< The probed codec supports a 10-bit SDR profile.
    bool supports_444 {false};  ///< The probed codec supports 4:4:4 chroma.
  };

  /**
   * @brief Inputs for one immutable pre-stream codec decision.
   */
  struct request_t {
    codec_e requested {codec_e::h264};  ///< Codec selected by the RTSP client.
    int bit_depth {8};  ///< Requested SDR bit depth.
    bool chroma_444 {false};  ///< Whether the client requested 4:4:4 chroma.
    policy_e policy {policy_e::automatic};  ///< Administrator selection policy.
    fallback_e fallback {fallback_e::strict};  ///< Explicit manual fallback policy.
    bool allow_software {false};  ///< Whether diagnostic software encoding is allowed.
    std::array<capability_t, 3> capabilities {};  ///< Capability gates indexed by @ref codec_e.
  };

  /**
   * @brief Result of applying codec policy before a stream starts.
   */
  struct result_t {
    bool accepted {false};  ///< Whether stream startup may continue.
    codec_e selected {codec_e::h264};  ///< Codec fixed for this stream.
    int bit_depth {8};  ///< Selected SDR bit depth.
    std::string_view profile {"high"};  ///< Existing protocol/FFmpeg profile name.
    std::string_view reason {"codec_policy_uninitialized"};  ///< Stable decision or rejection reason.
  };

  /**
   * @brief Convert a codec to its stable configuration/status name.
   *
   * @param codec Codec value.
   * @return Stable lower-case codec name.
   */
  constexpr std::string_view to_string(const codec_e codec) {
    switch (codec) {
      case codec_e::hevc:
        return "hevc";
      case codec_e::av1:
        return "av1";
      default:
        return "h264";
    }
  }

  /**
   * @brief Parse an administrator codec policy.
   *
   * @param value Configuration value.
   * @return Parsed policy, or no value when invalid.
   */
  constexpr std::optional<policy_e> parse_policy(const std::string_view value) {
    if (value == "auto") {
      return policy_e::automatic;
    }
    if (value == "h264") {
      return policy_e::h264;
    }
    if (value == "hevc") {
      return policy_e::hevc;
    }
    if (value == "av1") {
      return policy_e::av1;
    }
    return std::nullopt;
  }

  /**
   * @brief Convert a codec policy to its configuration name.
   *
   * @param policy Policy value.
   * @return Stable lower-case policy name.
   */
  constexpr std::string_view to_string(const policy_e policy) {
    switch (policy) {
      case policy_e::h264:
        return "h264";
      case policy_e::hevc:
        return "hevc";
      case policy_e::av1:
        return "av1";
      default:
        return "auto";
    }
  }

  /**
   * @brief Parse the explicit manual-codec fallback behavior.
   *
   * @param value Configuration value.
   * @return Parsed fallback, or no value when invalid.
   */
  constexpr std::optional<fallback_e> parse_fallback(const std::string_view value) {
    if (value == "strict") {
      return fallback_e::strict;
    }
    if (value == "h264") {
      return fallback_e::h264_recovery;
    }
    return std::nullopt;
  }

  /**
   * @brief Convert fallback behavior to its configuration name.
   *
   * @param fallback Fallback value.
   * @return Stable lower-case fallback name.
   */
  constexpr std::string_view to_string(const fallback_e fallback) {
    return fallback == fallback_e::h264_recovery ? "h264" : "strict";
  }

  /**
   * @brief Filter one optional codec advertisement through administrator policy.
   *
   * H.264 is the protocol baseline and is handled separately. This helper
   * controls only HEVC and AV1 capability lines.
   *
   * @param policy Administrator codec policy.
   * @param codec Optional codec represented by the advertisement.
   * @param probed_mode Existing GameStream probe mode, where one is disabled.
   * @return Policy-filtered GameStream mode.
   */
  constexpr int advertised_mode(const policy_e policy, const codec_e codec, const int probed_mode) {
    if (policy == policy_e::automatic) {
      return probed_mode;
    }
    const bool target {(policy == policy_e::hevc && codec == codec_e::hevc) || (policy == policy_e::av1 && codec == codec_e::av1)};
    return target ? probed_mode : 1;
  }

  /**
   * @brief Select and validate the fixed SDR codec for a stream.
   *
   * A client selects one codec in RTSP, so this helper never rewrites an
   * accepted request to a codec the client did not choose. Explicit H.264
   * recovery is accepted only when the manual target is unavailable and the
   * client actually requested H.264 after seeing the recovery advertisement.
   *
   * @param request Client, host, measurement, and administrator inputs.
   * @return Accepted codec/profile or a stable rejection reason.
   */
  constexpr result_t select(const request_t &request) {
    const auto index {static_cast<std::size_t>(request.requested)};
    if (index >= request.capabilities.size()) {
      return {.reason = "codec_unknown"};
    }
    if (request.bit_depth != 8 && request.bit_depth != 10) {
      return {.selected = request.requested, .bit_depth = request.bit_depth, .reason = "codec_bit_depth_invalid"};
    }

    const auto &capability {request.capabilities[index]};
    const bool measured_success {!capability.history_known || capability.previous_success};
    const bool bit_depth_supported {request.bit_depth == 8 ? capability.supports_8bit : capability.supports_10bit};
    const bool chroma_supported {!request.chroma_444 || capability.supports_444};
    const bool candidate_valid {capability.client_advertised && capability.host_open && (capability.hardware || request.allow_software) && capability.geometry_supported && capability.latency_accepted && capability.power_accepted && measured_success && bit_depth_supported && chroma_supported};

    codec_e manual_target {codec_e::h264};
    bool manual {request.policy != policy_e::automatic};
    if (request.policy == policy_e::hevc) {
      manual_target = codec_e::hevc;
    }
    if (request.policy == policy_e::av1) {
      manual_target = codec_e::av1;
    }

    if (manual && request.requested != manual_target) {
      const auto target_index {static_cast<std::size_t>(manual_target)};
      const auto &target {request.capabilities[target_index]};
      const bool target_bit_depth {request.bit_depth == 8 ? target.supports_8bit : target.supports_10bit};
      const bool target_chroma {!request.chroma_444 || target.supports_444};
      const bool target_history {!target.history_known || target.previous_success};
      const bool target_usable {target.host_open && (target.hardware || request.allow_software) && target.geometry_supported && target.latency_accepted && target.power_accepted && target_history && target_bit_depth && target_chroma};
      const bool h264_recovery {request.fallback == fallback_e::h264_recovery && !target_usable && request.requested == codec_e::h264 && candidate_valid};
      if (!h264_recovery) {
        return {.selected = request.requested, .bit_depth = request.bit_depth, .reason = target_usable ? "manual_codec_mismatch" : "manual_codec_unavailable"};
      }
      return {.accepted = true, .selected = codec_e::h264, .bit_depth = 8, .profile = "high", .reason = "manual_codec_h264_fallback"};
    }

    if (!candidate_valid) {
      return {.selected = request.requested, .bit_depth = request.bit_depth, .reason = manual ? "manual_codec_unavailable" : "requested_codec_unavailable"};
    }

    const std::string_view profile {request.chroma_444 && request.requested == codec_e::h264 ? "high444" : request.chroma_444 && request.requested == codec_e::hevc    ? "rext" :
                                                                                                         request.chroma_444 && request.requested == codec_e::av1       ? "high" :
                                                                                                         request.requested == codec_e::h264                            ? "high" :
                                                                                                         request.requested == codec_e::hevc && request.bit_depth == 10 ? "main10" :
                                                                                                                                                                         "main"};
    return {.accepted = true, .selected = request.requested, .bit_depth = request.bit_depth, .profile = profile, .reason = manual ? "manual_codec_selected" : "client_codec_probed"};
  }
}  // namespace codec_policy
