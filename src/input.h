/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "latency_diagnostics.h"
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  /**
   * @brief Result of adding one decoded packet to the bounded input queue.
   */
  struct packet_enqueue_result_t {
    bool accepted {false};  ///< Whether the packet was accepted while the queue was running.
    bool schedule_worker {false};  ///< Whether the caller must schedule the sole queue worker.
    std::uint64_t coalesced {0};  ///< Motion packets combined while accepting this packet.
    std::uint64_t dropped {0};  ///< Stale motion packets discarded to preserve the queue bound.
  };

  /**
   * @brief One decoded input packet removed from the bounded input queue.
   */
  struct queued_packet_t {
    std::vector<std::uint8_t> data;  ///< Protocol packet storage.
    std::chrono::steady_clock::time_point received_at;  ///< Decode-complete queue insertion time.
    std::uint64_t coalesced {0};  ///< Later motion packets combined into this packet.
  };

  /**
   * @brief Bounded, edge-preserving queue for decoded client input packets.
   *
   * Motion is coalesced before consuming capacity. On overflow, only stale
   * motion packets that cannot carry a key, button, or touch edge are removed.
   * If the queue contains only edge-bearing packets, the producer applies
   * backpressure until the single consumer makes room.
   */
  class packet_queue_t {
  public:
    /**
     * @brief Construct an accepting queue with a fixed packet bound.
     *
     * @param capacity Maximum number of decoded packets retained at once.
     */
    explicit packet_queue_t(std::size_t capacity = 64);

    /**
     * @brief Destroy the packet queue implementation.
     */
    ~packet_queue_t();

    packet_queue_t(const packet_queue_t &) = delete;
    packet_queue_t &operator=(const packet_queue_t &) = delete;
    packet_queue_t(packet_queue_t &&) = delete;
    packet_queue_t &operator=(packet_queue_t &&) = delete;

    /**
     * @brief Queue or coalesce a decoded input packet.
     *
     * @param packet Decoded Moonlight input packet.
     * @return Queue mutations and whether the sole worker must be scheduled.
     */
    packet_enqueue_result_t push(std::vector<std::uint8_t> &&packet);

    /**
     * @brief Pop the oldest packet and coalesce compatible queued motion into it.
     *
     * @return Packet to inject, or no value when the queue is empty.
     */
    std::optional<queued_packet_t> pop();

    /**
     * @brief Complete one worker invocation and decide whether another is needed.
     *
     * @return True when queued input remains and the caller must reschedule.
     */
    bool worker_finished();

    /**
     * @brief Stop accepting packets, discard pending input, and wake producers.
     */
    void stop();

    /**
     * @brief Return the current number of retained packets.
     *
     * @return Queue depth.
     */
    std::size_t size() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Private synchronization and packet storage.
  };

  /**
   * @brief Lock-free input-path counters for the active stream.
   *
   * Counters are reset when a stream input context is allocated. They are
   * intentionally aggregate-only so collecting them never logs individual
   * input events or retains client data.
   */
  struct diagnostics_snapshot_t {
    uint64_t events_received {0};  ///< Raw client input packets received.
    uint64_t events_injected {0};  ///< Packets delivered to the platform input backend.
    uint64_t motion_coalesced {0};  ///< Motion packets combined before injection.
    uint64_t motion_dropped {0};  ///< Motion packets dropped by a bounded queue.
    uint64_t queue_current {0};  ///< Input packets currently awaiting injection.
    uint64_t queue_max {0};  ///< Greatest observed input queue depth.
    latency_diagnostics::statistics_t queue_age_ms;  ///< T2-to-T3 decoded-queue latency statistics.
    std::string route_target;  ///< Current platform input destination.
    std::string route_error;  ///< Machine-readable fail-closed reason, if any.
  };

  /**
   * @brief Return a consistent aggregate view of active-stream input counters.
   *
   * @return Non-secret input queue and delivery counters.
   */
  diagnostics_snapshot_t diagnostics_snapshot();

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * @return Cleanup handle for initialized input resources, or null if none are required.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Probe whether the platform can create virtual gamepads.
   *
   * @return True when at least one configured gamepad backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.
    bool reject_client_margins;  ///< Drop absolute input in letterbox or pillarbox margins instead of clamping it.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
