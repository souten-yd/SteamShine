/**
 * @file tests/unit/test_frame_pacing.cpp
 * @brief Tests for rational absolute-deadline video pacing.
 */
#include <chrono>
#include <deque>
#include <gtest/gtest.h>
#include <src/frame_pacing.h>
#include <src/platform/linux/pipewire_capture.h>
#include <vector>

namespace {
  using steady_clock_t = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  /**
   * @brief Convert a nanosecond offset into a deterministic test time point.
   *
   * @param offset Time elapsed from the default steady-clock epoch.
   * @return Deterministic steady-clock time point.
   */
  steady_clock_t::time_point at(const std::chrono::nanoseconds offset) {
    return steady_clock_t::time_point {offset};
  }

  /**
   * @brief Result of a deterministic producer/output simulation.
   */
  struct simulation_result_t {
    std::uint64_t unique {0};  ///< Source generations consumed in order.
    std::uint64_t duplicate {0};  ///< Deadlines without a pending source generation.
    std::uint64_t overflow {0};  ///< Source generations rejected by the bounded handoff.
  };

  /**
   * @brief Drive a bounded synthetic PipeWire producer against rational deadlines.
   *
   * @param arrivals Ordered source arrival offsets.
   * @param output_rate Rational client output rate.
   * @param duration Total simulated session duration.
   * @param capacity Source handoff capacity.
   * @return Unique, duplicate, and overflow counts.
   */
  simulation_result_t simulate_producer(
    const std::vector<std::chrono::nanoseconds> &arrivals,
    const frame_pacing::rate_t output_rate,
    const std::chrono::nanoseconds duration,
    const std::size_t capacity = 4
  ) {
    pipewire_capture::bounded_source_queue_t<std::uint64_t> pending {capacity};
    frame_pacing::deadline_pacer_t pacer {output_rate};
    pacer.reset(at(0ns));
    simulation_result_t result;
    std::size_t next_arrival {};

    while (pacer.next_deadline() <= at(duration)) {
      const auto deadline {pacer.next_deadline()};
      while (next_arrival < arrivals.size() && at(arrivals[next_arrival]) <= deadline) {
        if (!pending.push(next_arrival + 1)) {
          ++result.overflow;
        }
        ++next_arrival;
      }
      if (!pacer.poll(deadline).due) {
        ADD_FAILURE() << "Synthetic deadline was not due";
        break;
      }
      if (pending.pop()) {
        ++result.unique;
      } else {
        ++result.duplicate;
      }
    }
    return result;
  }
}  // namespace

/**
 * @brief Verify 60 FPS emits one deadline per exact rational period.
 */
TEST(FramePacingDeadline, SchedulesSixtyFpsFromAbsoluteEpoch) {
  frame_pacing::deadline_pacer_t pacer {{60, 1}};
  pacer.reset(at(0ns));

  EXPECT_FALSE(pacer.poll(at(16666665ns)).due);
  const auto first {pacer.poll(at(16666666ns))};
  ASSERT_TRUE(first.due);
  EXPECT_EQ(first.deadline_index, 1U);
  EXPECT_EQ(first.scheduled_time, at(16666666ns));
  EXPECT_EQ(first.missed_deadlines, 0U);

  const auto second {pacer.poll(at(33333333ns))};
  ASSERT_TRUE(second.due);
  EXPECT_EQ(second.deadline_index, 2U);
  EXPECT_EQ(second.scheduled_time, at(33333333ns));
}

/**
 * @brief Verify NTSC 59.94 pacing remains distinct from integer 60 FPS.
 */
TEST(FramePacingDeadline, DistinguishesNtscRateFromSixtyFps) {
  frame_pacing::deadline_pacer_t sixty {{60, 1}};
  frame_pacing::deadline_pacer_t ntsc {{60000, 1001}};
  sixty.reset(at(0ns));
  ntsc.reset(at(0ns));

  EXPECT_EQ(sixty.next_deadline(), at(16666666ns));
  EXPECT_EQ(ntsc.next_deadline(), at(16683333ns));

  for (std::uint64_t index {1}; index <= 60000; ++index) {
    const auto result {ntsc.poll(ntsc.next_deadline())};
    ASSERT_TRUE(result.due);
    ASSERT_EQ(result.deadline_index, index);
  }
  EXPECT_EQ(ntsc.next_deadline(), at(1001016683333ns));
}

/**
 * @brief Verify a late poll discards missed deadlines and never catches up in a burst.
 */
TEST(FramePacingDeadline, SkipsMissedDeadlinesWithoutBurst) {
  frame_pacing::deadline_pacer_t pacer {{60, 1}};
  pacer.reset(at(0ns));

  const auto late {pacer.poll(at(100ms))};
  ASSERT_TRUE(late.due);
  EXPECT_EQ(late.deadline_index, 6U);
  EXPECT_EQ(late.missed_deadlines, 5U);
  EXPECT_FALSE(pacer.poll(at(100ms)).due);
  EXPECT_EQ(pacer.next_deadline(), at(116666666ns));
}

/**
 * @brief Verify common high frame rates retain their expected absolute cadence.
 */
TEST(FramePacingDeadline, SupportsNinetyAndOneHundredTwentyFps) {
  frame_pacing::deadline_pacer_t ninety {{90, 1}};
  frame_pacing::deadline_pacer_t one_twenty {{120, 1}};
  ninety.reset(at(0ns));
  one_twenty.reset(at(0ns));

  EXPECT_EQ(ninety.next_deadline(), at(11111111ns));
  EXPECT_EQ(one_twenty.next_deadline(), at(8333333ns));
  EXPECT_EQ(ninety.poll(at(1s)).deadline_index, 90U);
  EXPECT_EQ(one_twenty.poll(at(1s)).deadline_index, 120U);
}

/**
 * @brief Verify an invalid rational rate remains inactive.
 */
TEST(FramePacingDeadline, RejectsInvalidRate) {
  frame_pacing::deadline_pacer_t zero_numerator {{0, 1}};
  frame_pacing::deadline_pacer_t zero_denominator {{60, 0}};

  EXPECT_FALSE(zero_numerator.rate().valid());
  EXPECT_FALSE(zero_denominator.rate().valid());
  EXPECT_EQ(zero_numerator.next_deadline(), steady_clock_t::time_point {});
  EXPECT_FALSE(zero_numerator.poll(at(1s)).due);
  EXPECT_FALSE(zero_denominator.poll(at(1s)).due);
}

/**
 * @brief Verify irregular low-rate unique frames remain ordered at 60 FPS deadlines.
 */
TEST(FramePacingDeadline, PreservesIrregularUniqueFrameOrder) {
  frame_pacing::deadline_pacer_t pacer {{60, 1}};
  pacer.reset(at(0ns));
  const std::vector<std::chrono::nanoseconds> arrivals {5ms, 26ms, 47ms, 70ms, 91ms};
  std::deque<std::size_t> pending;
  std::vector<std::size_t> encoded;
  std::size_t next_arrival {};

  for (auto now = 1ms; now <= 120ms; now += 1ms) {
    while (next_arrival < arrivals.size() && arrivals[next_arrival] <= now) {
      pending.push_back(next_arrival++);
    }
    if (pacer.poll(at(now)).due && !pending.empty()) {
      encoded.push_back(pending.front());
      pending.pop_front();
    }
  }

  EXPECT_EQ(encoded, (std::vector<std::size_t> {0, 1, 2, 3, 4}));
  EXPECT_TRUE(pending.empty());
}

/**
 * @brief Verify static content suppresses duplicate encodes until keepalive.
 */
TEST(FramePacingOutputPolicy, SuppressesStaticDuplicates) {
  frame_pacing::output_policy_t policy {500ms, 1s};
  policy.reset(at(0ms));

  EXPECT_TRUE(policy.should_encode(at(499ms), false));
  policy.record_output(at(499ms));
  EXPECT_TRUE(policy.static_mode(at(500ms)));
  EXPECT_FALSE(policy.should_encode(at(1498ms), false));
  EXPECT_TRUE(policy.should_encode(at(1499ms), false));
}

/**
 * @brief Verify a low-rate unique frame exits static mode without being lost.
 */
TEST(FramePacingOutputPolicy, UniqueFrameRestoresActiveOutput) {
  frame_pacing::output_policy_t policy {500ms, 1s};
  policy.reset(at(0ms));
  policy.record_output(at(500ms));
  ASSERT_TRUE(policy.static_mode(at(900ms)));

  policy.observe_unique(at(900ms));
  EXPECT_FALSE(policy.static_mode(at(900ms)));
  EXPECT_TRUE(policy.should_encode(at(900ms), false));
}

/**
 * @brief Verify IDR and reconnect requests bypass static keepalive suppression.
 */
TEST(FramePacingOutputPolicy, ForcedOutputBypassesStaticDelay) {
  frame_pacing::output_policy_t policy {500ms, 1s};
  policy.reset(at(0ms));
  policy.record_output(at(500ms));

  ASSERT_FALSE(policy.should_encode(at(600ms), false));
  EXPECT_TRUE(policy.should_encode(at(600ms), true));
}

/**
 * @brief Verify 60 seconds of 60 FPS source motion preserves every generation.
 */
TEST(SyntheticPipeWireProducer, PreservesContinuousSixtyFpsMotion) {
  std::vector<std::chrono::nanoseconds> arrivals;
  arrivals.reserve(3600);
  for (std::uint64_t index {1}; index <= 3600; ++index) {
    arrivals.push_back(std::chrono::nanoseconds {static_cast<std::int64_t>((static_cast<__int128>(index) * 1000000000) / 60)});
  }

  const auto result {simulate_producer(arrivals, {60, 1}, 60s)};
  EXPECT_EQ(result.unique, 3600U);
  EXPECT_EQ(result.duplicate, 0U);
  EXPECT_EQ(result.overflow, 0U);
}

/**
 * @brief Verify irregular 48 FPS and steady 30 FPS sources remain source ordered.
 */
TEST(SyntheticPipeWireProducer, PreservesLowerRateGameFrames) {
  std::vector<std::chrono::nanoseconds> irregular_48;
  std::chrono::nanoseconds timestamp {5ms};
  while (timestamp <= 1s) {
    irregular_48.push_back(timestamp);
    timestamp += irregular_48.size() % 2 == 0 ? 20ms : 22ms;
  }
  std::vector<std::chrono::nanoseconds> steady_30;
  for (std::uint64_t index {1}; index <= 30; ++index) {
    steady_30.push_back(std::chrono::nanoseconds {static_cast<std::int64_t>((static_cast<__int128>(index) * 1000000000) / 30)});
  }

  const auto irregular_result {simulate_producer(irregular_48, {60, 1}, 1s)};
  const auto steady_result {simulate_producer(steady_30, {60, 1}, 1s)};
  EXPECT_EQ(irregular_result.unique, irregular_48.size());
  EXPECT_EQ(irregular_result.overflow, 0U);
  EXPECT_EQ(steady_result.unique, 30U);
  EXPECT_EQ(steady_result.duplicate, 30U);
  EXPECT_EQ(steady_result.overflow, 0U);
}

/**
 * @brief Verify a slower consumer produces explicit bounded overflow.
 */
TEST(SyntheticPipeWireProducer, ReportsSlowConsumerOverflow) {
  std::vector<std::chrono::nanoseconds> arrivals;
  for (std::uint64_t index {1}; index <= 60; ++index) {
    arrivals.push_back(std::chrono::nanoseconds {static_cast<std::int64_t>((static_cast<__int128>(index) * 1000000000) / 60)});
  }

  const auto result {simulate_producer(arrivals, {30, 1}, 1s)};
  EXPECT_GT(result.overflow, 0U);
  EXPECT_LE(result.unique, 30U);
}

/**
 * @brief Verify repeated PTS requires affirmative no-damage evidence to drop.
 */
TEST(SyntheticPipeWireProducer, ClassifiesPtsDamageAndCorruption) {
  EXPECT_TRUE(pipewire_capture::classify_frame(10U, 10U, true, false).unique);
  EXPECT_TRUE(pipewire_capture::classify_frame(10U, 10U, std::nullopt, false).unique);
  EXPECT_FALSE(pipewire_capture::classify_frame(10U, 10U, false, false).unique);
  EXPECT_FALSE(pipewire_capture::classify_frame(10U, 11U, true, true).unique);
}

/**
 * @brief Verify motion-to-static-to-motion reduces only unchanged output.
 */
TEST(SyntheticPipeWireProducer, TransitionsThroughTenSecondsStatic) {
  frame_pacing::deadline_pacer_t pacer {{60, 1}};
  frame_pacing::output_policy_t policy {500ms, 1s};
  pacer.reset(at(0ns));
  policy.reset(at(0ns));
  std::uint64_t unique_outputs {};
  std::uint64_t duplicate_outputs {};

  while (pacer.next_deadline() <= at(12s)) {
    const auto deadline {pacer.next_deadline()};
    const bool unique {deadline <= at(1s) || deadline > at(11s)};
    if (unique) {
      policy.observe_unique(deadline);
    }
    ASSERT_TRUE(pacer.poll(deadline).due);
    if (!policy.should_encode(deadline, false)) {
      continue;
    }
    policy.record_output(deadline);
    unique ? ++unique_outputs : ++duplicate_outputs;
  }

  EXPECT_EQ(unique_outputs, 120U);
  EXPECT_GE(duplicate_outputs, 9U);
  EXPECT_LT(duplicate_outputs, 45U);
  EXPECT_FALSE(policy.static_mode(at(12s)));
}
