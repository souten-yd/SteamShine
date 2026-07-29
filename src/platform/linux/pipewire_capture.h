/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pipewire_capture {
  /**
   * @brief Maximum-frame-rate range advertised to a PipeWire producer.
   */
  struct max_framerate_range_t {
    uint32_t preferred {0};  ///< Preferred maximum frame rate in frames per second.
    uint32_t minimum {0};  ///< Lowest accepted maximum frame rate in frames per second.
    uint32_t maximum {0};  ///< Highest accepted maximum frame rate in frames per second.
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
   * @param requested_fps Maximum frame rate requested by the streaming client.
   * @return Preferred, minimum, and maximum values for PipeWire negotiation.
   */
  max_framerate_range_t max_framerate_range(uint32_t requested_fps);

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
