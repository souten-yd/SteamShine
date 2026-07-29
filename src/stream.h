/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <cstdint>
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

  struct session_t;

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
     */
    void stop(session_t &session);
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
