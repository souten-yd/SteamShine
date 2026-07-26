/**
 * @file src/platform/linux/gamescope_presenter.h
 * @brief Low-latency frame handoff primitives for local Gamescope presentation.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace gamescope_presenter {
  /**
   * @brief One acquired source frame and its mandatory release operation.
   */
  struct frame_t {
    uint64_t sequence {0};  ///< Monotonically increasing source-frame sequence.
    std::function<void()> release;  ///< Returns the source buffer when dropped or unused.
  };

  /**
   * @brief Result of publishing a frame into the latest-frame queue.
   */
  struct publish_result_t {
    bool replaced_pending_frame {false};  ///< Whether an older pending frame was released.
  };

  /**
   * @brief A one-frame, latest-frame-wins handoff between capture and presentation.
   *
   * A producer never waits for the consumer. Replacing a pending frame invokes
   * its release callback outside the queue lock, so PipeWire buffer release
   * cannot block remote capture or encode work.
   */
  class latest_frame_queue_t {
  public:
    /**
     * @brief Release a pending source frame when the queue is destroyed.
     */
    ~latest_frame_queue_t();

    latest_frame_queue_t() = default;
    latest_frame_queue_t(const latest_frame_queue_t &) = delete;
    latest_frame_queue_t &operator=(const latest_frame_queue_t &) = delete;

    /**
     * @brief Publish a new frame without waiting for the presenter.
     *
     * @param frame Newly acquired source frame.
     * @return Whether the preceding pending frame was released.
     */
    publish_result_t publish(frame_t frame);

    /**
     * @brief Take the newest pending frame for local presentation.
     *
     * @return The frame, or std::nullopt when no frame is pending.
     */
    std::optional<frame_t> take_latest();

    /**
     * @brief Return the number of pending frames, always zero or one.
     *
     * @return Current queue occupancy.
     */
    uint32_t pending_count() const;

  private:
    mutable std::mutex mutex_;  ///< Synchronizes producer and presenter access.
    std::optional<frame_t> pending_;  ///< The single newest frame awaiting presentation.
  };
}  // namespace gamescope_presenter
