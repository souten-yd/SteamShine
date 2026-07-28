/**
 * @file src/frame_pacing.h
 * @brief Rational absolute-deadline scheduling for video output.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace frame_pacing {
  /**
   * @brief Positive rational frame rate expressed as frames per second.
   */
  struct rate_t {
    std::uint32_t numerator {0};  ///< Frames produced during one denominator interval.
    std::uint32_t denominator {1};  ///< Seconds represented by the rate denominator.

    /**
     * @brief Test whether both rational components form a positive frame rate.
     *
     * @return True when neither component is zero.
     */
    [[nodiscard]] bool valid() const;
  };

  /**
   * @brief Result of polling one absolute output deadline.
   */
  struct poll_result_t {
    bool due {false};  ///< Whether one output action is due now.
    std::uint64_t deadline_index {0};  ///< Absolute one-based deadline selected for this action.
    std::uint64_t missed_deadlines {0};  ///< Earlier deadlines discarded to avoid a catch-up burst.
    std::chrono::steady_clock::time_point scheduled_time {};  ///< Exact selected deadline on the steady clock.
  };

  /**
   * @brief Drift-free rational scheduler that emits at most one action per poll.
   *
   * Every deadline is calculated from the original epoch and a one-based index.
   * A late poll skips directly to the newest elapsed deadline, reports the
   * skipped count, and schedules the following future deadline without bursting.
   */
  class deadline_pacer_t {
  public:
    /**
     * @brief Construct an inactive pacer for a rational frame rate.
     *
     * @param rate Requested output rate. An invalid rate leaves the pacer inactive.
     */
    explicit deadline_pacer_t(rate_t rate);

    /**
     * @brief Start or restart the schedule from a monotonic epoch.
     *
     * The first output deadline is one frame period after @p epoch.
     *
     * @param epoch Absolute steady-clock origin for all subsequent deadlines.
     */
    void reset(std::chrono::steady_clock::time_point epoch);

    /**
     * @brief Poll the schedule without creating catch-up output bursts.
     *
     * @param now Current steady-clock time.
     * @return One due deadline and skipped count, or a result with `due == false`.
     */
    [[nodiscard]] poll_result_t poll(std::chrono::steady_clock::time_point now);

    /**
     * @brief Return the next absolute deadline without advancing the schedule.
     *
     * @return Next deadline, or a default time point for an invalid rate.
     */
    [[nodiscard]] std::chrono::steady_clock::time_point next_deadline() const;

    /**
     * @brief Return the configured rational output rate.
     *
     * @return Rate supplied during construction.
     */
    [[nodiscard]] rate_t rate() const;

  private:
    /**
     * @brief Calculate one indexed deadline directly from the schedule epoch.
     *
     * @param index One-based output deadline index.
     * @return Absolute steady-clock deadline.
     */
    [[nodiscard]] std::chrono::steady_clock::time_point deadline_for(std::uint64_t index) const;

    rate_t rate_;  ///< Requested rational output rate.
    std::chrono::steady_clock::time_point epoch_ {};  ///< Immutable origin of the current schedule.
    std::uint64_t next_index_ {1};  ///< One-based index of the next expected deadline.
  };
}  // namespace frame_pacing
