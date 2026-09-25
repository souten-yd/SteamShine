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
#include <span>
#include <string>
#include <string_view>
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

    /** @brief Resume accepting packets after the retained client reconnects. */
    void resume();

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
   * @brief Hold-to-release edges observed between two controller states.
   *
   * The summary deliberately records only release categories and a button
   * mask. It is suitable for low-volume diagnostics without sampling a
   * player's continuous controller motion.
   */
  struct gamepad_hold_release_t {
    std::uint32_t released_buttons {0};  ///< Buttons that changed from pressed to released.
    bool left_stick_released {false};  ///< Whether the left stick returned from a held direction to neutral.
    bool right_stick_released {false};  ///< Whether the right stick returned from a held direction to neutral.

    /**
     * @brief Report whether any held control was released.
     *
     * @return True when a button or stick release edge is present.
     */
    bool any() const {
      return released_buttons != 0 || left_stick_released || right_stick_released;
    }
  };

  /**
   * @brief Detect release edges that can interrupt a held game action.
   *
   * Stick classification uses separate held and neutral thresholds so small
   * center noise cannot create a false release event.
   *
   * @param previous Previously injected controller state.
   * @param current Controller state about to be injected.
   * @param held_threshold Minimum absolute axis magnitude considered held.
   * @param neutral_threshold Maximum absolute axis magnitude considered neutral.
   * @return Released button mask and stick release categories.
   */
  gamepad_hold_release_t detect_gamepad_hold_release(
    const platf::gamepad_state_t &previous,
    const platf::gamepad_state_t &current,
    std::int16_t held_threshold = 4096,
    std::int16_t neutral_threshold = 1024
  );

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
   * @brief Destroy every retained virtual gamepad session.
   *
   * Retained gamepads survive a paused transport connection so they can be reused on resume. Call this when the
   * streamed application or all streaming sessions are explicitly terminated.
   */
  void terminate_gamepads();

  /**
   * @brief Destroy virtual gamepads retained for one paired client.
   *
   * @param session_id Stable paired-client identity used by alloc().
   */
  void terminate_gamepads(std::string_view session_id);

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
   * @brief Recreate shared libvirtualhid keyboard and mouse devices after a license-state change.
   *
   * The work is serialized with streamed input so both backends can switch
   * safely between the Windows HID and SendInput paths.
   */
  void refresh_virtual_input();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param session_id Stable paired-client identity shared by launch and resume connections.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Replace the global platform input backend for a unit test.
     *
     * @param input Test-owned platform input backend.
     */
    void set_platform_input(platf::input_t input);

    /**
     * @brief Allocate a gamepad directly in retained input state for a unit test.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param metadata Client-reported controller metadata.
     * @return Assigned global gamepad slot, or -1 on failure.
     */
    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata);

    /**
     * @brief Return the global gamepad slot stored for a test controller.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @return Assigned global gamepad slot, or -1 when unallocated.
     */
    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index);

    /**
     * @brief Keyboard event Sunshine emitted toward the platform backend.
     */
    struct keyboard_event_t {
      std::uint16_t key_code;  ///< Platform keycode after the configured keybinding remap.
      bool release;  ///< Whether the event releases the key.
      std::uint8_t flags;  ///< Bit flags carried by the client keyboard packet.
    };

    /**
     * @brief Redirect keyboard output away from the host operating system.
     *
     * Tests must install a sink before emitting keys, otherwise the events are typed into the
     * machine running the test suite.
     *
     * @param sink Recorder invoked in place of platf::keyboard_update, or empty to restore
     *             delivery to the platform backend.
     */
    void set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink);

    /**
     * @brief Process one client keyboard packet on the calling thread.
     *
     * @param input Retained input state.
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Client modifier bitmask carried by the packet.
     * @param flags Bit flags carried by the client keyboard packet.
     * @param release Whether the packet releases the key.
     */
    void send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code, std::uint8_t modifiers, std::uint8_t flags, bool release);

    /**
     * @brief Forget every key Sunshine tracks as pressed and cancel any pending key repeat.
     */
    void reset_keyboard_state();

    /**
     * @brief Release every key Sunshine tracks as pressed, as a disconnect does.
     */
    void release_held_keys();

    /**
     * @brief Validate raw protocol input bytes for a unit test.
     *
     * @param packet Raw packet bytes.
     * @return True when the packet is safe for typed processing.
     */
    bool is_valid_input_packet(std::span<const std::uint8_t> packet);

    /**
     * @brief Return the number of validated packets waiting in a test input queue.
     *
     * @param input Shared stream input state.
     * @return Number of queued packets, or zero for an empty input pointer.
     */
    std::size_t queued_input_packet_count(const std::shared_ptr<input_t> &input);
  }  // namespace testing
#endif

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
