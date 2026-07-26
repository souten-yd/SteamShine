/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#pragma once

#include <cstdint>
#include <string>

namespace pipewire_capture {
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
