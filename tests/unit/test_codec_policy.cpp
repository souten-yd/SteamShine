/**
 * @file tests/unit/test_codec_policy.cpp
 * @brief Tests for probed SDR codec-selection policy.
 */

#include "src/codec_policy.h"

#include <gtest/gtest.h>

namespace {
  /**
   * @brief Build an accepted hardware capability for policy tests.
   *
   * @param ten_bit Whether Main10/Main 10-bit SDR is available.
   * @return Capability with every mandatory gate accepted.
   */
  codec_policy::capability_t accepted_capability(const bool ten_bit = false) {
    return {
      .client_advertised = true,
      .host_open = true,
      .hardware = true,
      .geometry_supported = true,
      .latency_accepted = true,
      .power_accepted = true,
      .supports_8bit = true,
      .supports_10bit = ten_bit,
    };
  }
}  // namespace

TEST(CodecPolicyTest, ParsesOnlyDocumentedPolicyValues) {
  EXPECT_EQ(codec_policy::parse_policy("auto"), codec_policy::policy_e::automatic);
  EXPECT_EQ(codec_policy::parse_policy("h264"), codec_policy::policy_e::h264);
  EXPECT_EQ(codec_policy::parse_policy("hevc"), codec_policy::policy_e::hevc);
  EXPECT_EQ(codec_policy::parse_policy("av1"), codec_policy::policy_e::av1);
  EXPECT_FALSE(codec_policy::parse_policy("always-av1"));
  EXPECT_EQ(codec_policy::parse_fallback("strict"), codec_policy::fallback_e::strict);
  EXPECT_EQ(codec_policy::parse_fallback("h264"), codec_policy::fallback_e::h264_recovery);
  EXPECT_FALSE(codec_policy::parse_fallback("silent"));
}

TEST(CodecPolicyTest, AcceptsClientAndHostIntersection) {
  for (const auto codec : {codec_policy::codec_e::h264, codec_policy::codec_e::hevc, codec_policy::codec_e::av1}) {
    codec_policy::request_t request;
    request.requested = codec;
    request.capabilities[static_cast<std::size_t>(codec)] = accepted_capability();
    const auto result {codec_policy::select(request)};
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.selected, codec);
    EXPECT_EQ(result.bit_depth, 8);
    EXPECT_EQ(result.reason, "client_codec_probed");
  }
}

TEST(CodecPolicyTest, FiltersOptionalCodecAdvertisements) {
  EXPECT_EQ(codec_policy::advertised_mode(codec_policy::policy_e::automatic, codec_policy::codec_e::av1, 3), 3);
  EXPECT_EQ(codec_policy::advertised_mode(codec_policy::policy_e::h264, codec_policy::codec_e::hevc, 3), 1);
  EXPECT_EQ(codec_policy::advertised_mode(codec_policy::policy_e::hevc, codec_policy::codec_e::hevc, 3), 3);
  EXPECT_EQ(codec_policy::advertised_mode(codec_policy::policy_e::hevc, codec_policy::codec_e::av1, 3), 1);
  EXPECT_EQ(codec_policy::advertised_mode(codec_policy::policy_e::av1, codec_policy::codec_e::av1, 2), 2);
}

TEST(CodecPolicyTest, RejectsEachMissingCandidateGate) {
  codec_policy::request_t request;
  request.requested = codec_policy::codec_e::av1;
  auto &capability {request.capabilities[2]};
  capability = accepted_capability();

  capability.client_advertised = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  capability = accepted_capability();
  capability.host_open = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  capability = accepted_capability();
  capability.hardware = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  request.allow_software = true;
  EXPECT_TRUE(codec_policy::select(request).accepted);
  request.allow_software = false;
  capability = accepted_capability();
  capability.geometry_supported = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  capability = accepted_capability();
  capability.latency_accepted = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  capability = accepted_capability();
  capability.power_accepted = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  capability = accepted_capability();
  capability.history_known = true;
  capability.previous_success = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
}

TEST(CodecPolicyTest, KeepsManualSelectionStrict) {
  codec_policy::request_t request;
  request.requested = codec_policy::codec_e::h264;
  request.policy = codec_policy::policy_e::av1;
  request.capabilities[0] = accepted_capability();
  request.capabilities[2] = accepted_capability();
  EXPECT_EQ(codec_policy::select(request).reason, "manual_codec_mismatch");

  request.requested = codec_policy::codec_e::av1;
  EXPECT_TRUE(codec_policy::select(request).accepted);
  EXPECT_EQ(codec_policy::select(request).reason, "manual_codec_selected");
}

TEST(CodecPolicyTest, UsesH264RecoveryOnlyWhenExplicitAndTargetUnavailable) {
  codec_policy::request_t request;
  request.requested = codec_policy::codec_e::h264;
  request.policy = codec_policy::policy_e::av1;
  request.fallback = codec_policy::fallback_e::h264_recovery;
  request.capabilities[0] = accepted_capability();
  request.capabilities[2].host_open = false;

  const auto fallback {codec_policy::select(request)};
  EXPECT_TRUE(fallback.accepted);
  EXPECT_EQ(fallback.selected, codec_policy::codec_e::h264);
  EXPECT_EQ(fallback.reason, "manual_codec_h264_fallback");

  request.capabilities[2] = accepted_capability();
  EXPECT_EQ(codec_policy::select(request).reason, "manual_codec_mismatch");
  request.capabilities[2].host_open = false;
  request.fallback = codec_policy::fallback_e::strict;
  EXPECT_EQ(codec_policy::select(request).reason, "manual_codec_unavailable");

  request.fallback = codec_policy::fallback_e::h264_recovery;
  request.capabilities[2] = accepted_capability();
  request.capabilities[2].latency_accepted = false;
  EXPECT_EQ(codec_policy::select(request).reason, "manual_codec_h264_fallback");
}

TEST(CodecPolicyTest, ValidatesProfileAndBitDepth) {
  codec_policy::request_t request;
  request.requested = codec_policy::codec_e::hevc;
  request.bit_depth = 10;
  request.capabilities[1] = accepted_capability(true);
  auto result {codec_policy::select(request)};
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.profile, "main10");

  request.capabilities[1].supports_10bit = false;
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");
  request.bit_depth = 12;
  EXPECT_EQ(codec_policy::select(request).reason, "codec_bit_depth_invalid");

  request.requested = codec_policy::codec_e::av1;
  request.bit_depth = 10;
  request.capabilities[2] = accepted_capability(true);
  result = codec_policy::select(request);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.profile, "main");
}

TEST(CodecPolicyTest, ValidatesChromaAndReportsExistingProfiles) {
  codec_policy::request_t request;
  request.requested = codec_policy::codec_e::hevc;
  request.chroma_444 = true;
  request.capabilities[1] = accepted_capability(true);
  EXPECT_EQ(codec_policy::select(request).reason, "requested_codec_unavailable");

  request.capabilities[1].supports_444 = true;
  EXPECT_EQ(codec_policy::select(request).profile, "rext");
  request.requested = codec_policy::codec_e::h264;
  request.capabilities[0] = accepted_capability();
  request.capabilities[0].supports_444 = true;
  EXPECT_EQ(codec_policy::select(request).profile, "high444");
  request.requested = codec_policy::codec_e::av1;
  request.capabilities[2] = accepted_capability();
  request.capabilities[2].supports_444 = true;
  EXPECT_EQ(codec_policy::select(request).profile, "high");
}
