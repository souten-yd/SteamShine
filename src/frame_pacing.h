/**
 * @file src/frame_pacing.h
 * @brief Rational absolute-deadline scheduling for duplicate video output.
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
   * @brief Result of polling one absolute duplicate deadline.
   */
  struct poll_result_t {
    bool due {false};  ///< Whether one output action is due now.
    std::uint64_t deadline_index {0};  ///< Absolute one-based deadline selected for this action.
    std::uint64_t missed_deadlines {0};  ///< Earlier deadlines discarded to avoid a catch-up burst.
    std::chrono::steady_clock::time_point scheduled_time {};  ///< Exact selected deadline on the steady clock.
  };

  /**
   * @brief Drift-free rational duplicate scheduler emitting at most one action per poll.
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
     * @brief Re-anchor duplicate pacing after an immediate output.
     *
     * A unique source frame or forced recovery frame is emitted immediately
     * instead of waiting for an independent duplicate deadline. Re-anchoring
     * places the next duplicate one full rational period later and prevents a
     * stale deadline from creating a back-to-back output burst.
     *
     * @param output_time Monotonic time at which the immediate frame is emitted.
     */
    void record_immediate_output(std::chrono::steady_clock::time_point output_time);

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

  /**
   * @brief Conservative active/static output policy independent of capture APIs.
   *
   * Active content emits on every requested deadline. After a bounded interval
   * without a unique source generation, unchanged content emits only periodic
   * keepalives. A new unique generation or forced IDR immediately restores an
   * eligible output without inventing a captured frame.
   */
  class output_policy_t {
  public:
    /**
     * @brief Construct an output policy with explicit bounded intervals.
     *
     * @param static_after Inactivity interval before entering static mode.
     * @param keepalive_interval Minimum static interval between duplicate encodes.
     */
    output_policy_t(std::chrono::steady_clock::duration static_after, std::chrono::steady_clock::duration keepalive_interval);

    /**
     * @brief Start or restart policy accounting for a stream session.
     *
     * @param now Monotonic session start time.
     */
    void reset(std::chrono::steady_clock::time_point now);

    /**
     * @brief Record consumption of a new unique source generation.
     *
     * @param now Monotonic time at which the generation became available to output.
     */
    void observe_unique(std::chrono::steady_clock::time_point now);

    /**
     * @brief Decide whether the current deadline should encode.
     *
     * @param now Current monotonic time.
     * @param force_immediate Whether an IDR or reconnect request must bypass static suppression.
     * @return True for active CFR, a due static keepalive, or a forced encode.
     */
    [[nodiscard]] bool should_encode(std::chrono::steady_clock::time_point now, bool force_immediate) const;

    /**
     * @brief Record completion of an output decision.
     *
     * @param now Monotonic time of the encoder submission.
     */
    void record_output(std::chrono::steady_clock::time_point now);

    /**
     * @brief Report whether the source is currently treated as static.
     *
     * @param now Current monotonic time.
     * @return True after the configured unique-source inactivity interval.
     */
    [[nodiscard]] bool static_mode(std::chrono::steady_clock::time_point now) const;

  private:
    std::chrono::steady_clock::duration static_after_;  ///< Unique-frame inactivity required for static mode.
    std::chrono::steady_clock::duration keepalive_interval_;  ///< Minimum interval between static outputs.
    std::chrono::steady_clock::time_point last_unique_ {};  ///< Most recent unique generation observation.
    std::chrono::steady_clock::time_point last_output_ {};  ///< Most recent encoder submission.
  };
}  // namespace frame_pacing
