/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "rtsp.h"
#include "stream_negotiation.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;  ///< GameStream base-port offset used for the video UDP stream.
  constexpr auto CONTROL_PORT = 10;  ///< GameStream base-port offset used for the control channel.
  constexpr auto AUDIO_STREAM_PORT = 11;  ///< GameStream base-port offset used for the audio UDP stream.
  constexpr std::uint16_t FRAME_FEC_STATUS_PACKET_TYPE = 0x5502;  ///< Moonlight per-frame FEC status control-message type.

  /**
   * @brief Validated Moonlight per-frame FEC reception counters.
   */
  struct frame_fec_status_t {
    std::uint32_t frame_index {};  ///< Video frame index described by this report.
    std::uint16_t highest_received_sequence_number {};  ///< Highest RTP sequence number observed for the frame.
    std::uint16_t next_contiguous_sequence_number {};  ///< First sequence number after the contiguous receive prefix.
    std::uint16_t missing_packets_before_highest {};  ///< Packet holes observed before the highest sequence number.
    std::uint16_t total_data_packets {};  ///< Data packets transmitted for this FEC block.
    std::uint16_t total_parity_packets {};  ///< Parity packets transmitted for this FEC block.
    std::uint16_t received_data_packets {};  ///< Data packets received by Moonlight.
    std::uint16_t received_parity_packets {};  ///< Parity packets received by Moonlight.
    std::uint8_t fec_percentage {};  ///< FEC percentage associated with the encoded frame.
    std::uint8_t multi_fec_block_index {};  ///< Zero-based block index within the frame.
    std::uint8_t multi_fec_block_count {};  ///< Number of FEC blocks forming the frame.
  };

  /**
   * @brief First cause that requested termination of a streaming session.
   */
  enum class disconnect_reason_e : std::uint8_t {
    unknown,  ///< No terminating path recorded a more specific cause.
    remote_control_disconnect,  ///< ENet reported that the remote control peer disconnected.
    control_ping_timeout,  ///< The server received no control traffic before its ping deadline.
    control_protocol_error,  ///< Authentication or framing validation failed on a control packet.
    initial_video_ping_timeout,  ///< The client never established its video UDP endpoint.
    initial_audio_ping_timeout,  ///< The client never established its audio UDP endpoint.
    video_worker_ended,  ///< Capture or video transmission ended before another stop cause was recorded.
    audio_worker_ended,  ///< Audio capture or transmission ended before another stop cause was recorded.
    local_session_cleanup,  ///< RTSP or application lifecycle cleanup stopped the session locally.
    service_shutdown,  ///< SteamShine shutdown stopped the control broadcast.
  };

  struct session_t;

  /**
   * @brief Parse and validate one big-endian Moonlight frame-FEC status payload.
   *
   * @param payload Raw control-message payload following packet type 0x5502.
   * @return Parsed counters, or no value for truncated or inconsistent input.
   */
  std::optional<frame_fec_status_t> parse_frame_fec_status(std::string_view payload);

  /**
   * @brief Convert a disconnect cause to a stable diagnostics token.
   *
   * @param reason Disconnect cause to convert.
   * @return Stable lowercase token suitable for logs and JSON.
   */
  std::string_view to_string(disconnect_reason_e reason);

  /**
   * @brief Classify an IP address for transport-path diagnostics.
   *
   * The shared-address-space result covers RFC 6598 addresses commonly used
   * by carrier-grade NAT and overlay networks without assuming a particular
   * networking product.
   *
   * @param address Textual IPv4 or IPv6 address without a port.
   * @return Stable diagnostics token describing the address scope.
   */
  std::string_view classify_network_address(std::string_view address);

  /**
   * @brief Concatenate byte ranges while inserting padding at fixed intervals.
   *
   * @param insert_size Number of zero bytes to insert before each slice.
   * @param slice_size Number of payload bytes between insertions.
   * @param data1 First byte range.
   * @param data2 Second byte range.
   * @return Concatenated payload with the requested padding inserted.
   */
  std::vector<std::uint8_t> concat_and_insert(
    std::uint64_t insert_size,
    std::uint64_t slice_size,
    const std::string_view &data1,
    const std::string_view &data2
  );

  /**
   * @brief Stream configuration shared by capture and network senders.
   */
  struct config_t {
    audio::config_t audio;  ///< Audio capture configuration for the stream.
    video::config_t monitor;  ///< Video capture and encoder configuration for the selected monitor.

    int packetsize;  ///< Maximum payload size for network packets.
    int minRequiredFecPackets;  ///< Minimum recovery packets required before FEC is emitted.
    int mlFeatureFlags;  ///< Moonlight feature flags negotiated for this session.
    int controlProtocolType;  ///< GameStream control protocol variant selected by the client.
    int audioQosType;  ///< Audio QoS type.
    int videoQosType;  ///< Video QoS type.

    uint32_t encryptionFlagsEnabled;  ///< Bitmask of GameStream encryption features enabled for the session.

    std::optional<int> gcmap;  ///< Optional game-controller mapping override from the launch request.
  };

  /**
   * @brief Convert integer or hundredths-of-Hz protocol fields to an exact rational rate.
   *
   * @param integer_fps Integer FPS supplied by the client.
   * @param refresh_rate_x100 Optional refresh rate in hundredths of Hz.
   * @return Reduced rational rate that preserves a valid hundredths value.
   */
  rational_rate_t rational_rate_from_protocol(int integer_fps, int refresh_rate_x100 = 0);

  /**
   * @brief Initialize immutable launch-request fields on an existing launch session.
   *
   * @param launch_session Existing NVHTTP/RTSP launch-session owner.
   */
  void initialize_launch_negotiation(rtsp_stream::launch_session_t &launch_session);

  /**
   * @brief Add one stable fallback reason without duplicating it.
   *
   * @param snapshot Session-owned canonical negotiation state.
   * @param reason Stable machine-readable fallback reason.
   */
  void add_fallback_reason(stream_negotiation_snapshot_t &snapshot, std::string_view reason);

  /**
   * @brief Append RTSP request values and the pre-start selection to a launch snapshot.
   *
   * @param launch_session Existing session containing immutable NVHTTP request values.
   * @param config Validated RTSP stream configuration selected for startup.
   * @param requested_monitor Immutable RTSP video request before host/profile policy.
   * @param client_maximum_bitrate_kbps Raw maximum bitrate supplied by RTSP.
   * @param configured_total_bitrate_kbps Raw configured total bitrate before overhead deductions.
   * @param refresh_hint_discarded Whether the hundredths-of-Hz hint failed consistency validation.
   */
  void populate_rtsp_negotiation(
    rtsp_stream::launch_session_t &launch_session,
    const config_t &config,
    const video::config_t &requested_monitor,
    std::int64_t client_maximum_bitrate_kbps,
    std::int64_t configured_total_bitrate_kbps,
    bool refresh_hint_discarded
  );

  /**
   * @brief Update active source and encoder facts after the video backend opens.
   *
   * @param snapshot Session-owned canonical negotiation state.
   * @param source_width Opened source width in pixels.
   * @param source_height Opened source height in pixels.
   * @param backend Existing encoder backend name.
   * @param profile Existing encoder codec/profile implementation name.
   * @param colorspace Active encoder colorspace.
   */
  void populate_active_video(
    stream_negotiation_snapshot_t &snapshot,
    int source_width,
    int source_height,
    std::string_view backend,
    std::string_view profile,
    const video::sunshine_colorspace_t &colorspace
  );

  /**
   * @brief Request an immediate independently decodable frame for a newly armed recording.
   *
   * The request is raised on every running stream mailbox so recording does not
   * have to wait for an encoder's optional periodic key-frame policy.
   */
  void request_recording_key_frame();

  /**
   * @brief Serialize one canonical stream negotiation snapshot.
   *
   * @param snapshot Session snapshot to serialize.
   * @param available Whether an active session supplied the snapshot.
   * @return Stable JSON object with all four negotiation stages.
   */
  nlohmann::json negotiation_snapshot_json(const stream_negotiation_snapshot_t &snapshot, bool available = true);

  namespace session {
    /**
     * @brief Enumerates supported state options.
     */
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Allocate and initialize platform input state for a stream.
     *
     * @param config Configuration values to apply.
     * @param launch_session Launch session.
     * @return Allocated object or identifier, or an error value on failure.
     */
    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    /**
     * @brief Start a streaming session for the supplied peer address.
     *
     * @param session Active streaming or pairing session for the request.
     * @param addr_string Addr string.
     * @return Start status.
     */
    int start(session_t &session, const std::string &addr_string);
    /**
     * @brief Stop a streaming session and prevent more packets from being queued.
     *
     * @param session Active streaming or pairing session for the request.
     * @param reason First local or remote cause requesting termination.
     * @param event_data Optional ENet disconnect data supplied by the peer.
     */
    void stop(
      session_t &session,
      disconnect_reason_e reason = disconnect_reason_e::local_session_cleanup,
      std::uint32_t event_data = 0
    );
    /**
     * @brief Wait for worker threads owned by the session to exit.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void join(session_t &session);
    /**
     * @brief Platform handle returned from stream setup.
     *
     * @param session Active streaming or pairing session for the request.
     * @return Current lifecycle state for the stream session.
     */
    state_e state(session_t &session);
    /**
     * @brief Return the paired client certificate for a stream session.
     *
     * @param session Active streaming or pairing session for the request.
     * @return PEM certificate associated with the session's client.
     */
    const std::string &client_cert(session_t &session);

    /**
     * @brief Return the canonical state owned by a running stream session.
     *
     * Observed fields are refreshed from the existing bounded diagnostics before
     * the copy is returned.
     *
     * @param session Active streaming session.
     * @return Thread-safe copy of the session negotiation snapshot.
     */
    stream_negotiation_snapshot_t negotiation_snapshot(session_t &session);

    /**
     * @brief Record active video backend facts after source and encoder startup.
     *
     * @param session Active streaming session.
     * @param source_width Opened source width in pixels.
     * @param source_height Opened source height in pixels.
     * @param backend Existing encoder backend name.
     * @param profile Existing encoder codec/profile implementation name.
     * @param colorspace Active encoder colorspace.
     */
    void record_video_active(
      session_t &session,
      int source_width,
      int source_height,
      std::string_view backend,
      std::string_view profile,
      const video::sunshine_colorspace_t &colorspace
    );
  }  // namespace session
}  // namespace stream
