/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include "../tests_common.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <src/stream.h>
#include <string>
#include <vector>

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

/**
 * @brief Verify protocol refresh hints preserve exact rational rates.
 */
TEST(StreamNegotiationTests, PreservesRationalRefreshRate) {
  const auto ntsc_like = stream::rational_rate_from_protocol(60, 5994);
  const auto integer = stream::rational_rate_from_protocol(60);

  EXPECT_EQ(ntsc_like.numerator, 2997U);
  EXPECT_EQ(ntsc_like.denominator, 50U);
  EXPECT_EQ(integer.numerator, 60U);
  EXPECT_EQ(integer.denominator, 1U);
  EXPECT_NE(ntsc_like.numerator * integer.denominator, integer.numerator * ntsc_like.denominator);
}

/**
 * @brief Verify reconnects receive independent generations and request storage.
 */
TEST(StreamNegotiationTests, SeparatesReconnectGenerations) {
  rtsp_stream::launch_session_t first {};
  first.id = 10;
  first.width = 1920;
  first.height = 1080;
  first.fps = 60;
  first.unique_id = "client-a";
  stream::initialize_launch_negotiation(first);

  rtsp_stream::launch_session_t resumed {};
  resumed.id = 11;
  resumed.width = 1280;
  resumed.height = 720;
  resumed.fps = 90;
  resumed.unique_id = "client-a";
  stream::initialize_launch_negotiation(resumed);

  EXPECT_EQ(first.negotiation.generation, 10U);
  EXPECT_EQ(resumed.negotiation.generation, 11U);
  EXPECT_EQ(first.negotiation.requested.launch_geometry.width, 1920U);
  EXPECT_EQ(resumed.negotiation.requested.launch_geometry.width, 1280U);
  EXPECT_EQ(first.negotiation.requested.launch_geometry.frame_rate.numerator, 60U);
  EXPECT_EQ(resumed.negotiation.requested.launch_geometry.frame_rate.numerator, 90U);
}

/**
 * @brief Verify NVHTTP and RTSP values populate one generation without overwriting launch data.
 */
TEST(StreamNegotiationTests, KeepsRequestedStagesSeparateFromSelection) {
  rtsp_stream::launch_session_t launch {};
  launch.id = 42;
  launch.width = 1920;
  launch.height = 1080;
  launch.fps = 60;
  launch.unique_id = "moonlight-client";
  launch.appid = 7;
  launch.hdr_requested = true;
  launch.enable_hdr = false;
  stream::initialize_launch_negotiation(launch);

  stream::config_t config {};
  config.monitor.width = 1280;
  config.monitor.height = 720;
  config.monitor.framerate = 60;
  config.monitor.framerateX100 = 5994;
  config.monitor.bitrate = 12000;
  config.monitor.videoFormat = 1;
  config.monitor.dynamicRange = 1;
  config.monitor.requestedDynamicRange = 1;
  config.monitor.encoderCscMode = 2;
  config.monitor.chromaSamplingType = 0;

  const auto requested_monitor {config.monitor};
  stream::populate_rtsp_negotiation(launch, config, requested_monitor, 15000, 20000, false);

  const auto &snapshot = launch.negotiation;
  EXPECT_EQ(snapshot.generation, 42U);
  EXPECT_EQ(snapshot.requested.client_id, "moonlight-client");
  EXPECT_EQ(snapshot.requested.application_id, 7);
  EXPECT_EQ(snapshot.requested.launch_geometry.width, 1920U);
  EXPECT_EQ(snapshot.requested.launch_geometry.height, 1080U);
  EXPECT_EQ(snapshot.requested.launch_geometry.frame_rate.numerator, 60U);
  EXPECT_EQ(snapshot.requested.stream_geometry.width, 1280U);
  EXPECT_EQ(snapshot.requested.stream_geometry.height, 720U);
  EXPECT_EQ(snapshot.requested.stream_geometry.frame_rate.numerator, 2997U);
  EXPECT_EQ(snapshot.requested.stream_geometry.frame_rate.denominator, 50U);
  EXPECT_EQ(snapshot.requested.client_bitrate_ceiling_bps, 15000000U);
  EXPECT_EQ(snapshot.requested.configured_total_bitrate_bps, 20000000U);
  EXPECT_EQ(snapshot.requested.client_codec_mask, 1U << 1);
  EXPECT_TRUE(snapshot.requested.hdr_requested);
  EXPECT_TRUE(snapshot.selected.color.hdr_requested);
  EXPECT_TRUE(snapshot.requested.ten_bit_requested);
  EXPECT_EQ(snapshot.selected.encode_geometry.width, 1280U);
  EXPECT_EQ(snapshot.selected.codec, 1);
  EXPECT_EQ(snapshot.selected.profile, "hevc_main10");
  EXPECT_EQ(snapshot.selected.bitrate.target_bps, 12000000U);
  EXPECT_NE(std::ranges::find(snapshot.fallback_reasons, "rtsp_geometry_differs_from_launch"), snapshot.fallback_reasons.end());
  EXPECT_NE(std::ranges::find(snapshot.fallback_reasons, "rtsp_frame_rate_differs_from_launch"), snapshot.fallback_reasons.end());
  EXPECT_NE(std::ranges::find(snapshot.fallback_reasons, "configured_total_bitrate_adjusted_for_overhead"), snapshot.fallback_reasons.end());
}

/**
 * @brief Verify fallback reasons are stable and do not grow from repeated updates.
 */
TEST(StreamNegotiationTests, DeduplicatesFallbackReasons) {
  stream::stream_negotiation_snapshot_t snapshot;
  stream::add_fallback_reason(snapshot, "codec_fallback");
  stream::add_fallback_reason(snapshot, "codec_fallback");
  stream::add_fallback_reason(snapshot, "");

  ASSERT_EQ(snapshot.fallback_reasons.size(), 1U);
  EXPECT_EQ(snapshot.fallback_reasons.front(), "codec_fallback");
}

/**
 * @brief Verify opened encoder facts and all four stages remain visible in JSON.
 */
TEST(StreamNegotiationTests, SerializesSelectedActiveAndObservedSeparately) {
  stream::stream_negotiation_snapshot_t snapshot;
  snapshot.generation = 9;
  snapshot.selected.source_geometry = {1280, 720, {60, 1}};
  snapshot.selected.encode_geometry = {1280, 720, {60, 1}};
  snapshot.selected.codec = 0;
  snapshot.selected.profile = "h264_8bit";
  snapshot.selected.bitrate.target_bps = 10000000;

  stream::populate_active_video(
    snapshot,
    1920,
    1080,
    "vulkan",
    "h264_vulkan",
    video::sunshine_colorspace_t {video::colorspace_e::rec709, false, 8}
  );
  snapshot.observed.source_fps = 59.5;
  snapshot.observed.encode_fps = 60.0;

  const auto json = stream::negotiation_snapshot_json(snapshot);
  EXPECT_TRUE(json.at("available"));
  EXPECT_TRUE(json.contains("requested"));
  EXPECT_TRUE(json.contains("selected"));
  EXPECT_TRUE(json.contains("active"));
  EXPECT_TRUE(json.contains("observed"));
  EXPECT_EQ(json.at("selected").at("source_geometry").at("width"), 1280U);
  EXPECT_EQ(json.at("active").at("source_geometry").at("width"), 1920U);
  EXPECT_EQ(json.at("active").at("backend"), "vulkan");
  EXPECT_EQ(json.at("active").at("color").at("colorspace"), "rec709");
  EXPECT_DOUBLE_EQ(json.at("observed").at("source_fps"), 59.5);
  EXPECT_LT(json.dump().size(), 16384U);
}
