/**
 * @file src/adaptive_bitrate.h
 * @brief Bounded video bitrate envelope and deterministic congestion controller.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace adaptive_bitrate {
  /**
   * @brief Congestion classification for the most recent decision window.
   */
  enum class congestion_state_e {
    unknown,  ///< Not enough observations are available for a decision.
    clean,  ///< The network has remained below every congestion threshold.
    warning,  ///< Isolated loss or moderate latency caused a cautious reduction.
    congested,  ///< Persistent loss, latency, or queue pressure caused a strong reduction.
    recovering,  ///< A clean window is being held during direction-change cooldown.
  };

  /**
   * @brief Convert a congestion state into its stable diagnostics value.
   *
   * @param state Congestion state to serialize.
   * @return Stable lower-case diagnostics value.
   */
  constexpr std::string_view to_string(const congestion_state_e state) {
    switch (state) {
      case congestion_state_e::clean:
        return "clean";
      case congestion_state_e::warning:
        return "warning";
      case congestion_state_e::congested:
        return "congested";
      case congestion_state_e::recovering:
        return "recovering";
      default:
        return "unknown";
    }
  }

  /**
   * @brief Video-only VBR bounds in kilobits per second.
   */
  struct bitrate_envelope_t {
    std::uint32_t minimum_kbps {1000};  ///< Lowest permitted video target.
    std::uint32_t initial_kbps {1000};  ///< Target selected at session start.
    std::uint32_t target_kbps {1000};  ///< Current controller target.
    std::uint32_t maximum_kbps {1000};  ///< Effective client/administrator/backend ceiling.
    std::uint32_t peak_kbps {1000};  ///< Encoder peak-rate ceiling.
    std::uint32_t vbv_kbits {17};  ///< Encoder VBV capacity in kilobits.
  };

  /**
   * @brief Normalize a bitrate envelope without trusting unbounded inputs.
   *
   * @param minimum_kbps Requested lower video bound.
   * @param initial_kbps Requested session-start target.
   * @param maximum_kbps Effective upper video bound.
   * @param framerate Encoder output frames per second.
   * @return Ordered and clamped bitrate envelope.
   */
  constexpr bitrate_envelope_t make_envelope(
    std::uint32_t minimum_kbps,
    std::uint32_t initial_kbps,
    std::uint32_t maximum_kbps,
    std::uint32_t framerate
  ) {
    maximum_kbps = std::max(maximum_kbps, 1U);
    minimum_kbps = std::clamp(minimum_kbps, 1U, maximum_kbps);
    initial_kbps = std::clamp(initial_kbps, minimum_kbps, maximum_kbps);
    framerate = std::max(framerate, 1U);
    return {
      minimum_kbps,
      initial_kbps,
      initial_kbps,
      maximum_kbps,
      maximum_kbps,
      std::max(initial_kbps / framerate, 1U),
    };
  }

  /**
   * @brief One fixed-memory network observation supplied to the controller.
   */
  struct sample_t {
    std::uint32_t elapsed_ms {500};  ///< Time represented by this sample.
    std::uint32_t lost_packets {0};  ///< Client-reported packet loss in the interval.
    std::uint32_t rtt_ms {0};  ///< Round-trip latency when available, otherwise zero.
    std::uint32_t network_queue_frames {0};  ///< Ordered encoded frames waiting for send.
    std::uint64_t socket_outq_bytes {0};  ///< Bytes waiting in the kernel video socket.
    bool idr_or_reconnect {false};  ///< Whether rate growth must cool down after a burst event.
  };

  /**
   * @brief Result of one controller observation.
   */
  struct decision_t {
    congestion_state_e state {congestion_state_e::unknown};  ///< Current congestion classification.
    std::uint32_t target_kbps {0};  ///< Bounded video target after the decision.
    bool changed {false};  ///< Whether the target changed at this boundary.
    std::string_view reason {"collecting"};  ///< Stable reason for diagnostics.
  };

  /**
   * @brief Typed result returned by an encoder runtime-rate adapter.
   */
  enum class backend_update_e {
    applied,  ///< The backend accepted the new target at a frame boundary.
    unsupported,  ///< The backend cannot update rate control without recreation.
    failed,  ///< The backend advertised support but rejected this update.
  };

  /**
   * @brief Stable stream and next-session targets after one backend response.
   */
  struct update_outcome_t {
    std::uint32_t active_kbps {0};  ///< Target actually active in the current encoder.
    std::uint32_t learned_next_kbps {0};  ///< Safe target retained for the next session.
    bool retry {false};  ///< Whether the current stream should retry or recreate the encoder.
  };

  /**
   * @brief Resolve a backend response without starting a recreation or retry loop.
   *
   * @param result Typed backend response.
   * @param active_kbps Target active before the request.
   * @param requested_kbps Newly requested bounded target.
   * @return Current and learned targets; retry is always false by policy.
   */
  constexpr update_outcome_t resolve_backend_update(
    const backend_update_e result,
    const std::uint32_t active_kbps,
    const std::uint32_t requested_kbps
  ) {
    switch (result) {
      case backend_update_e::applied:
        return {requested_kbps, requested_kbps, false};
      case backend_update_e::unsupported:
        return {active_kbps, requested_kbps, false};
      case backend_update_e::failed:
      default:
        return {active_kbps, active_kbps, false};
    }
  }

  /**
   * @brief Stateful 500 ms sample and 2 s decision-window bitrate controller.
   */
  class controller_t {
  public:
    /**
     * @brief Construct a controller from an already normalized envelope.
     *
     * @param envelope Video target limits for the active stream.
     */
    explicit constexpr controller_t(const bitrate_envelope_t envelope):
        envelope_ {envelope} {
    }

    /**
     * @brief Add one observation and possibly change the target at a 2 s boundary.
     *
     * Persistent congestion reduces 20%, isolated loss reduces 10%, and two
     * clean windows permit a 5% increase. Reductions install a cooldown so the
     * controller cannot immediately reverse direction.
     *
     * @param sample Bounded network observation.
     * @return Current state, target, change flag, and stable reason.
     */
    constexpr decision_t observe(const sample_t &sample) {
      elapsed_ms_ += std::min(sample.elapsed_ms, 2000U);
      lost_packets_ += sample.lost_packets;
      maximum_rtt_ms_ = std::max(maximum_rtt_ms_, sample.rtt_ms);
      maximum_queue_frames_ = std::max(maximum_queue_frames_, sample.network_queue_frames);
      maximum_socket_outq_bytes_ = std::max(maximum_socket_outq_bytes_, sample.socket_outq_bytes);
      if (sample.idr_or_reconnect) {
        cooldown_windows_ = std::max(cooldown_windows_, 2U);
      }

      if (elapsed_ms_ < 2000) {
        return {state_, envelope_.target_kbps, false, "collecting"};
      }

      const auto one_second_video_bytes {static_cast<std::uint64_t>(envelope_.target_kbps) * 125U};
      const bool persistent_congestion {
        lost_packets_ >= 4 || maximum_rtt_ms_ >= 200 || maximum_queue_frames_ >= 2 ||
        maximum_socket_outq_bytes_ >= one_second_video_bytes
      };
      const bool isolated_pressure {
        lost_packets_ > 0 || maximum_rtt_ms_ >= 120 ||
        maximum_socket_outq_bytes_ >= one_second_video_bytes / 2
      };
      reset_window();

      if (persistent_congestion) {
        clean_windows_ = 0;
        cooldown_windows_ = 2;
        state_ = congestion_state_e::congested;
        return reduce(20, "persistent_congestion");
      }
      if (isolated_pressure) {
        clean_windows_ = 0;
        cooldown_windows_ = std::max(cooldown_windows_, 1U);
        state_ = congestion_state_e::warning;
        return reduce(10, "isolated_network_pressure");
      }
      if (cooldown_windows_ > 0) {
        --cooldown_windows_;
        clean_windows_ = 0;
        state_ = congestion_state_e::recovering;
        return {state_, envelope_.target_kbps, false, "cooldown"};
      }

      state_ = congestion_state_e::clean;
      if (++clean_windows_ < 2 || envelope_.target_kbps >= envelope_.maximum_kbps) {
        return {state_, envelope_.target_kbps, false, "clean_hold"};
      }
      clean_windows_ = 0;
      const auto increase {std::max(envelope_.target_kbps / 20U, 1U)};
      const auto previous {envelope_.target_kbps};
      envelope_.target_kbps = std::min(envelope_.maximum_kbps, envelope_.target_kbps + increase);
      envelope_.peak_kbps = envelope_.target_kbps;
      envelope_.vbv_kbits = std::max<std::uint32_t>(
        static_cast<std::uint64_t>(envelope_.vbv_kbits) * envelope_.target_kbps / previous,
        1U
      );
      return {state_, envelope_.target_kbps, envelope_.target_kbps != previous, "clean_recovery"};
    }

    /**
     * @brief Return the current bounded envelope.
     *
     * @return Current envelope and target.
     */
    constexpr const bitrate_envelope_t &envelope() const {
      return envelope_;
    }

  private:
    /**
     * @brief Reduce the target by a percentage while honoring the minimum.
     *
     * @param percentage Reduction percentage.
     * @param reason Stable decision reason.
     * @return Result containing the reduced target.
     */
    constexpr decision_t reduce(const std::uint32_t percentage, const std::string_view reason) {
      const auto previous {envelope_.target_kbps};
      const auto reduction {std::max(previous * percentage / 100U, 1U)};
      envelope_.target_kbps = std::max(envelope_.minimum_kbps, previous - reduction);
      envelope_.peak_kbps = envelope_.target_kbps;
      envelope_.vbv_kbits = std::max<std::uint32_t>(
        static_cast<std::uint64_t>(envelope_.vbv_kbits) * envelope_.target_kbps / previous,
        1U
      );
      return {state_, envelope_.target_kbps, envelope_.target_kbps != previous, reason};
    }

    /**
     * @brief Clear bounded aggregates after one decision window.
     */
    constexpr void reset_window() {
      elapsed_ms_ = 0;
      lost_packets_ = 0;
      maximum_rtt_ms_ = 0;
      maximum_queue_frames_ = 0;
      maximum_socket_outq_bytes_ = 0;
    }

    bitrate_envelope_t envelope_;  ///< Current bounded VBR state.
    congestion_state_e state_ {congestion_state_e::unknown};  ///< Latest completed-window state.
    std::uint32_t elapsed_ms_ {0};  ///< Accumulated sample duration.
    std::uint32_t lost_packets_ {0};  ///< Accumulated packet loss.
    std::uint32_t maximum_rtt_ms_ {0};  ///< Highest RTT in the current window.
    std::uint32_t maximum_queue_frames_ {0};  ///< Highest sender queue depth in the current window.
    std::uint64_t maximum_socket_outq_bytes_ {0};  ///< Highest kernel queue size in the current window.
    std::uint32_t clean_windows_ {0};  ///< Consecutive clean windows eligible for growth.
    std::uint32_t cooldown_windows_ {0};  ///< Clean windows required before growth can resume.
  };
}  // namespace adaptive_bitrate
