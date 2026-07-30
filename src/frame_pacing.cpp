/**
 * @file src/frame_pacing.cpp
 * @brief Rational absolute-deadline scheduling for duplicate video output.
 */
#include "frame_pacing.h"

#include <algorithm>
#include <limits>

namespace frame_pacing {
  namespace {
    constexpr std::int64_t nanoseconds_per_second {1000000000};  ///< Nanoseconds in one second.

    /**
     * @brief Clamp a wide integer to the representation used by nanoseconds.
     *
     * @param value Nonnegative wide duration value.
     * @return Value clamped to a signed 64-bit duration representation.
     */
    std::int64_t clamp_nanoseconds(const __int128 value) {
      return static_cast<std::int64_t>(std::min<__int128>(value, std::numeric_limits<std::int64_t>::max()));
    }
  }  // namespace

  bool rate_t::valid() const {
    return numerator != 0 && denominator != 0;
  }

  deadline_pacer_t::deadline_pacer_t(const rate_t rate):
      rate_ {rate} {
  }

  void deadline_pacer_t::reset(const std::chrono::steady_clock::time_point epoch) {
    epoch_ = epoch;
    next_index_ = 1;
  }

  void deadline_pacer_t::record_immediate_output(const std::chrono::steady_clock::time_point output_time) {
    reset(output_time);
  }

  poll_result_t deadline_pacer_t::poll(const std::chrono::steady_clock::time_point now) {
    if (!rate_.valid() || now < next_deadline()) {
      return {};
    }

    const auto elapsed {std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count()};
    const auto elapsed_deadlines {
      static_cast<std::uint64_t>((static_cast<__int128>(elapsed) * rate_.numerator) / (static_cast<__int128>(nanoseconds_per_second) * rate_.denominator))
    };
    const auto selected_index {std::max(next_index_, elapsed_deadlines)};
    const auto missed {selected_index - next_index_};
    next_index_ = selected_index + 1;

    return {
      .due = true,
      .deadline_index = selected_index,
      .missed_deadlines = missed,
      .scheduled_time = deadline_for(selected_index),
    };
  }

  std::chrono::steady_clock::time_point deadline_pacer_t::next_deadline() const {
    if (!rate_.valid()) {
      return {};
    }
    return deadline_for(next_index_);
  }

  rate_t deadline_pacer_t::rate() const {
    return rate_;
  }

  std::chrono::steady_clock::time_point deadline_pacer_t::deadline_for(const std::uint64_t index) const {
    const auto numerator {static_cast<__int128>(index) * nanoseconds_per_second * rate_.denominator};
    const auto offset {clamp_nanoseconds(numerator / rate_.numerator)};
    return epoch_ + std::chrono::nanoseconds {offset};
  }

  output_policy_t::output_policy_t(const std::chrono::steady_clock::duration static_after, const std::chrono::steady_clock::duration keepalive_interval):
      static_after_ {std::max(static_after, std::chrono::steady_clock::duration::zero())},
      keepalive_interval_ {std::max(keepalive_interval, std::chrono::steady_clock::duration::zero())} {
  }

  void output_policy_t::reset(const std::chrono::steady_clock::time_point now) {
    last_unique_ = now;
    last_output_ = now;
    observed_unique_ = false;
  }

  void output_policy_t::observe_unique(const std::chrono::steady_clock::time_point now) {
    last_unique_ = now;
    observed_unique_ = true;
  }

  bool output_policy_t::should_encode(const std::chrono::steady_clock::time_point now, const bool force_immediate) const {
    return force_immediate || !static_mode(now) || now - last_output_ >= keepalive_interval_;
  }

  void output_policy_t::record_output(const std::chrono::steady_clock::time_point now) {
    last_output_ = now;
  }

  bool output_policy_t::static_mode(const std::chrono::steady_clock::time_point now) const {
    // Keep emitting startup dummy frames at the active cadence until the
    // producer supplies a real frame. A one-second pre-frame gap can make a
    // Moonlight client classify an otherwise healthy stream as disconnected.
    return observed_unique_ && now - last_unique_ >= static_after_;
  }
}  // namespace frame_pacing
