/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief PipeWire capture provider for verified Gamescope sessions.
 */
#include "pipewire.cpp"
#include "pipewire_capture.h"
#include "src/logging.h"
#include "src/steamos_virtual_session.h"

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
      std::string failure;
      pipewire_capture::stream_descriptor_t descriptor;
      if (!steamos_virtual_session::open_verified_gamescope_pipewire_consumer(descriptor, failure)) {
        BOOST_LOG(error) << "PIPEWIRE_STREAM_CONNECT_FAILED reason=" << failure;
        return -1;
      }
      const auto session {steamos_virtual_session::status_snapshot()};
      if (session.width <= 0 || session.height <= 0 || session.fps <= 0) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=invalid_session_dimensions";
        close(descriptor.connected_core_fd);
        return -1;
      }
      width = session.width;
      height = session.height;
      pipewire.set_gamescope_requested_size(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
      offset_x = 0;
      offset_y = 0;
      source_was_productive_ = session.captured_frames > 0;
      first_frame_timeout_at_.reset();
      if (!pipewire_capture::has_verified_source_identity(descriptor)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=incomplete_verified_source_identity";
        close(descriptor.connected_core_fd);
        return -1;
      }
      if (!set_verified_dmabuf_render_node(descriptor.render_node)) {
        BOOST_LOG(error) << "PIPEWIRE_NODE_DISCOVERY_FAILED reason=invalid_verified_render_node";
        close(descriptor.connected_core_fd);
        return -1;
      }
      out_pipewire_fd = descriptor.connected_core_fd;
      out_pipewire_node = descriptor.node_id;
      out_pipewire_objectserial = descriptor.object_serial;
      steamos_virtual_session::mark_gamescope_pipewire_node(descriptor.node_id, descriptor.object_serial, descriptor.producer_pid);
      BOOST_LOG(info) << "CAPTURE_SOURCE source=gamescope_pipewire PIPEWIRE_NODE id=" << descriptor.node_id << " PIPEWIRE_SERIAL serial=" << descriptor.object_serial << " PRODUCER_PID=" << descriptor.producer_pid << " DRM_RENDER_NODE=" << session.render_node;
      return 0;
    }

    /**
     * @brief Keep virtual-session dimensions rather than querying desktop outputs.
     */
    void verify_and_update_display_parameters() override {}

    /**
     * @brief Preserve Gamescope's edge-triggered initial frame during encoder probes.
     *
     * The verified session descriptor already supplies dimensions and GPU
     * identity, so codec validation does not need to connect to the producer.
     *
     * @return False because a live stream is only needed for real capture.
     */
    bool live_stream_required_for_encoder_probe() const override {
      return false;
    }

    /**
     * @brief Reinitialize after a verified Gamescope PipeWire interruption.
     *
     * Gamescope pauses and resumes its stream during a size renegotiation. The
     * shared PipeWire backend reports that pause as a dead stream so the image
     * and encoder dimensions can be rebuilt. Preserve fail-closed behavior by
     * permitting reinitialization only while the original Gamescope process
     * identity remains current.
     *
     * @param out_status Receives reinitialization or terminal capture status.
     * @return True because Gamescope uses identity-aware custom handling.
     */
    bool check_stream_dead(platf::capture_e &out_status) override {
      if (steamos_virtual_session::mark_capture_reinitializing()) {
        BOOST_LOG(info) << "PIPEWIRE_STREAM_REINITIALIZING source=gamescope_pipewire reason=producer_pause";
        out_status = platf::capture_e::reinit;
        return true;
      }
      BOOST_LOG(error) << "PIPEWIRE_SOURCE_IDENTITY_LOST source=gamescope_pipewire";
      steamos_virtual_session::mark_capture_lost();
      out_status = platf::capture_e::error;
      return true;
    }

    /**
     * @brief Fail closed when a reused Gamescope source stops producing frames.
     *
     * A newly created owned session may legitimately need several seconds to
     * start Steam and publish its first commit. A source that already produced
     * frames has no equivalent startup dependency, so an empty reconnect is
     * treated as a stale producer instead of streaming black duplicates.
     *
     * @param frame_received Whether this capture consumer has received a frame.
     * @param out_status Receives the terminal capture error.
     * @return True after the retained-source grace period expires.
     */
    bool check_frame_timeout(const bool frame_received, platf::capture_e &out_status) override {
      if (frame_received) {
        first_frame_timeout_at_.reset();
        return false;
      }
      if (!source_was_productive_) {
        return false;
      }
      const auto now {std::chrono::steady_clock::now()};
      if (!first_frame_timeout_at_) {
        first_frame_timeout_at_ = now;
        return false;
      }
      const auto elapsed {std::chrono::duration_cast<std::chrono::milliseconds>(now - *first_frame_timeout_at_)};
      if (!pipewire_capture::retained_first_frame_timeout_expired(source_was_productive_, frame_received, elapsed, retained_first_frame_timeout)) {
        return false;
      }
      BOOST_LOG(error) << "PIPEWIRE_SOURCE_STALLED source=gamescope_pipewire reason=retained_first_frame_timeout";
      steamos_virtual_session::mark_capture_lost();
      out_status = platf::capture_e::error;
      return true;
    }

  private:
    static constexpr std::chrono::seconds retained_first_frame_timeout {2};  ///< Grace period for an already productive source.
    bool source_was_productive_ {false};  ///< Whether this Gamescope source produced frames before the consumer connected.
    std::optional<std::chrono::steady_clock::time_point> first_frame_timeout_at_;  ///< Start of retained first-frame starvation.
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
    steamos_virtual_session::record_capture_geometry(display->width, display->height);
    steamos_virtual_session::mark_capture_ready();
    return display;
  }
}  // namespace platf
