/**
 * @file src/stream_negotiation.h
 * @brief Canonical staged state for one Moonlight streaming negotiation.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

namespace stream {
  /**
   * @brief Exact frame or refresh rate represented as a rational number.
   */
  struct rational_rate_t {
    std::uint32_t numerator {0};  ///< Number of frames or refresh cycles in one denominator interval.
    std::uint32_t denominator {1};  ///< Positive interval denominator.
  };

  /**
   * @brief Width, height, and frame rate for one stream pipeline stage.
   */
  struct stream_geometry_t {
    std::uint32_t width {0};  ///< Width in pixels.
    std::uint32_t height {0};  ///< Height in pixels.
    rational_rate_t frame_rate;  ///< Exact rate when the protocol supplied one.
  };

  /**
   * @brief Bounded encoder bitrate values selected for one session.
   */
  struct bitrate_envelope_t {
    std::uint64_t minimum_bps {0};  ///< Lowest permitted encoder video bitrate.
    std::uint64_t initial_bps {0};  ///< Encoder video bitrate used when the session opens.
    std::uint64_t target_bps {0};  ///< Current target or average encoder video bitrate.
    std::uint64_t maximum_bps {0};  ///< Hard encoder video bitrate ceiling.
    std::uint64_t peak_bps {0};  ///< Short-term encoder video peak ceiling.
    std::uint64_t vbv_buffer_bits {0};  ///< Video buffering verifier capacity, when known.
  };

  /**
   * @brief Color and dynamic-range facts for a negotiation stage.
   */
  struct stream_color_state_t {
    bool hdr_requested {false};  ///< Whether the client requested HDR at this stage.
    bool hdr_selected {false};  ///< Whether policy selected HDR before startup.
    bool hdr_active {false};  ///< Whether the opened source and encoder are actively HDR.
    std::uint8_t bit_depth {8};  ///< Selected or active component bit depth.
    int encoder_csc_mode {0};  ///< GameStream encoder colorspace/range mode.
    int chroma_sampling_type {0};  ///< GameStream chroma mode, where zero is 4:2:0.
    std::string colorspace;  ///< Stable active colorspace label when known.
    bool full_range {false};  ///< Whether active video uses full-range samples.
  };

  /**
   * @brief Immutable client values collected across NVHTTP and RTSP.
   */
  struct stream_request_t {
    stream_geometry_t launch_geometry;  ///< Geometry parsed from the NVHTTP mode query.
    stream_geometry_t stream_geometry;  ///< Authoritative geometry parsed from RTSP ANNOUNCE.
    bool rtsp_received {false};  ///< Whether RTSP request fields have been appended.
    std::uint64_t client_bitrate_ceiling_bps {0};  ///< RTSP maximum video bitrate before host policy.
    std::uint64_t configured_total_bitrate_bps {0};  ///< Client-configured total A/V network budget before deductions.
    std::uint32_t client_codec_mask {0};  ///< Codec formats selected or advertised through the current protocol exchange.
    int requested_codec {-1};  ///< RTSP bitstream format, or -1 before ANNOUNCE.
    bool hdr_requested {false};  ///< HDR intent from NVHTTP launch.
    bool ten_bit_requested {false};  ///< Ten-bit dynamic range requested by RTSP.
    std::string client_id;  ///< Stable Moonlight client unique identifier.
    std::string capability_signature;  ///< Stable signature derived only from current protocol request facts.
    int application_id {0};  ///< Application ID requested for launch or resume.
  };

  /**
   * @brief Geometry, codec, color, and rate facts for selected or active state.
   */
  struct stream_endpoint_state_t {
    std::string source_origin;  ///< Physical, attached, or owned source selected for this stage.
    stream_geometry_t source_geometry;  ///< Source canvas geometry for this stage.
    stream_geometry_t capture_geometry;  ///< Capture producer geometry for this stage.
    stream_geometry_t encode_geometry;  ///< Encoded output geometry for this stage.
    int codec {-1};  ///< GameStream codec format: 0 H.264, 1 HEVC, or 2 AV1.
    std::string profile;  ///< Existing protocol/backend profile label when known.
    stream_color_state_t color;  ///< Selected or active color state.
    bitrate_envelope_t bitrate;  ///< Selected or active encoder bitrate envelope.
    std::string backend;  ///< Active encoder backend name when opened.
    std::string render_node;  ///< Selected encoder render node when available.
    bool runtime_rate_update_supported {false};  ///< Whether this active backend can update bitrate without recreation.
    std::string reason;  ///< Stable reason for the stage decision.
    std::string network_class;  ///< Explicit profile network class selected for this session.
    std::string profile_selection_reason;  ///< Stable exact-match or rejection explanation.
    std::string geometry_policy;  ///< Selected profile geometry policy, when a profile matched.
    std::string quality_preset;  ///< Selected existing-setting quality preset, when a profile matched.
    std::string orientation;  ///< Selected content orientation policy, when a profile matched.
    int safe_area_percent {0};  ///< Selected bounded safe-area inset percentage.
  };

  /**
   * @brief Runtime measurements appended to the canonical session snapshot.
   */
  struct stream_observed_t {
    double source_fps {0.0};  ///< Unique source frames observed per second.
    double encode_fps {0.0};  ///< Encoder submissions observed per second.
    std::uint64_t target_bitrate_bps {0};  ///< Active encoder target bitrate.
    std::uint64_t actual_bitrate_bps {0};  ///< Measured encoded bitrate when available.
    std::uint64_t capture_queue_frames {0};  ///< Current capture queue depth.
    std::uint64_t encoder_queue_frames {0};  ///< Current encoder occupancy.
    std::uint64_t network_queue_frames {0};  ///< Current encoded network queue depth.
    std::uint64_t network_queue_bytes {0};  ///< Current encoded network queue bytes.
    double capture_age_p99_ms {0.0};  ///< p99 source age when capture publishes.
    double encode_age_p99_ms {0.0};  ///< p99 source age when encoding starts.
    double network_age_p99_ms {0.0};  ///< p99 source age when packetization starts.
    std::string output_status_reason;  ///< Stable source/static/consumer status reason.
  };

  /**
   * @brief One session-owned requested, selected, active, and observed transaction.
   */
  struct stream_negotiation_snapshot_t {
    std::uint64_t generation {0};  ///< Launch-session generation that owns this snapshot.
    stream_request_t requested;  ///< Client protocol values, never overwritten by fallback.
    stream_endpoint_state_t selected;  ///< Policy result selected before pipeline startup.
    stream_endpoint_state_t active;  ///< Values reported by opened source and encoder objects.
    stream_observed_t observed;  ///< Runtime measurements appended from bounded diagnostics.
    std::vector<std::string> fallback_reasons;  ///< Stable ordered reasons for every substitution or discarded hint.
  };
}  // namespace stream
