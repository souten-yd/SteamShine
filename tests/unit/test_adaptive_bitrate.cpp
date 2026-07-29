/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Tests for bounded adaptive video bitrate decisions.
 */
#include <gtest/gtest.h>
#include <src/adaptive_bitrate.h>

namespace {
  /**
   * @brief Complete one 2 s controller window using four identical samples.
   *
   * @param controller Controller receiving the samples.
   * @param sample Observation repeated at 500 ms intervals.
   * @return Decision produced at the window boundary.
   */
  adaptive_bitrate::decision_t complete_window(
    adaptive_bitrate::controller_t &controller,
    const adaptive_bitrate::sample_t sample = {}
  ) {
    adaptive_bitrate::decision_t decision;
    for (int index = 0; index < 4; ++index) {
      decision = controller.observe(sample);
    }
    return decision;
  }
}  // namespace

/**
 * @brief Verify invalid and reversed inputs form one ordered envelope.
 */
TEST(AdaptiveBitrateEnvelope, ClampsEveryTargetToEffectiveBounds) {
  const auto low {adaptive_bitrate::make_envelope(0, 9000, 4000, 0)};
  EXPECT_EQ(low.minimum_kbps, 1U);
  EXPECT_EQ(low.initial_kbps, 4000U);
  EXPECT_EQ(low.target_kbps, 4000U);
  EXPECT_EQ(low.maximum_kbps, 4000U);
  EXPECT_EQ(low.peak_kbps, 4000U);
  EXPECT_EQ(low.vbv_kbits, 4000U);

  const auto reversed {adaptive_bitrate::make_envelope(8000, 2000, 4000, 60)};
  EXPECT_EQ(reversed.minimum_kbps, 4000U);
  EXPECT_EQ(reversed.initial_kbps, 4000U);
  EXPECT_EQ(reversed.vbv_kbits, 66U);
}

/**
 * @brief Verify persistent queue pressure reduces the video target by 20%.
 */
TEST(AdaptiveBitrateController, ReducesPersistentCongestionBeforeGeometry) {
  adaptive_bitrate::controller_t controller {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  adaptive_bitrate::sample_t sample;
  sample.network_queue_frames = 2;
  const auto decision {complete_window(controller, sample)};
  EXPECT_EQ(decision.state, adaptive_bitrate::congestion_state_e::congested);
  EXPECT_EQ(decision.target_kbps, 8000U);
  EXPECT_EQ(controller.envelope().vbv_kbits, 132U);
  EXPECT_TRUE(decision.changed);
  EXPECT_EQ(decision.reason, "persistent_congestion");
}

/**
 * @brief Verify isolated loss uses the smaller 10% reduction.
 */
TEST(AdaptiveBitrateController, ReducesRepeatedIsolatedLossCautiously) {
  adaptive_bitrate::controller_t controller {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  adaptive_bitrate::sample_t sample;
  sample.lost_packets = 1;
  const auto decision {complete_window(controller, sample)};
  EXPECT_EQ(decision.state, adaptive_bitrate::congestion_state_e::congested);
  EXPECT_EQ(decision.target_kbps, 8000U);

  adaptive_bitrate::controller_t isolated {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  sample.lost_packets = 0;
  isolated.observe({.lost_packets = 1});
  isolated.observe(sample);
  isolated.observe(sample);
  const auto isolated_decision {isolated.observe(sample)};
  EXPECT_EQ(isolated_decision.state, adaptive_bitrate::congestion_state_e::warning);
  EXPECT_EQ(isolated_decision.target_kbps, 9000U);
}

/**
 * @brief Verify RTT warning and kernel socket pressure use distinct severities.
 */
TEST(AdaptiveBitrateController, ClassifiesRttAndSocketQueueTraces) {
  adaptive_bitrate::controller_t latency {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  adaptive_bitrate::sample_t latency_sample;
  latency_sample.rtt_ms = 120;
  const auto warning {complete_window(latency, latency_sample)};
  EXPECT_EQ(warning.state, adaptive_bitrate::congestion_state_e::warning);
  EXPECT_EQ(warning.target_kbps, 9000U);

  adaptive_bitrate::controller_t socket {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  adaptive_bitrate::sample_t socket_sample;
  socket_sample.socket_outq_bytes = 1'250'000;
  const auto congested {complete_window(socket, socket_sample)};
  EXPECT_EQ(congested.state, adaptive_bitrate::congestion_state_e::congested);
  EXPECT_EQ(congested.target_kbps, 8000U);
}

/**
 * @brief Verify reductions clamp at the minimum without reporting false changes.
 */
TEST(AdaptiveBitrateController, NeverDropsBelowMinimum) {
  adaptive_bitrate::controller_t controller {adaptive_bitrate::make_envelope(8000, 9000, 12000, 60)};
  adaptive_bitrate::sample_t sample;
  sample.rtt_ms = 250;
  EXPECT_EQ(complete_window(controller, sample).target_kbps, 8000U);
  const auto held {complete_window(controller, sample)};
  EXPECT_EQ(held.target_kbps, 8000U);
  EXPECT_FALSE(held.changed);
}

/**
 * @brief Verify cooldown and clean-window hysteresis prevent oscillation.
 */
TEST(AdaptiveBitrateController, RecoversOnlyAfterCooldownAndTwoCleanWindows) {
  adaptive_bitrate::controller_t controller {adaptive_bitrate::make_envelope(2000, 10000, 12000, 60)};
  adaptive_bitrate::sample_t congested;
  congested.network_queue_frames = 2;
  EXPECT_EQ(complete_window(controller, congested).target_kbps, 8000U);

  EXPECT_EQ(complete_window(controller).state, adaptive_bitrate::congestion_state_e::recovering);
  EXPECT_EQ(complete_window(controller).state, adaptive_bitrate::congestion_state_e::recovering);
  EXPECT_FALSE(complete_window(controller).changed);
  const auto recovered {complete_window(controller)};
  EXPECT_TRUE(recovered.changed);
  EXPECT_EQ(recovered.target_kbps, 8400U);
  EXPECT_EQ(recovered.reason, "clean_recovery");
}

/**
 * @brief Verify reconnect and IDR events install a growth-only cooldown.
 */
TEST(AdaptiveBitrateController, HoldsGrowthAfterIdrOrReconnect) {
  adaptive_bitrate::controller_t controller {adaptive_bitrate::make_envelope(2000, 8000, 12000, 60)};
  adaptive_bitrate::sample_t reconnect;
  reconnect.idr_or_reconnect = true;
  EXPECT_EQ(complete_window(controller, reconnect).state, adaptive_bitrate::congestion_state_e::recovering);
  EXPECT_EQ(complete_window(controller).state, adaptive_bitrate::congestion_state_e::recovering);
  EXPECT_FALSE(complete_window(controller).changed);
  EXPECT_TRUE(complete_window(controller).changed);
}

/**
 * @brief Verify every congestion state has a stable diagnostics spelling.
 */
TEST(AdaptiveBitrateController, SerializesAllStates) {
  using adaptive_bitrate::congestion_state_e;
  EXPECT_EQ(adaptive_bitrate::to_string(congestion_state_e::unknown), "unknown");
  EXPECT_EQ(adaptive_bitrate::to_string(congestion_state_e::clean), "clean");
  EXPECT_EQ(adaptive_bitrate::to_string(congestion_state_e::warning), "warning");
  EXPECT_EQ(adaptive_bitrate::to_string(congestion_state_e::congested), "congested");
  EXPECT_EQ(adaptive_bitrate::to_string(congestion_state_e::recovering), "recovering");
}

/**
 * @brief Verify backend failures never request encoder recreation or repeated updates.
 */
TEST(AdaptiveBitrateBackend, RetainsStableStreamAndLearnsOnlyWhenSafe) {
  using adaptive_bitrate::backend_update_e;
  const auto applied {adaptive_bitrate::resolve_backend_update(backend_update_e::applied, 10000, 8000)};
  EXPECT_EQ(applied.active_kbps, 8000U);
  EXPECT_EQ(applied.learned_next_kbps, 8000U);
  EXPECT_FALSE(applied.retry);

  const auto unsupported {adaptive_bitrate::resolve_backend_update(backend_update_e::unsupported, 10000, 8000)};
  EXPECT_EQ(unsupported.active_kbps, 10000U);
  EXPECT_EQ(unsupported.learned_next_kbps, 8000U);
  EXPECT_FALSE(unsupported.retry);

  const auto failed {adaptive_bitrate::resolve_backend_update(backend_update_e::failed, 10000, 8000)};
  EXPECT_EQ(failed.active_kbps, 10000U);
  EXPECT_EQ(failed.learned_next_kbps, 10000U);
  EXPECT_FALSE(failed.retry);
}
