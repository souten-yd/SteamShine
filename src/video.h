/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

// standard includes
#include <chrono>
#include <string>

// local includes
#include "input.h"
#include "latency_diagnostics.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "video_colorspace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;

namespace video {
  constexpr std::uint32_t CAPTURE_QUEUE_FRAME_LIMIT = 1;  ///< Latest captured image retained for low-latency encoding.
  constexpr std::uint32_t NETWORK_QUEUE_FRAME_LIMIT = 2;  ///< Encoded frames retained before producer backpressure.

  /**
   * @brief Reasons that require or explain an IDR frame.
   */
  enum class idr_reason_e {
    client_request,  ///< Moonlight explicitly requested a decoder refresh.
    recovery,  ///< Reference-frame recovery required a full decoder refresh.
    periodic,  ///< The encoder emitted an IDR without a pending external request.
    reconnect,  ///< A newly connected or resumed decoder requires an initial IDR.
  };

  /**
   * @brief Encoding configuration requested by a remote client.
   */
  struct config_t {
    int width;  ///< Video width in pixels.
    int height;  ///< Video height in pixels.
    int framerate;  ///< Requested framerate used in the per-frame bitrate budget.
    int framerateX100;  ///< Optional NTSC-style framerate value, e.g. 59.94 as 5994.
    int bitrate;  ///< Video bitrate in kilobits for the requested framerate.
    int slicesPerFrame;  ///< Number of slices per frame.
    int numRefFrames;  ///< Maximum number of reference frames.
    int encoderCscMode;  ///< Requested color range and SDR colorspace; HDR always uses BT.2020 and ST2084.
    int videoFormat;  ///< Video codec format: 0 = H.264, 1 = HEVC, 2 = AV1.
    int dynamicRange;  ///< Encoding color depth: 0 = 8-bit, 1 = 10-bit.
    int chromaSamplingType;  ///< Chroma sampling type: 0 = 4:2:0, 1 = 4:4:4.
    int enableIntraRefresh;  ///< Intra refresh setting: 0 = disabled, 1 = enabled.
  };

  /**
   * @brief Map an FFmpeg hardware device type to Sunshine's memory type.
   *
   * @param type FFmpeg hardware device type reported by the encoder backend.
   * @return Sunshine memory type used by the capture and encode pipeline.
   */
  platf::mem_type_e map_base_dev_type(AVHWDeviceType type);
  /**
   * @brief Map an FFmpeg pixel format to Sunshine's pixel format enum.
   *
   * @param fmt FFmpeg pixel format to convert.
   * @return Sunshine pixel format used by display and encoder backends.
   */
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt);

  /**
   * @brief Release an FFmpeg codec context.
   *
   * @param ctx Codec context allocated by FFmpeg.
   */
  void free_ctx(AVCodecContext *ctx);
  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   *
   * @param frame Video or graphics frame being processed.
   */
  void free_frame(AVFrame *frame);
  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   *
   * @param ref FFmpeg buffer reference to unref and free.
   */
  void free_buffer(AVBufferRef *ref);

  /**
   * @brief Owning pointer for an FFmpeg codec context.
   */
  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_ctx>;
  /**
   * @brief Owning pointer for an FFmpeg frame.
   */
  using avcodec_frame_t = util::safe_ptr<AVFrame, free_frame>;
  /**
   * @brief Owning pointer for an FFmpeg buffer reference.
   */
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, free_buffer>;
  /**
   * @brief Owning pointer for an FFmpeg software-scaling context.
   */
  using sws_t = util::safe_ptr<SwsContext, sws_freeContext>;
  /**
   * @brief Latest-frame event transporting captured images to the encoder.
   */
  using img_event_t = std::shared_ptr<safe::event_t<std::shared_ptr<platf::img_t>>>;

  /**
   * @brief Bounded capture, encode, and network pipeline diagnostics.
   */
  struct pipeline_diagnostics_t {
    std::uint64_t capture_queue_current {0};  ///< Captured images currently waiting for encode.
    std::uint64_t capture_queue_max {0};  ///< Greatest captured-image queue depth.
    std::uint64_t capture_frames_replaced {0};  ///< Older pending images replaced by a newer frame.
    std::uint64_t pipewire_buffers_received {0};  ///< Producer buffers dequeued by PipeWire callbacks.
    std::uint64_t pipewire_buffers_replaced {0};  ///< Producer buffers superseded before capture consumed them.
    std::uint64_t encoder_queue_current {0};  ///< Frames currently executing in the encoder.
    std::uint64_t encoder_queue_max {0};  ///< Greatest concurrent encoder occupancy.
    std::uint64_t capture_deadline_misses {0};  ///< Output deadlines skipped instead of creating catch-up bursts.
    std::uint64_t encoded_unique_frames {0};  ///< Encoder submissions carrying a newly captured frame.
    std::uint64_t encoded_duplicate_frames {0};  ///< Encoder submissions reusing the preceding converted frame.
    std::uint64_t encoded_frames {0};  ///< Compatibility total of unique and duplicate encoder submissions.
    std::uint64_t duplicate_frames {0};  ///< Compatibility alias for duplicate encoder submissions.
    std::uint64_t duplicate_run_max {0};  ///< Longest consecutive run of duplicate encoder submissions.
    std::uint64_t pipewire_unique_frames {0};  ///< Valid unique PipeWire frames accepted for capture.
    std::uint64_t pipewire_redundant_pts {0};  ///< PipeWire buffers repeating the preceding PTS.
    std::uint64_t pipewire_no_damage_frames {0};  ///< PipeWire buffers explicitly reporting no damage.
    std::uint64_t pipewire_queue_overflows {0};  ///< Source buffers rejected at the bounded producer handoff.
    std::uint32_t requested_fps_numerator {0};  ///< Client-requested rational FPS numerator.
    std::uint32_t requested_fps_denominator {1};  ///< Client-requested rational FPS denominator.
    std::uint32_t negotiated_fps_numerator {0};  ///< PipeWire negotiated preferred FPS numerator.
    std::uint32_t negotiated_fps_denominator {1};  ///< PipeWire negotiated preferred FPS denominator.
    std::uint32_t negotiated_max_fps_numerator {0};  ///< PipeWire negotiated maximum FPS numerator.
    std::uint32_t negotiated_max_fps_denominator {1};  ///< PipeWire negotiated maximum FPS denominator.
    double observed_source_fps {0.0};  ///< Unique source generations observed per elapsed source interval.
    double observed_encode_fps {0.0};  ///< Encoder submissions observed per elapsed encode interval.
    std::string output_status_reason;  ///< Stable source/static/consumer status reason.
    latency_diagnostics::statistics_t source_interarrival_ms;  ///< Interarrival time between accepted unique source frames.
    latency_diagnostics::statistics_t encode_interarrival_ms;  ///< Interarrival time between encoder submissions.
    std::uint64_t network_queue_bytes {0};  ///< Encoded bytes waiting for the video sender.
    std::uint64_t network_queue_frames {0};  ///< Encoded frames waiting for the video sender.
    std::uint64_t network_queue_frames_max {0};  ///< Greatest encoded-frame queue depth.
    std::uint64_t socket_outq_bytes {0};  ///< Latest kernel video socket output-queue size.
    std::uint64_t socket_outq_bytes_max {0};  ///< Greatest kernel video socket output-queue size.
    std::uint64_t idr_requests {0};  ///< Accepted external and reconnect IDR requests.
    std::uint64_t idr_emitted {0};  ///< IDR frames emitted by the encoder.
    std::uint64_t idr_reason_client_request {0};  ///< Emitted IDRs attributed to a client request.
    std::uint64_t idr_reason_recovery {0};  ///< Emitted IDRs attributed to reference recovery.
    std::uint64_t idr_reason_periodic {0};  ///< Unrequested periodic IDRs emitted by the encoder.
    std::uint64_t idr_reason_reconnect {0};  ///< Emitted initial IDRs attributed to connection setup.
    latency_diagnostics::statistics_t frame_age_at_capture_ms;  ///< Source timestamp age when capture publishes a frame.
    latency_diagnostics::statistics_t frame_age_at_encode_ms;  ///< Source timestamp age when encoding starts.
    latency_diagnostics::statistics_t frame_age_at_network_ms;  ///< Source timestamp age when network packetization starts.
  };

  /**
   * @brief Reset bounded pipeline counters at the start of a stream.
   */
  void reset_pipeline_diagnostics();

  /**
   * @brief Record one captured frame published to the one-frame encode handoff.
   *
   * @param timestamp Source frame timestamp when available.
   * @param replaced_pending Whether the previous pending frame was replaced.
   * @param queue_depth Captured frames waiting after publication.
   */
  void record_capture_enqueued(const std::optional<std::chrono::steady_clock::time_point> &timestamp, bool replaced_pending, std::size_t queue_depth = 1);

  /**
   * @brief Record removal of a captured frame for conversion and encoding.
   *
   * @param queue_depth Captured frames still waiting after removal.
   */
  void record_capture_dequeued(std::size_t queue_depth = 0);

  /**
   * @brief Record one PipeWire producer buffer superseded before capture.
   */
  void record_pipewire_buffer_replaced();

  /**
   * @brief Mark the beginning of one frame encode operation.
   *
   * @param timestamp Source frame timestamp when available.
   * @param unique_frame Whether this submission consumes a new source generation.
   */
  void record_encode_started(const std::optional<std::chrono::steady_clock::time_point> &timestamp, bool unique_frame);

  /**
   * @brief Mark completion of one frame encode operation.
   */
  void record_encode_finished();

  /**
   * @brief Record output deadlines discarded after a late pacer wakeup.
   *
   * @param count Number of elapsed deadlines intentionally skipped.
   */
  void record_capture_deadline_misses(std::uint64_t count);

  /**
   * @brief Record metadata classification for one PipeWire callback buffer.
   *
   * @param redundant_pts Whether the buffer repeats the preceding producer PTS.
   * @param no_damage Whether VideoDamage explicitly reports no changed region.
   */
  void record_pipewire_buffer(bool redundant_pts, bool no_damage);

  /**
   * @brief Record one valid unique PipeWire frame accepted by capture.
   *
   * @param timestamp Producer callback arrival time when available.
   */
  void record_pipewire_unique_frame(const std::optional<std::chrono::steady_clock::time_point> &timestamp = std::nullopt);

  /**
   * @brief Record one explicit overflow at the bounded PipeWire producer handoff.
   */
  void record_pipewire_queue_overflow();

  /**
   * @brief Record the client-requested rational output rate.
   *
   * @param numerator Requested FPS numerator.
   * @param denominator Requested FPS denominator.
   */
  void record_requested_frame_rate(std::uint32_t numerator, std::uint32_t denominator);

  /**
   * @brief Record the rational frame rates negotiated with PipeWire.
   *
   * @param negotiated_num PipeWire preferred FPS numerator.
   * @param negotiated_den PipeWire preferred FPS denominator.
   * @param maximum_num PipeWire maximum FPS numerator.
   * @param maximum_den PipeWire maximum FPS denominator.
   */
  void record_pipewire_negotiated_frame_rate(
    std::uint32_t negotiated_num,
    std::uint32_t negotiated_den,
    std::uint32_t maximum_num,
    std::uint32_t maximum_den
  );

  /**
   * @brief Record whether static keepalive suppression is currently active.
   *
   * @param static_mode True while duplicate output is reduced to keepalives.
   */
  void record_output_static_mode(bool static_mode);

  /**
   * @brief Record an encoded frame after it enters the ordered network queue.
   *
   * @param bytes Encoded payload bytes.
   * @param queue_frames Current bounded queue occupancy.
   */
  void record_network_enqueued(std::size_t bytes, std::size_t queue_frames);

  /**
   * @brief Record an encoded frame removed for packetization.
   *
   * @param bytes Encoded payload bytes.
   * @param queue_frames Remaining bounded queue occupancy.
   * @param timestamp Source frame timestamp when available.
   */
  void record_network_dequeued(std::size_t bytes, std::size_t queue_frames, const std::optional<std::chrono::steady_clock::time_point> &timestamp);

  /**
   * @brief Record the kernel output queue observed after sending a video frame.
   *
   * @param bytes Unsent bytes reported by the socket.
   */
  void record_socket_outq(std::uint64_t bytes);

  /**
   * @brief Record an accepted IDR request, rate-limiting duplicate client requests.
   *
   * Reconnect and recovery requests are never delayed.
   *
   * @param reason Reason supplied by the request source.
   * @return True when the encoder should receive this request.
   */
  bool record_idr_request(idr_reason_e reason);

  /**
   * @brief Associate the next emitted IDR with a request consumed by the encoder.
   *
   * @param reason Reason consumed immediately before forcing the next frame.
   */
  void record_idr_submitted(idr_reason_e reason);

  /**
   * @brief Record an encoded IDR and classify unrequested frames as periodic.
   *
   * @param emitted Whether the encoded frame is an IDR.
   */
  void record_idr_emitted(bool emitted);

  /**
   * @brief Return an atomic snapshot of current pipeline diagnostics.
   *
   * @return Bounded counters and latency statistics.
   */
  pipeline_diagnostics_t pipeline_diagnostics_snapshot();

  /**
   * @brief Pixel formats supported by one encoder backend.
   */
  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;  ///< Platform memory type required by this encoder.
    platf::pix_fmt_e pix_fmt_8bit;  ///< 8-bit 4:2:0 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_10bit;  ///< 10-bit 4:2:0 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_yuv444_8bit;  ///< 8-bit 4:4:4 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_yuv444_10bit;  ///< 10-bit 4:4:4 input format accepted by this encoder.
  };

  /**
   * @brief AVCodec-specific pixel formats supported by a platform.
   */
  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    /**
     * @brief Callback that prepares an FFmpeg hardware input buffer for the encode device.
     */
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    /**
     * @brief Construct AVCodec platform format mappings.
     *
     * @param avcodec_base_dev_type Base AVCodec hardware device type.
     * @param avcodec_derived_dev_type Derived AVCodec hardware device type.
     * @param avcodec_dev_pix_fmt AVCodec device pixel format.
     * @param avcodec_pix_fmt_8bit AVCodec 8-bit pixel format.
     * @param avcodec_pix_fmt_10bit AVCodec 10-bit pixel format.
     * @param avcodec_pix_fmt_yuv444_8bit AVCodec 8-bit YUV444 pixel format.
     * @param avcodec_pix_fmt_yuv444_10bit AVCodec 10-bit YUV444 pixel format.
     * @param init_avcodec_hardware_input_buffer_function Hardware input buffer initialization callback.
     */
    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_8bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function
    ):
        avcodec_base_dev_type {avcodec_base_dev_type},
        avcodec_derived_dev_type {avcodec_derived_dev_type},
        avcodec_dev_pix_fmt {avcodec_dev_pix_fmt},
        avcodec_pix_fmt_8bit {avcodec_pix_fmt_8bit},
        avcodec_pix_fmt_10bit {avcodec_pix_fmt_10bit},
        avcodec_pix_fmt_yuv444_8bit {avcodec_pix_fmt_yuv444_8bit},
        avcodec_pix_fmt_yuv444_10bit {avcodec_pix_fmt_yuv444_10bit},
        init_avcodec_hardware_input_buffer {init_avcodec_hardware_input_buffer_function} {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
      pix_fmt_yuv444_8bit = map_pix_fmt(avcodec_pix_fmt_yuv444_8bit);
      pix_fmt_yuv444_10bit = map_pix_fmt(avcodec_pix_fmt_yuv444_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type;  ///< FFmpeg device type used to create the primary hardware context.
    AVHWDeviceType avcodec_derived_dev_type;  ///< FFmpeg device type derived from the primary hardware context.
    AVPixelFormat avcodec_dev_pix_fmt;  ///< FFmpeg hardware-device pixel format for frames handed to the encoder.
    AVPixelFormat avcodec_pix_fmt_8bit;  ///< FFmpeg 8-bit 4:2:0 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_10bit;  ///< FFmpeg 10-bit 4:2:0 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_yuv444_8bit;  ///< FFmpeg 8-bit 4:4:4 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_yuv444_10bit;  ///< FFmpeg 10-bit 4:4:4 software pixel format for this encoder.

    init_buffer_function_t init_avcodec_hardware_input_buffer;  ///< Backend hook that allocates or imports hardware frames for FFmpeg.
  };

  /**
   * @brief NVENC-specific pixel formats supported by a platform.
   */
  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    /**
     * @brief Construct NVENC platform format mappings.
     *
     * @param dev_type Platform memory type.
     * @param pix_fmt_8bit Platform 8-bit pixel format.
     * @param pix_fmt_10bit Platform 10-bit pixel format.
     * @param pix_fmt_yuv444_8bit Platform 8-bit YUV444 pixel format.
     * @param pix_fmt_yuv444_10bit Platform 10-bit YUV444 pixel format.
     */
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  /**
   * @brief Encoder name and feature flags advertised by Sunshine.
   */
  struct encoder_t {
    std::string_view name;  ///< Encoder name used in logs, configuration, and capability probes.

    /**
     * @brief Capability flags that describe which stream modes an encoder supports.
     */
    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      DYNAMIC_RANGE,  ///< HDR support.
      YUV444,  ///< YUV 4:4:4 support.
      DYNAMIC_RANGE_YUV444,  ///< YUV 4:4:4 HDR support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    /**
     * @brief Convert an encoder capability flag to its diagnostic string.
     *
     * @param flag Feature flag to name.
     * @return Stable string used in logs and probe output.
     */
    static std::string_view from_flag(flag_e flag) {
#ifndef DOXYGEN
  #define _CONVERT(x) \
    case flag_e::x: \
      return std::string_view(#x)
#endif
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(YUV444);
        _CONVERT(DYNAMIC_RANGE_YUV444);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#undef _CONVERT

      return {"unknown"};
    }

    /**
     * @brief Runtime encoder option exposed to configuration parsing.
     */
    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      /**
       * @brief Copy an encoder option descriptor.
       */
      option_t(const option_t &) = default;

      std::string name;  ///< Encoder command-line option name.
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *, std::function<const std::string(const config_t &)>> value;  ///< Literal, pointer, or callback that supplies the option value.

      /**
       * @brief Store a named encoder option and its value source.
       *
       * @param name Human-readable name to assign.
       * @param value Literal value, pointer, or callback used to resolve the option.
       */
      option_t(std::string &&name, decltype(value) &&value):
          name {std::move(name)},
          value {std::move(value)} {
      }
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;  ///< Platform-specific memory and pixel formats accepted by the encoder.

    /**
     * @brief Codec capabilities for AV1, HEVC, or H.264.
     */
    struct codec_t {
      std::vector<option_t> common_options;  ///< Options applied to every encode mode for this codec.
      std::vector<option_t> sdr_options;  ///< Options applied to SDR 4:2:0 streams.
      std::vector<option_t> hdr_options;  ///< Options applied to HDR 4:2:0 streams.
      std::vector<option_t> sdr444_options;  ///< Options applied to SDR 4:4:4 streams.
      std::vector<option_t> hdr444_options;  ///< Options applied to HDR 4:4:4 streams.
      std::vector<option_t> fallback_options;  ///< Options used when the preferred mode cannot be selected.

      std::string name;  ///< Codec name passed to the encoder backend.
      std::bitset<MAX_FLAGS> capabilities;  ///< Capability flags supported by this codec on the encoder.

      /**
       * @brief Test whether a codec capability is enabled.
       *
       * @param flag Feature flag to test or modify.
       * @return True when the requested video flag is set.
       */
      bool operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      /**
       * @brief Return a mutable reference to a codec capability flag.
       *
       * @param flag Feature flag to test or modify.
       * @return Mutable bitset reference for the requested capability.
       */
      std::bitset<MAX_FLAGS>::reference operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    };

    codec_t av1;  ///< AV1 codec capability and option set.
    codec_t hevc;  ///< HEVC codec capability and option set.
    codec_t h264;  ///< H.264 codec capability and option set.

    /**
     * @brief Select the codec descriptor requested by a stream configuration.
     *
     * @param config Configuration values to apply.
     * @return Codec descriptor for H.264, HEVC, or AV1.
     */
    const codec_t &codec_from_config(const config_t &config) const {
      switch (config.videoFormat) {
        default:
          BOOST_LOG(error) << "Unknown video format " << config.videoFormat << ", falling back to H.264";
          // fallthrough
        case 0:
          return h264;
        case 1:
          return hevc;
        case 2:
          return av1;
      }
    }

    uint32_t flags;  ///< Encoder flags advertised to clients through GameStream capability responses.
  };

  /**
   * @brief Encoder session state shared by capture and encoding threads.
   */
  struct encode_session_t {
    virtual ~encode_session_t() = default;

    /**
     * @brief Convert a captured frame into the encoder's required input representation.
     *
     * @param img Captured image supplied by the platform display backend.
     * @return Zero when the frame was converted or imported successfully.
     */
    virtual int convert(platf::img_t &img) = 0;

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    virtual void request_idr_frame() = 0;

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    virtual void request_normal_frame() = 0;

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     * @return True when recovery forced a new IDR instead of invalidating references in place.
     */
    virtual bool invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
  extern encoder_t mediafoundation;
#endif

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  /**
   * @brief Encoded packet wrapper used by the streaming pipeline.
   */
  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    /**
     * @brief Report whether this packet starts an IDR frame.
     *
     * @return True when this frame is an IDR frame.
     */
    virtual bool is_idr() = 0;

    /**
     * @brief Return the frame index associated with this encoded packet.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    virtual int64_t frame_index() = 0;

    /**
     * @brief Return writable access to the encoded packet bytes.
     *
     * @return Pointer to the first byte of encoded frame data.
     */
    virtual uint8_t *data() = 0;

    /**
     * @brief Return the encoded payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    virtual size_t data_size() = 0;

    /**
     * @brief Packet byte-range replacement descriptor.
     */
    struct replace_t {
      std::string_view old;  ///< Byte sequence to find in the encoded packet.
      std::string_view _new;  ///< Byte sequence that replaces `old` before transmission.

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      /**
       * @brief Store an encoded-byte replacement rule.
       *
       * @param old Byte sequence to replace in encoded packets.
       * @param _new Replacement byte sequence inserted into encoded packets.
       */
      replace_t(std::string_view old, std::string_view _new) noexcept:
          old {std::move(old)},
          _new {std::move(_new)} {
      }
    };

    std::vector<replace_t> *replacements = nullptr;  ///< Optional encoded-byte substitutions applied before packetization.
    void *channel_data = nullptr;  ///< Platform or protocol state carried with this packet.
    bool after_ref_frame_invalidation = false;  ///< Whether the frame follows reference-frame invalidation.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Capture timestamp associated with the frame.
  };

  /**
   * @brief AVCodec packet wrapper with codec-specific metadata.
   */
  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    /**
     * @brief Report whether the FFmpeg packet is marked as a key frame.
     *
     * @return True when this frame is an IDR frame.
     */
    bool is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    /**
     * @brief Return the FFmpeg packet PTS used as Sunshine's frame index.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    int64_t frame_index() override {
      return av_packet->pts;
    }

    /**
     * @brief Return writable access to the FFmpeg packet payload.
     *
     * @return Pointer to the first encoded byte in the FFmpeg packet.
     */
    uint8_t *data() override {
      return av_packet->data;
    }

    /**
     * @brief Return the FFmpeg packet payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    size_t data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;  ///< FFmpeg packet that owns encoded data and frame metadata.
  };

  /**
   * @brief Generic encoded packet bytes and metadata.
   */
  struct packet_raw_generic: packet_raw_t {
    /**
     * @brief Wrap generic encoded frame bytes in Sunshine packet metadata.
     *
     * @param frame_data Encoded frame bytes produced by a non-FFmpeg encoder.
     * @param frame_index Monotonic frame index assigned to the encoded frame.
     * @param idr Whether the packet begins an IDR frame.
     */
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data {std::move(frame_data)},
        index {frame_index},
        idr {idr} {
    }

    /**
     * @brief Report whether the generic packet starts an IDR frame.
     *
     * @return True when this frame is an IDR frame.
     */
    bool is_idr() override {
      return idr;
    }

    /**
     * @brief Return the stored frame index for this generic packet.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    int64_t frame_index() override {
      return index;
    }

    /**
     * @brief Return writable access to the generic packet payload.
     *
     * @return Pointer to the first encoded byte in `frame_data`.
     */
    uint8_t *data() override {
      return frame_data.data();
    }

    /**
     * @brief Return the generic packet payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    size_t data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;  ///< Encoded frame bytes owned by this packet.
    int64_t index;  ///< Monotonic frame index assigned to this packet.
    bool idr;  ///< Whether the packet belongs to an IDR frame.
  };

  /**
   * @brief Owning pointer to an encoded packet abstraction.
   */
  using packet_t = std::unique_ptr<packet_raw_t>;

  /**
   * @brief Raw HDR metadata fields parsed from the display backend.
   */
  struct hdr_info_raw_t {
    /**
     * @brief Initialize HDR metadata with only the enabled state.
     *
     * @param enabled Whether the feature should be enabled.
     */
    explicit hdr_info_raw_t(bool enabled):
        enabled {enabled},
        metadata {} {};
    /**
     * @brief Initialize HDR metadata with display metadata from the backend.
     *
     * @param enabled Whether the feature should be enabled.
     * @param metadata Output structure populated with HDR metadata.
     */
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled {enabled},
        metadata {metadata} {};

    bool enabled;  ///< Whether HDR mode should be enabled.
    SS_HDR_METADATA metadata;  ///< Display HDR metadata forwarded to the encoder and client.
  };

  /**
   * @brief Owning pointer to optional HDR metadata for a captured display.
   */
  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  extern int active_hevc_mode;
  extern int active_av1_mode;
  extern bool last_encoder_probe_supported_ref_frames_invalidation;
  extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1

  /**
   * @brief Check whether encoder capability probing is currently active.
   *
   * Capture backends may use this to avoid consuming edge-triggered producer
   * frames when codec validation only needs a dummy image and GPU device.
   *
   * @return True while @ref probe_encoders is validating encoder capabilities.
   */
  bool encoder_probe_in_progress();

  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  );

  /**
   * @brief Validate encoder before it is used.
   *
   * @param encoder Encoder configuration or encoder instance.
   * @param expect_failure Expect failure.
   * @return True when encoder validation matches `expect_failure`.
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure);

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   * @return 0 when a usable encoder is selected; nonzero when probing fails.
   */
  int probe_encoders();

  // Several NTSC standard refresh rates are hardcoded here, because their
  // true rate requires a denominator of 1001. ffmpeg's av_d2q() would assume it could
  // reduce 29.97 to 2997/100 but this would be slightly wrong. We also include
  // support for 23.976 film in case someone wants to stream a film at the perfect
  // framerate.
  /**
   * @brief Convert a framerate stored as hundredths of Hz to an FFmpeg rational.
   *
   * @param framerateX100 Framerate multiplied by 100, such as 5994 for 59.94 Hz.
   * @return Rational frame rate preserving NTSC fractional rates when applicable.
   */
  inline AVRational framerateX100_to_rational(const int framerateX100) {
    if (framerateX100 % 2997 == 0) {
      // Multiples of NTSC 29.97 e.g. 59.94, 119.88
      return AVRational {(framerateX100 / 2997) * 30000, 1001};
    }
    switch (framerateX100) {
      case 2397:  // the other weird NTSC framerate, assume these want 23.976 film
      case 2398:
        return AVRational {24000, 1001};
      default:
        // any other fractional rate can be reduced by ffmpeg. Max is set to 1 << 26 based on docs:
        // "rational numbers with |num| <= 1<<26 && |den| <= 1<<26 can be recovered exactly from their double representation"
        return av_d2q((double) framerateX100 / 100.0f, 1 << 26);
    }
  }

  /**
   * @brief Requested framerate as an exact rational.
   * Uses the exact fractional rate when the client provided an X100 value,
   * otherwise the integer framerate over 1.
   *
   * @param config Stream configuration carrying the requested framerate.
   * @return Requested frame rate as a rational.
   */
  inline AVRational framerate_to_rational(const config_t &config) {
    if (config.framerateX100 > 0) {
      return framerateX100_to_rational(config.framerateX100);
    }
    return AVRational {config.framerate, 1};
  }

  /**
   * @brief Capture frame interval for the requested framerate.
   * Uses the exact fractional rate when the client provided an X100 value.
   *
   * @param config Stream configuration carrying the requested framerate.
   * @return Duration of a single frame interval in nanoseconds.
   */
  inline std::chrono::nanoseconds capture_frame_interval(const config_t &config) {
    const AVRational fps = framerate_to_rational(config);
    return std::chrono::nanoseconds {(static_cast<int64_t>(fps.den) * 1'000'000'000LL) / fps.num};
  }
}  // namespace video
