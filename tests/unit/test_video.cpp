/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <future>
#include <src/video.h>

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    BaseTest::SetUp();
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#if defined(__linux__) || defined(__FreeBSD__)
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

struct FramerateX100Test: BaseTest, testing::WithParamInterface<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

struct FramerateToRationalTest: testing::TestWithParam<std::tuple<int, int, AVRational>> {};

TEST_P(FramerateToRationalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  auto res = video::framerate_to_rational(config);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateToRationalTests,
  FramerateToRationalTest,
  testing::Values(
    std::make_tuple(60, 0, AVRational {60, 1}),  // no X100 value, fall back to integer framerate
    std::make_tuple(60, 5994, AVRational {60000, 1001}),
    std::make_tuple(120, 11988, AVRational {120000, 1001}),
    std::make_tuple(24, 2398, AVRational {24000, 1001})
  )
);

struct CaptureFrameIntervalTest: testing::TestWithParam<std::tuple<int, int, std::chrono::nanoseconds>> {};

TEST_P(CaptureFrameIntervalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  ASSERT_EQ(expected, video::capture_frame_interval(config));
}

INSTANTIATE_TEST_SUITE_P(
  CaptureFrameIntervalTests,
  CaptureFrameIntervalTest,
  testing::Values(
    std::make_tuple(60, 0, std::chrono::nanoseconds {16666666}),
    std::make_tuple(60, 5994, std::chrono::nanoseconds {16683333}),  // 1e9 * 1001 / 60000
    std::make_tuple(120, 11988, std::chrono::nanoseconds {8341666})  // 1e9 * 1001 / 120000
  )
);

/**
 * @brief Verify bounded pipeline counters and age samples are resettable.
 */
TEST(VideoPipelineDiagnostics, RecordsBoundedStages) {
  video::reset_pipeline_diagnostics();
  const auto timestamp {std::chrono::steady_clock::now() - std::chrono::milliseconds {2}};

  video::record_capture_enqueued(timestamp, false);
  video::record_capture_enqueued(timestamp, true);
  video::record_capture_dequeued();
  video::record_encode_started(timestamp, true);
  video::record_encode_finished();
  video::record_encode_started(std::nullopt, false);
  video::record_encode_finished();
  video::record_encode_started(std::nullopt, false);
  video::record_encode_finished();
  video::record_encode_started(std::nullopt, true);
  video::record_encode_finished();
  video::record_capture_deadline_misses(3);
  video::record_pipewire_buffer(false, false);
  video::record_pipewire_buffer(true, true);
  video::record_pipewire_buffer_replaced();
  video::record_pipewire_unique_frame();
  video::record_pipewire_unique_frame();
  video::record_pipewire_queue_overflow();
  video::record_requested_frame_rate(60000, 1001);
  video::record_pipewire_negotiated_frame_rate(0, 1, 60000, 1001);
  video::record_output_static_mode(true);
  video::record_network_enqueued(1000, 1);
  video::record_network_enqueued(500, 2);
  video::record_network_dequeued(1000, 1, timestamp);
  video::record_socket_outq(4096);
  EXPECT_TRUE(video::record_idr_request(video::idr_reason_e::reconnect));
  video::record_idr_submitted(video::idr_reason_e::reconnect);
  video::record_idr_emitted(true);
  video::record_idr_emitted(true);

  const auto snapshot {video::pipeline_diagnostics_snapshot()};
  EXPECT_EQ(snapshot.capture_queue_current, 0U);
  EXPECT_EQ(snapshot.capture_queue_max, 1U);
  EXPECT_EQ(snapshot.capture_frames_replaced, 1U);
  EXPECT_EQ(snapshot.pipewire_buffers_received, 2U);
  EXPECT_EQ(snapshot.pipewire_buffers_replaced, 1U);
  EXPECT_EQ(snapshot.encoder_queue_current, 0U);
  EXPECT_EQ(snapshot.encoder_queue_max, 1U);
  EXPECT_EQ(snapshot.capture_deadline_misses, 3U);
  EXPECT_EQ(snapshot.encoded_unique_frames, 2U);
  EXPECT_EQ(snapshot.encoded_duplicate_frames, 2U);
  EXPECT_EQ(snapshot.encoded_frames, 4U);
  EXPECT_EQ(snapshot.duplicate_frames, 2U);
  EXPECT_EQ(snapshot.duplicate_run_max, 2U);
  EXPECT_EQ(snapshot.pipewire_buffers_received, 2U);
  EXPECT_EQ(snapshot.pipewire_unique_frames, 2U);
  EXPECT_EQ(snapshot.pipewire_redundant_pts, 1U);
  EXPECT_EQ(snapshot.pipewire_no_damage_frames, 1U);
  EXPECT_EQ(snapshot.pipewire_queue_overflows, 1U);
  EXPECT_EQ(snapshot.requested_fps_numerator, 60000U);
  EXPECT_EQ(snapshot.requested_fps_denominator, 1001U);
  EXPECT_EQ(snapshot.negotiated_fps_numerator, 0U);
  EXPECT_EQ(snapshot.negotiated_fps_denominator, 1U);
  EXPECT_EQ(snapshot.negotiated_max_fps_numerator, 60000U);
  EXPECT_EQ(snapshot.negotiated_max_fps_denominator, 1001U);
  EXPECT_EQ(snapshot.output_status_reason, "consumer_limited");
  EXPECT_EQ(snapshot.source_interarrival_ms.count, 1U);
  EXPECT_EQ(snapshot.encode_interarrival_ms.count, 3U);
  EXPECT_EQ(snapshot.network_queue_bytes, 500U);
  EXPECT_EQ(snapshot.network_queue_frames, 1U);
  EXPECT_EQ(snapshot.network_queue_frames_max, 2U);
  EXPECT_EQ(snapshot.socket_outq_bytes, 4096U);
  EXPECT_EQ(snapshot.socket_outq_bytes_max, 4096U);
  EXPECT_EQ(snapshot.idr_requests, 1U);
  EXPECT_EQ(snapshot.idr_emitted, 2U);
  EXPECT_EQ(snapshot.idr_reason_reconnect, 1U);
  EXPECT_EQ(snapshot.idr_reason_periodic, 1U);
  EXPECT_EQ(snapshot.frame_age_at_capture_ms.count, 2U);
  EXPECT_EQ(snapshot.frame_age_at_encode_ms.count, 1U);
  EXPECT_EQ(snapshot.frame_age_at_network_ms.count, 1U);
}

/**
 * @brief Verify a transient latest-frame replacement does not imply backlog.
 */
TEST(VideoPipelineDiagnostics, AllowsTransientCaptureReplacement) {
  video::reset_pipeline_diagnostics();
  video::record_capture_enqueued(std::nullopt, false);
  video::record_capture_enqueued(std::nullopt, true);

  EXPECT_EQ(video::pipeline_diagnostics_snapshot().output_status_reason, "ok");
}

/**
 * @brief Verify sustained latest-frame replacement reports consumer limitation.
 */
TEST(VideoPipelineDiagnostics, ReportsPersistentCaptureReplacementPressure) {
  video::reset_pipeline_diagnostics();
  video::record_capture_enqueued(std::nullopt, true);
  video::record_capture_enqueued(std::nullopt, true);
  video::record_capture_enqueued(std::nullopt, true);

  EXPECT_EQ(video::pipeline_diagnostics_snapshot().output_status_reason, "consumer_limited");
}

/**
 * @brief Verify duplicate client IDR requests are limited without delaying recovery.
 */
TEST(VideoPipelineDiagnostics, RateLimitsOnlyDuplicateClientIdrRequests) {
  video::reset_pipeline_diagnostics();

  EXPECT_TRUE(video::record_idr_request(video::idr_reason_e::client_request));
  EXPECT_FALSE(video::record_idr_request(video::idr_reason_e::client_request));
  EXPECT_TRUE(video::record_idr_request(video::idr_reason_e::recovery));
  EXPECT_TRUE(video::record_idr_request(video::idr_reason_e::reconnect));

  const auto snapshot {video::pipeline_diagnostics_snapshot()};
  EXPECT_EQ(snapshot.idr_requests, 3U);
}

/**
 * @brief Verify client loss feedback changes only the bounded video target.
 */
TEST(VideoPipelineDiagnostics, RecordsBoundedAdaptiveBitrateDecision) {
  const auto original_enabled {config::video.adaptive_bitrate};
  const auto original_maximum {config::video.max_bitrate};
  config::video.adaptive_bitrate = true;
  config::video.max_bitrate = 12000;

  video::reset_pipeline_diagnostics();
  video::config_t stream_config {};
  stream_config.framerate = 60;
  stream_config.bitrate = 10000;
  video::initialize_adaptive_bitrate(stream_config);
  video::record_bitrate_feedback(1, 500);
  video::record_bitrate_feedback(0, 500);
  video::record_bitrate_feedback(0, 500);
  video::record_bitrate_feedback(0, 500);

  const auto snapshot {video::pipeline_diagnostics_snapshot()};
  EXPECT_TRUE(snapshot.adaptive_bitrate_enabled);
  EXPECT_EQ(snapshot.bitrate.minimum_kbps, 2500U);
  EXPECT_EQ(snapshot.bitrate.initial_kbps, 10000U);
  EXPECT_EQ(snapshot.bitrate.target_kbps, 9000U);
  EXPECT_EQ(snapshot.bitrate.maximum_kbps, 10000U);
  EXPECT_EQ(snapshot.bitrate.peak_kbps, 9000U);
  EXPECT_EQ(snapshot.bitrate.vbv_kbits, 149U);
  EXPECT_EQ(snapshot.congestion_state, adaptive_bitrate::congestion_state_e::warning);
  EXPECT_EQ(snapshot.bitrate_reason, "isolated_network_pressure");
  EXPECT_EQ(snapshot.bitrate_feedback_samples, 4U);
  EXPECT_EQ(snapshot.bitrate_lost_packets, 1U);
  EXPECT_EQ(snapshot.bitrate_active_kbps, 10000U);
  EXPECT_EQ(snapshot.bitrate_learned_next_kbps, 10000U);

  config::video.adaptive_bitrate = original_enabled;
  config::video.max_bitrate = original_maximum;
}

/**
 * @brief Verify capture handoff retains only the newest source frame.
 */
TEST(VideoPipelineBackpressure, CaptureHandoffReplacesObsoleteFrame) {
  safe::event_t<std::uint64_t> capture_handoff;

  EXPECT_FALSE(capture_handoff.raise_replacing(1U));
  EXPECT_TRUE(capture_handoff.raise_replacing(2U));
  EXPECT_EQ(capture_handoff.pop(), 2U);
}

/**
 * @brief Verify a slow network consumer preserves order within the two-frame bound.
 */
TEST(VideoPipelineBackpressure, SlowNetworkConsumerAppliesOrderedBackpressure) {
  safe::queue_t<std::uint64_t> network_queue {
    video::NETWORK_QUEUE_FRAME_LIMIT,
    safe::queue_overflow_e::block_producer
  };
  ASSERT_TRUE(network_queue.raise(1U));
  ASSERT_TRUE(network_queue.raise(2U));

  auto producer = std::async(std::launch::async, [&network_queue]() {
    return network_queue.raise(3U);
  });
  EXPECT_EQ(producer.wait_for(std::chrono::milliseconds {20}), std::future_status::timeout);
  EXPECT_EQ(network_queue.size(), video::NETWORK_QUEUE_FRAME_LIMIT);
  EXPECT_EQ(network_queue.pop(std::chrono::seconds {1}), 1U);
  EXPECT_EQ(producer.wait_for(std::chrono::seconds {1}), std::future_status::ready);
  EXPECT_TRUE(producer.get());
  EXPECT_EQ(network_queue.pop(std::chrono::seconds {1}), 2U);
  EXPECT_EQ(network_queue.pop(std::chrono::seconds {1}), 3U);
}
