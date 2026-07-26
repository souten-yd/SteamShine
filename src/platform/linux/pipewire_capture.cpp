/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#include "pipewire_capture.h"

namespace pipewire_capture {
  bool has_verified_source_identity(const stream_descriptor_t &descriptor) {
    return descriptor.node_id != UINT32_MAX && descriptor.object_serial != UINT64_MAX && descriptor.producer_pid > 0 && descriptor.producer_start_time > 0 && !descriptor.render_node.empty();
  }

  bool refers_to_same_source(const stream_descriptor_t &left, const stream_descriptor_t &right) {
    return has_verified_source_identity(left) && has_verified_source_identity(right) && left.node_id == right.node_id && left.object_serial == right.object_serial && left.producer_pid == right.producer_pid && left.producer_start_time == right.producer_start_time && left.render_node == right.render_node;
  }
}  // namespace pipewire_capture
