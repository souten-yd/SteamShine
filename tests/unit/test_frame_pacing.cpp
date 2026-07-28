/**
 * @file tests/unit/test_frame_pacing.cpp
 * @brief Tests for rational absolute-deadline video pacing.
 */
#include <chrono>
#include <deque>
#include <gtest/gtest.h>
#include <src/frame_pacing.h>
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
