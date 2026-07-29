/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pipewire_capture {
  /**
   * @brief Small ordered source queue with explicit overflow and fixed capacity.
   *
   * @tparam T Source item retained until the capture consumer accepts it.
   */
  template<typename T>
  class bounded_source_queue_t {
  public:
    /**
     * @brief Construct an empty queue with a fixed maximum depth.
     *
     * @param capacity Maximum number of retained source items.
     */
    explicit bounded_source_queue_t(const std::size_t capacity):
        capacity_ {capacity} {
    }

    /**
     * @brief Append an item without replacing an older generation.
     *
     * @param value Source item to append.
     * @return True when retained, or false when the bounded queue is full.
     */
    bool push(T value) {
      if (items_.size() >= capacity_) {
        return false;
      }
      items_.push_back(std::move(value));
      return true;
    }

    /**
     * @brief Remove the oldest retained source item.
     *
     * @return Oldest item, or no value when empty.
     */
    std::optional<T> pop() {
      if (items_.empty()) {
        return std::nullopt;
      }
      T value {std::move(items_.front())};
      items_.pop_front();
      return value;
    }

    /**
     * @brief Visit and remove every retained item in source order.
     *
     * @tparam Function Callable accepting one moved source item.
     * @param function Callable invoked once for each retained item.
     */
    template<typename Function>
    void drain(Function &&function) {
      while (auto value = pop()) {
        function(std::move(*value));
      }
    }

    /**
     * @brief Discard every retained item.
     */
    void clear() {
      items_.clear();
    }

    /**
     * @brief Report whether no source item is pending.
     *
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const {
      return items_.empty();
    }

    /**
     * @brief Return current bounded queue occupancy.
     *
     * @return Number of retained source items.
     */
    [[nodiscard]] std::size_t size() const {
      return items_.size();
    }

  private:
    std::size_t capacity_ {0};  ///< Fixed source queue capacity.
    std::deque<T> items_;  ///< Retained source items in producer order.
  };

  /**
   * @brief Positive rational PipeWire frame rate.
   */
  struct rational_rate_t {
    uint32_t numerator {0};  ///< Frames produced during one denominator interval.
    uint32_t denominator {1};  ///< Seconds represented by the rate denominator.
  };

  /**
   * @brief Origin used to discover DMA-BUF import capabilities.
   */
  enum class dmabuf_device_origin_e {
    desktop_wayland,  ///< Query the active Desktop Wayland compositor.
    verified_render_node,  ///< Query a previously verified DRM render node directly.
  };

  /**
   * @brief GPU endpoint used for PipeWire DMA-BUF capability discovery.
   */
  struct dmabuf_device_t {
    dmabuf_device_origin_e origin {dmabuf_device_origin_e::desktop_wayland};  ///< Capability-discovery endpoint type.
    std::string render_node;  ///< Absolute DRM render-node path for direct discovery.
  };

  /**
   * @brief Describe ordinary Desktop PipeWire DMA-BUF discovery.
   *
   * @return A descriptor that queries the current Wayland compositor.
   */
  dmabuf_device_t desktop_dmabuf_device();

  /**
   * @brief Describe display-server-independent DMA-BUF discovery on a verified GPU.
   *
   * @param render_node Previously verified absolute `/dev/dri/renderD*` path.
   * @return Direct device descriptor, or `std::nullopt` for an invalid path.
   */
  std::optional<dmabuf_device_t> verified_render_node_dmabuf_device(std::string_view render_node);

  /**
   * @brief Maximum-frame-rate range advertised to a PipeWire producer.
   */
  struct max_framerate_range_t {
    rational_rate_t preferred;  ///< Preferred maximum rational frame rate.
    rational_rate_t minimum;  ///< Lowest accepted maximum rational frame rate.
    rational_rate_t maximum;  ///< Highest accepted maximum rational frame rate.
  };

  /**
   * @brief State of PipeWire format negotiation before capture begins.
   */
  enum class negotiation_state_e {
    pending,  ///< No format or terminal stream error has arrived yet.
    complete,  ///< A positive frame size has been negotiated.
    failed,  ///< The stream terminated before negotiating a frame size.
  };

  /**
   * @brief Pure classification of one PipeWire producer buffer.
   */
  struct frame_classification_t {
    bool redundant_pts {false};  ///< Whether the producer repeated the preceding valid PTS.
    bool no_damage {false};  ///< Whether VideoDamage explicitly reports no changed pixels.
    bool corrupted {false};  ///< Whether SPA marked the buffer corrupt.
    bool unique {false};  ///< Whether capture should assign a new source generation.
  };

  /**
   * @brief Classify producer metadata without treating PTS repetition alone as loss.
   *
   * @param previous_pts Previously accepted producer PTS when available.
   * @param current_pts Current producer PTS when available.
   * @param damage Optional VideoDamage state; absence remains unknown.
   * @param corrupted Whether SPA marked the current buffer corrupt.
   * @return Independent metadata flags and the resulting unique-frame decision.
   */
  frame_classification_t classify_frame(
    const std::optional<std::uint64_t> &previous_pts,
    const std::optional<std::uint64_t> &current_pts,
    const std::optional<bool> &damage,
    bool corrupted
  );

  /**
   * @brief Immutable identity for one verified PipeWire video producer.
   *
   * A consumer opens its own PipeWire core connection. The descriptor must
   * never be used to share @c connected_core_fd between the remote encoder and
   * a local presenter.
   */
  struct stream_descriptor_t {
    int connected_core_fd {-1};  ///< Newly opened PipeWire socket for exactly one consumer.
    uint32_t node_id {UINT32_MAX};  ///< Volatile PipeWire node ID.
    uint64_t object_serial {UINT64_MAX};  ///< Stable PipeWire object serial used as target.object.
    int producer_pid {-1};  ///< Verified Gamescope producer process ID.
    uint64_t producer_start_time {0};  ///< `/proc/<pid>/stat` start-time identity.
    std::string render_node;  ///< DRM render node exported by the producer.
    std::string source_label;  ///< Human-readable verified source label.
  };

  /**
   * @brief Build a producer-compatible maximum-frame-rate range.
   *
   * A raw PipeWire stream remains variable-rate through a separate `0/1`
   * framerate property. The maximum-frame-rate choice must still contain
   * positive values so producers such as KWin can intersect their supported
   * refresh range with the consumer request.
   *
   * @param numerator Requested frame-rate numerator.
   * @param denominator Requested frame-rate denominator.
   * @return Preferred, minimum, and maximum values for PipeWire negotiation.
   */
  max_framerate_range_t max_framerate_range(uint32_t numerator, uint32_t denominator);

  /**
   * @brief Classify whether PipeWire format negotiation can start capture.
   *
   * @param stream_dead Whether PipeWire reported a terminal stream state.
   * @param width Negotiated frame width, or zero while unavailable.
   * @param height Negotiated frame height, or zero while unavailable.
   * @return Failed for a terminal stream, complete for a positive size, or pending.
   */
  negotiation_state_e negotiation_state(bool stream_dead, int width, int height);

  /**
   * @brief Decide whether a PipeWire source should connect during display initialization.
   *
   * Edge-triggered producers such as Gamescope retain their last emitted
   * commit across consumer connections. Capability-only encoder probes must
   * therefore avoid consuming the initial commit before the real stream.
   *
   * @param encoder_probe Whether encoder capability probing is active.
   * @param live_stream_required_for_probe Whether this source needs live
   *        negotiation to provide a usable probe display.
   * @return True when initialization should connect and negotiate the stream.
   */
  bool should_start_stream_during_initialization(bool encoder_probe, bool live_stream_required_for_probe);

  /**
   * @brief Decide whether an already productive source exhausted its reconnect grace period.
   *
   * @param source_was_productive Whether the selected source produced frames before reconnecting.
   * @param frame_received Whether the replacement consumer has received its first frame.
   * @param elapsed Time since the replacement consumer connected.
   * @param grace Maximum time allowed without a first producer frame.
   * @return True only for a previously productive source whose replacement consumer remains empty at or beyond the grace period.
   */
  bool retained_first_frame_timeout_expired(
    bool source_was_productive,
    bool frame_received,
    std::chrono::milliseconds elapsed,
    std::chrono::milliseconds grace
  );

  /**
   * @brief Validate the source identity required before opening a consumer.
   *
   * @param descriptor Candidate descriptor.
   * @return True only when node, object serial, process identity, and render node exist.
   */
  bool has_verified_source_identity(const stream_descriptor_t &descriptor);

  /**
   * @brief Check an optional producer render-node property against the selected GPU.
   *
   * Gamescope's PipeWire node may omit a render-node property even though its
   * producer process and configured Gamescope GPU are independently verified.
   * An absent property remains acceptable; a present value must match exactly.
   *
   * @param producer_render_node Optional render-node property from PipeWire.
   * @param selected_render_node DRM render node selected for the session.
   * @return True when the property is absent or exactly matches the selection.
   */
  bool matches_selected_render_node(const std::string &producer_render_node, const std::string &selected_render_node);

  /**
   * @brief Compare two descriptors without treating their consumer FDs as shared state.
   *
   * @param left First verified descriptor.
   * @param right Second verified descriptor.
   * @return True only when both refer to the same producer and PipeWire object.
   */
  bool refers_to_same_source(const stream_descriptor_t &left, const stream_descriptor_t &right);
}  // namespace pipewire_capture
