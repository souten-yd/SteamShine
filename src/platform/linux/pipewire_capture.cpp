/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#include "pipewire_capture.h"

namespace pipewire_capture {
  max_framerate_range_t max_framerate_range(const uint32_t requested_fps) {
    if (requested_fps == 0) {
      return {};
    }
    return {
      .preferred = requested_fps,
      .minimum = 1,
      .maximum = requested_fps,
    };
  }

  negotiation_state_e negotiation_state(const bool stream_dead, const int width, const int height) {
    if (stream_dead) {
      return negotiation_state_e::failed;
    }
    if (width > 0 && height > 0) {
      return negotiation_state_e::complete;
    }
    return negotiation_state_e::pending;
  }

  bool has_verified_source_identity(const stream_descriptor_t &descriptor) {
    return descriptor.node_id != UINT32_MAX && descriptor.object_serial != UINT64_MAX && descriptor.producer_pid > 0 && descriptor.producer_start_time > 0 && !descriptor.render_node.empty();
  }

  bool matches_selected_render_node(const std::string &producer_render_node, const std::string &selected_render_node) {
    return producer_render_node.empty() || producer_render_node == selected_render_node;
  }

  bool refers_to_same_source(const stream_descriptor_t &left, const stream_descriptor_t &right) {
    return has_verified_source_identity(left) && has_verified_source_identity(right) && left.node_id == right.node_id && left.object_serial == right.object_serial && left.producer_pid == right.producer_pid && left.producer_start_time == right.producer_start_time && left.render_node == right.render_node;
  }
}  // namespace pipewire_capture
