/**
 * @file src/latency_diagnostics.h
 * @brief Fixed-memory lock-free latency sampling for streaming diagnostics.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace latency_diagnostics {
  /**
   * @brief Aggregate latency statistics expressed in milliseconds.
   */
  struct statistics_t {
    std::uint64_t count {0};  ///< Total samples recorded since the last reset.
    std::size_t window_count {0};  ///< Samples represented by the percentile window.
    double average_ms {0.0};  ///< Arithmetic mean across all recorded samples.
    double p50_ms {0.0};  ///< Median of the fixed recent-sample window.
    double p95_ms {0.0};  ///< 95th percentile of the fixed recent-sample window.
    double p99_ms {0.0};  ///< 99th percentile of the fixed recent-sample window.
    double max_ms {0.0};  ///< Greatest sample recorded since the last reset.
  };

  /**
   * @brief Fixed-size atomic ring retaining recent monotonic latency samples.
   *
   * Writers perform only relaxed/release atomic operations and never allocate,
   * lock, or write to disk. Readers copy the bounded ring before sorting, so
   * percentile work occurs only when a status snapshot or final report is
   * requested.
   *
   * @tparam Capacity Number of recent samples retained for percentiles.
   */
  template<std::size_t Capacity = 512>
  class fixed_ring_t {
  public:
    static_assert(Capacity > 0, "A latency ring must retain at least one sample");

    /**
     * @brief Clear counters and retained samples for a new stream.
     */
    void reset() {
      write_count_.store(0, std::memory_order_relaxed);
      total_microseconds_.store(0, std::memory_order_relaxed);
      maximum_microseconds_.store(0, std::memory_order_relaxed);
      for (auto &sample : samples_) {
        sample.store(0, std::memory_order_relaxed);
      }
    }

    /**
     * @brief Record one nonnegative monotonic duration without blocking.
     *
     * @param duration Duration measured between adjacent pipeline stages.
     */
    template<class Rep, class Period>
    void record(const std::chrono::duration<Rep, Period> duration) {
      const auto signed_microseconds {std::chrono::duration_cast<std::chrono::microseconds>(duration).count()};
      const auto microseconds {static_cast<std::uint64_t>(std::max<std::int64_t>(signed_microseconds, 0))};
      const auto index {write_count_.fetch_add(1, std::memory_order_relaxed)};
      samples_[index % Capacity].store(microseconds, std::memory_order_release);
      total_microseconds_.fetch_add(microseconds, std::memory_order_relaxed);
      auto maximum {maximum_microseconds_.load(std::memory_order_relaxed)};
      while (maximum < microseconds && !maximum_microseconds_.compare_exchange_weak(maximum, microseconds, std::memory_order_relaxed)) {
      }
    }

    /**
     * @brief Calculate bounded aggregate and percentile statistics.
     *
     * @return Snapshot safe for status serialization.
     */
    statistics_t snapshot() const {
      const auto count {write_count_.load(std::memory_order_acquire)};
      const auto window_count {static_cast<std::size_t>(std::min<std::uint64_t>(count, Capacity))};
      std::array<std::uint64_t, Capacity> window {};
      const auto first {count > Capacity ? count - Capacity : 0};
      for (std::size_t index {}; index < window_count; ++index) {
        window[index] = samples_[(first + index) % Capacity].load(std::memory_order_acquire);
      }
      for (std::size_t index {1}; index < window_count; ++index) {
        const auto value {window[index]};
        auto insertion {index};
        while (insertion > 0 && value < window[insertion - 1]) {
          window[insertion] = window[insertion - 1];
          --insertion;
        }
        window[insertion] = value;
      }

      const auto percentile {[&window, window_count](const std::size_t numerator) {
        if (window_count == 0) {
          return std::uint64_t {0};
        }
        const auto index {std::min(window_count - 1, ((window_count - 1) * numerator + 99) / 100)};
        return window[index];
      }};
      return {
        .count = count,
        .window_count = window_count,
        .average_ms = count == 0 ? 0.0 : static_cast<double>(total_microseconds_.load(std::memory_order_relaxed)) / static_cast<double>(count) / 1000.0,
        .p50_ms = static_cast<double>(percentile(50)) / 1000.0,
        .p95_ms = static_cast<double>(percentile(95)) / 1000.0,
        .p99_ms = static_cast<double>(percentile(99)) / 1000.0,
        .max_ms = static_cast<double>(maximum_microseconds_.load(std::memory_order_relaxed)) / 1000.0,
      };
    }

  private:
    std::array<std::atomic_uint64_t, Capacity> samples_ {};  ///< Recent duration samples in microseconds.
    std::atomic_uint64_t write_count_ {0};  ///< Total samples and next ring position.
    std::atomic_uint64_t total_microseconds_ {0};  ///< Total duration used for the session average.
    std::atomic_uint64_t maximum_microseconds_ {0};  ///< Session maximum duration.
  };
}  // namespace latency_diagnostics
