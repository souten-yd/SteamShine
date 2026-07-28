/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief Identity descriptor shared by independent PipeWire capture consumers.
 */
#include "pipewire_capture.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace pipewire_capture {
  dmabuf_device_t desktop_dmabuf_device() {
    return {};
  }

  std::optional<dmabuf_device_t> verified_render_node_dmabuf_device(const std::string_view render_node) {
    const std::filesystem::path path {render_node};
    const auto filename {path.filename().string()};
    if (!path.is_absolute() || path.parent_path() != "/dev/dri" || !filename.starts_with("renderD") || filename.size() == 7 || !std::all_of(filename.begin() + 7, filename.end(), [](const unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      return std::nullopt;
    }
    return dmabuf_device_t {
      .origin = dmabuf_device_origin_e::verified_render_node,
      .render_node = path.string(),
    };
  }

  frame_classification_t classify_frame(
    const std::optional<std::uint64_t> &previous_pts,
    const std::optional<std::uint64_t> &current_pts,
    const std::optional<bool> &damage,
    const bool corrupted
  ) {
    const bool redundant_pts {previous_pts && current_pts && *previous_pts == *current_pts};
    const bool no_damage {damage && !*damage};
    return {
      .redundant_pts = redundant_pts,
      .no_damage = no_damage,
      .corrupted = corrupted,
      .unique = !corrupted && !no_damage,
    };
  }

  max_framerate_range_t max_framerate_range(const uint32_t numerator, const uint32_t denominator) {
    if (numerator == 0 || denominator == 0) {
      return {};
    }
    return {
      .preferred = {numerator, denominator},
      .minimum = {1, 1},
      .maximum = {numerator, denominator},
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

  bool should_start_stream_during_initialization(const bool encoder_probe, const bool live_stream_required_for_probe) {
    return !encoder_probe || live_stream_required_for_probe;
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
