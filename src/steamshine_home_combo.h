/**
 * @file src/steamshine_home_combo.h
 * @brief Configurable button combinations that emulate the gamepad Home button.
 *
 * Touch controllers in mobile Moonlight clients usually have no Guide button.
 * A combination such as Start + Back held for a configured time is turned into
 * one Home press. After it fires, the combination's buttons are held released
 * for the host until the player lets go, so games never see the long press.
 */
#pragma once

#include "platform/common.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steamshine_home_combo {

  /**
   * @brief Minimum accepted hold time.
   */
  constexpr std::chrono::milliseconds MIN_HOLD {200};

  /**
   * @brief Maximum accepted hold time.
   */
  constexpr std::chrono::milliseconds MAX_HOLD {10000};

  /**
   * @brief Hold time used when the configuration omits one.
   */
  constexpr std::chrono::milliseconds DEFAULT_HOLD {1000};

  /**
   * @brief Largest number of inputs in one combination.
   */
  constexpr std::size_t MAX_INPUTS {4};

  /**
   * @brief Trigger value at or above which a trigger counts as pressed.
   */
  constexpr std::uint8_t TRIGGER_THRESHOLD {128};

  /**
   * @brief One validated Home-button combination.
   */
  struct combo_t {
    std::uint32_t buttons {0};  ///< Required Moonlight button mask bits.
    bool left_trigger {false};  ///< Whether the left trigger must be pressed.
    bool right_trigger {false};  ///< Whether the right trigger must be pressed.
    std::chrono::milliseconds hold {DEFAULT_HOLD};  ///< Continuous hold time before Home fires.

    /**
     * @brief Report whether the combination requires at least one input.
     *
     * @return True when the combination can fire.
     */
    [[nodiscard]] bool enabled() const;
  };

  /**
   * @brief Per-gamepad state that suppresses a fired combination until release.
   */
  struct tracker_t {
    std::uint32_t suppressed_buttons {0};  ///< Buttons hidden from the host until released.
    bool suppress_left_trigger {false};  ///< Whether the left trigger is hidden until released.
    bool suppress_right_trigger {false};  ///< Whether the right trigger is hidden until released.
    bool timer_armed {false};  ///< Whether a hold timer is pending for this gamepad.
  };

  /**
   * @brief Timer change requested after filtering one gamepad packet.
   */
  enum class timer_action_e {
    none,  ///< Leave any pending timer unchanged.
    arm,  ///< Start the hold timer.
    cancel,  ///< Cancel the pending hold timer.
  };

  /**
   * @brief Return the stable input names accepted in a combination, in display order.
   *
   * @return Input names such as `START`, `BACK`, `LT`.
   */
  const std::vector<std::string_view> &input_names();

  /**
   * @brief Parse a `+`-separated input list and hold time into a combination.
   *
   * An empty list yields a disabled combination. Names are case-insensitive;
   * unknown or duplicate names, more than MAX_INPUTS inputs, and hold times
   * outside MIN_HOLD..MAX_HOLD are rejected.
   *
   * @param inputs Input list such as `START+BACK`.
   * @param hold_ms Hold time in milliseconds.
   * @return Parsed combination, or no value when invalid.
   */
  std::optional<combo_t> parse(std::string_view inputs, int hold_ms);

  /**
   * @brief Format a combination's inputs in canonical order.
   *
   * @param combo Combination to format.
   * @return `+`-separated input names, or an empty string when disabled.
   */
  std::string format_inputs(const combo_t &combo);

  /**
   * @brief Test whether every input of a combination is pressed.
   *
   * @param combo Combination to test.
   * @param state Raw client gamepad state.
   * @return True when the combination is enabled and fully held.
   */
  bool held(const combo_t &combo, const platf::gamepad_state_t &state);

  /**
   * @brief Filter one raw gamepad packet and decide how the hold timer changes.
   *
   * Suppressed inputs are removed from @p state and released from suppression
   * once the client reports them released.
   *
   * @param tracker Per-gamepad combination state.
   * @param combo Active combination.
   * @param state Raw client state on entry; host-visible state on return.
   * @return Timer change required for this packet.
   */
  timer_action_e filter(tracker_t &tracker, const combo_t &combo, platf::gamepad_state_t &state);

  /**
   * @brief Hide a fired combination's inputs from the host until they are released.
   *
   * @param tracker Per-gamepad combination state.
   * @param combo Combination that fired.
   * @param state Host-visible state to release the inputs in.
   */
  void fire(tracker_t &tracker, const combo_t &combo, platf::gamepad_state_t &state);

  /**
   * @brief Return the active combination.
   *
   * The first call loads the persisted configuration; later calls return the
   * value most recently published by set().
   *
   * @return Active combination.
   */
  combo_t current();

  /**
   * @brief Persist and publish a new combination without restarting the service.
   *
   * @param combo Validated combination; a disabled combination turns the feature off.
   * @param error Failure reason when the configuration could not be replaced.
   * @return True after the configuration file was replaced and the value published.
   */
  bool set(const combo_t &combo, std::string &error);

}  // namespace steamshine_home_combo
