/**
 * @file src/platform/linux/gamescope_presenter.h
 * @brief Low-latency frame handoff primitives for local Gamescope presentation.
 */
#pragma once

#include "pipewire_capture.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace gamescope_presenter {
  /**
   * @brief One zero-copy DMA-BUF frame held by the local PipeWire consumer.
   *
   * File descriptors are borrowed from PipeWire and remain valid until
   * @c release returns the corresponding PipeWire buffer. A renderer must not
   * close or CPU-map them.
   */
  struct dma_buf_frame_t {
    uint64_t sequence {0};  ///< Producer frame sequence.
    uint32_t width {0};  ///< Frame width in pixels.
    uint32_t height {0};  ///< Frame height in pixels.
    uint32_t fourcc {0};  ///< DRM fourcc pixel format.
    uint64_t modifier {0};  ///< DRM format modifier.
    std::array<int, 4> fds {{-1, -1, -1, -1}};  ///< Borrowed DMA-BUF plane FDs.
    std::array<uint32_t, 4> pitches {};  ///< DMA-BUF plane pitches in bytes.
    std::array<uint32_t, 4> offsets {};  ///< DMA-BUF plane byte offsets.
    std::function<void()> release;  ///< Returns the buffer to PipeWire exactly once.
  };

  /**
   * @brief Callback receiving a local-presenter DMA-BUF frame.
   */
  using dma_buf_frame_callback_t = std::function<void(dma_buf_frame_t)>;

  /**
   * @brief Independent PipeWire consumer for the local presentation path.
   *
   * This consumer owns exactly one PipeWire core FD supplied in the descriptor
   * and targets the verified object serial. It accepts DMA-BUF only and does
   * not CPU-map, encode, decode, or share remote-capture state.
   */
  class local_pipewire_consumer_t {
  public:
    /**
     * @brief Construct an inactive local PipeWire consumer.
     */
    local_pipewire_consumer_t();

    /**
     * @brief Stop the consumer and release its PipeWire connection.
     */
    ~local_pipewire_consumer_t();

    local_pipewire_consumer_t(const local_pipewire_consumer_t &) = delete;
    local_pipewire_consumer_t &operator=(const local_pipewire_consumer_t &) = delete;

    /**
     * @brief Start one independent DMA-BUF-only PipeWire stream.
     *
     * The descriptor's FD is consumed on every call, including a failed
     * initialization, and must not be reused by another consumer.
     *
     * @param descriptor Verified source and dedicated PipeWire connection.
     * @param callback Receives latest source buffers without rendering them.
     * @param error Human-readable startup failure reason.
     * @return True only when PipeWire accepts the DMA-BUF-only stream.
     */
    bool start(pipewire_capture::stream_descriptor_t descriptor, dma_buf_frame_callback_t callback, std::string &error);

    /**
     * @brief Stop without affecting the remote capture consumer or Gamescope.
     */
    void stop();

    /**
     * @brief Report whether the dedicated local consumer is connected.
     *
     * @return True while its PipeWire stream remains active.
     */
    bool active() const;

  private:
    struct impl_t;  ///< PipeWire-private implementation defined only on Linux builds.
    std::unique_ptr<impl_t> impl_;  ///< Owned consumer implementation.
  };

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
