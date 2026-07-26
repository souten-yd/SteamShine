/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief PipeWire capture provider for verified Gamescope sessions.
 */
#include "gamescope_source.h"
#include "pipewire_capture.h"
#include "pipewire.cpp"
#include "src/config.h"
#include "src/logging.h"
#include "src/steamos_virtual_session.h"

#include <algorithm>
#include <chrono>

namespace gamescope_pipewire {
  /**
   * @brief Direct PipeWire capture display for a verified Gamescope source.
   */
  class display_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief Discover the selected Gamescope node and configure the shared consumer.
     *
     * @param display_name Ignored synthetic Gamescope display name.
     * @param out_pipewire_fd Receives a direct host PipeWire descriptor.
     * @param out_pipewire_node Receives the verified Gamescope node ID.
     * @param out_pipewire_objectserial Receives the verified object serial.
     * @return Zero when the source identity and dimensions are valid.
     */
    int configure_stream(const std::string &display_name [[maybe_unused]], int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      std::string runtime;
      std::string remote;
      int gamescope_pid {};
      if (!steamos_virtual_session::gamescope_pipewire_endpoint(runtime, remote, gamescope_pid)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=selected_session_endpoint_unavailable";
        return -1;
      }
      std::string failure;
      const auto sources {gamescope_source::discover_gamescope_sources(runtime, remote, std::chrono::milliseconds {config::steamos_virtual_display.pipewire_node_timeout_milliseconds}, failure)};
      const auto source {std::find_if(sources.begin(), sources.end(), [gamescope_pid](const gamescope_source::gamescope_source_t &candidate) {
        return candidate.producer_pid == gamescope_pid && gamescope_source::source_identity_is_current(candidate);
      })};
      if (source == sources.end()) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=" << failure << " gamescope_pid=" << gamescope_pid;
        return -1;
      }
      const auto stream_fd {gamescope_source::open_host_pipewire_socket(runtime, remote, failure)};
      if (!stream_fd) {
        BOOST_LOG(error) << "PIPEWIRE_STREAM_CONNECT_FAILED reason=" << failure;
        return -1;
      }
      const auto session {steamos_virtual_session::status_snapshot()};
      if (session.width <= 0 || session.height <= 0 || session.fps <= 0) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=invalid_session_dimensions";
        return -1;
      }
      width = session.width;
      height = session.height;
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
      offset_x = 0;
      offset_y = 0;
      pipewire_capture::stream_descriptor_t descriptor {
        .connected_core_fd = *stream_fd,
        .node_id = source->node_id,
        .object_serial = source->object_serial,
        .producer_pid = source->producer_pid,
        .producer_start_time = source->producer_start_time,
        .render_node = source->render_node,
        .source_label = source->node_description,
      };
      if (!pipewire_capture::has_verified_source_identity(descriptor)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=incomplete_verified_source_identity";
        close(*stream_fd);
        return -1;
      }
      out_pipewire_fd = descriptor.connected_core_fd;
      out_pipewire_node = descriptor.node_id;
      out_pipewire_objectserial = descriptor.object_serial;
      steamos_virtual_session::mark_gamescope_pipewire_node(source->node_id, source->object_serial, source->producer_pid);
      BOOST_LOG(info) << "CAPTURE_SOURCE source=gamescope_pipewire PIPEWIRE_NODE id=" << source->node_id << " PIPEWIRE_SERIAL serial=" << source->object_serial << " PRODUCER_PID=" << source->producer_pid << " DRM_RENDER_NODE=" << session.render_node;
      return 0;
    }

    /**
     * @brief Keep virtual-session dimensions rather than querying desktop outputs.
     */
    void verify_and_update_display_parameters() override {}

    /**
     * @brief Fail the virtual session when its verified PipeWire node disappears.
     *
     * @param out_status Receives the terminal capture status.
     * @return True because desktop-source fallback is unsafe.
     */
    bool check_stream_dead(platf::capture_e &out_status) override {
      BOOST_LOG(error) << "PIPEWIRE_NODE_DISAPPEARED source=gamescope_pipewire";
      steamos_virtual_session::mark_capture_lost();
      out_status = platf::capture_e::error;
      return true;
    }
  };
}  // namespace gamescope_pipewire

namespace platf {
  /**
   * @brief Create capture for a verified Gamescope PipeWire source.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Synthetic Gamescope display name.
   * @param config Capture configuration.
   * @return PipeWire display, or nullptr when the source cannot be verified.
   */
  std::shared_ptr<display_t> gamescope_pipewire_display(const mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[gamescope-pipewire] Unsupported hardware device type";
      return nullptr;
    }
    auto display {std::make_shared<gamescope_pipewire::display_t>()};
    if (display->init(hwdevice_type, display_name, config) != 0) {
      steamos_virtual_session::mark_capture_lost();
      return nullptr;
    }
    steamos_virtual_session::mark_capture_ready();
    return display;
  }
}  // namespace platf
