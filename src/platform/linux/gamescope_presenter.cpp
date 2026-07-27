/**
 * @file src/platform/linux/gamescope_presenter.cpp
 * @brief Low-latency frame handoff primitives for local Gamescope presentation.
 */
#include "gamescope_presenter.h"

#include <mutex>
#include <utility>

namespace gamescope_presenter {
  namespace {
    /**
     * @brief Invoke a source-frame release callback when one is present.
     *
     * @param frame Source frame whose buffer must be returned.
     */
    void release_frame(frame_t &frame) {
      if (frame.release) {
        frame.release();
      }
    }
  }  // namespace

  latest_frame_queue_t::~latest_frame_queue_t() {
    std::optional<frame_t> pending;
    {
      std::scoped_lock lock {mutex_};
      pending = std::move(pending_);
    }
    if (pending) {
      release_frame(*pending);
    }
  }

  publish_result_t latest_frame_queue_t::publish(frame_t frame) {
    std::optional<frame_t> replaced;
    {
      std::scoped_lock lock {mutex_};
      replaced = std::move(pending_);
      pending_ = std::move(frame);
    }
    if (replaced) {
      release_frame(*replaced);
    }
    return {.replaced_pending_frame = replaced.has_value()};
  }

  std::optional<frame_t> latest_frame_queue_t::take_latest() {
    std::scoped_lock lock {mutex_};
    return std::exchange(pending_, std::nullopt);
  }

  uint32_t latest_frame_queue_t::pending_count() const {
    std::scoped_lock lock {mutex_};
    return pending_ ? 1U : 0U;
  }
}  // namespace gamescope_presenter
