/**
 * @file src/stream_recording.h
 * @brief Sender-side encoded stream recording and bounded retention.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace stream_recording {
  /**
   * @brief Default total recording capacity in megabytes.
   */
  constexpr std::uint64_t DEFAULT_CAPACITY_MB = 500;

  /**
   * @brief Stable result returned by one recording operation.
   */
  struct result_t {
    bool success;  ///< Whether the operation completed successfully.
    std::string code;  ///< Stable machine-readable result code.
    std::string message;  ///< Safe user-facing result message.
  };

  /**
   * @brief Metadata required to preserve an encoded sender frame.
   */
  struct frame_t {
    std::span<const std::uint8_t> payload;  ///< Final encoded bytes used for network packetization.
    std::uintptr_t stream_id;  ///< Process-local identity of the stream that produced the frame.
    std::int64_t frame_index;  ///< Monotonic sender frame index.
    int codec;  ///< GameStream codec identifier: H.264=0, HEVC=1, AV1=2.
    int width;  ///< Encoded width in pixels.
    int height;  ///< Encoded height in pixels.
    int fps;  ///< Nominal encoded frame rate.
    bool hdr;  ///< Whether the encoded stream carries HDR video.
    bool idr;  ///< Whether this frame is independently decodable.
  };

  /**
   * @brief Records final sender packets without adding another video encoder.
   */
  class Service {
  public:
    /**
     * @brief Construct a recording service.
     *
     * @param root Owner-private recording directory, or empty for the default state path.
     * @param ffmpeg_executable FFmpeg executable used only for lossless container muxing.
     */
    explicit Service(std::filesystem::path root = {}, std::string ffmpeg_executable = "ffmpeg");

    /**
     * @brief Stop the worker and finalize an active recording.
     */
    ~Service();

    /** @brief Recording services cannot share worker ownership. */
    Service(const Service &) = delete;

    /** @brief Recording services cannot share worker ownership. */
    Service &operator=(const Service &) = delete;

    /**
     * @brief Arm or stop sender-side recording.
     *
     * @param enabled True to wait for the next IDR, false to finalize the current recording.
     * @return Stable operation result.
     */
    result_t set_enabled(bool enabled);

    /**
     * @brief Update total retained recording capacity and immediately enforce it.
     *
     * @param capacity_mb Capacity in binary megabytes.
     * @return Stable operation result.
     */
    result_t set_capacity_megabytes(std::uint64_t capacity_mb);

    /**
     * @brief Queue one final encoded sender frame without blocking the network sender on disk I/O.
     *
     * @param frame Encoded bytes and stream metadata valid for the duration of this call.
     */
    void submit(const frame_t &frame);

    /**
     * @brief Finalize recording when the media session ends.
     *
     * @param stream_id Process-local identity of the ending stream.
     */
    void stream_ended(std::uintptr_t stream_id);

    /**
     * @brief Return current state, capacity usage, and completed recordings.
     *
     * @return Public bounded JSON snapshot.
     */
    nlohmann::json snapshot() const;

    /**
     * @brief Delete one completed recording selected by its generated identifier.
     *
     * @param id Generated recording identifier.
     * @return Stable operation result.
     */
    result_t remove(std::string_view id);

    /**
     * @brief Resolve one completed recording without accepting arbitrary paths.
     *
     * @param id Generated recording identifier.
     * @return Absolute regular-file path, or an empty path when unavailable.
     */
    std::filesystem::path resolve(std::string_view id) const;

  private:
    class Impl;  ///< Platform implementation hidden from Web and media callers.
    std::unique_ptr<Impl> impl_;  ///< Owned recording worker and storage state.
  };

  /**
   * @brief Return the process-wide sender recording service.
   *
   * @return Shared recording service used by Web handlers and the video sender.
   */
  Service &service();
}  // namespace stream_recording
