/**
 * @file src/steamshine_gamepad_shortcuts.h
 * @brief Configurable button combinations that emulate Steam's Home and Quick Access buttons.
 *
 * Touch controllers in mobile Moonlight clients usually have no Guide button.
 * A combination such as Start + Back held for a configured time is turned into
 * one Home press, and another into Home + A, which opens Steam's Quick Access
 * menu where Decky Loader lives. After a shortcut fires, its inputs are held
 * released for the host until the player lets go, so games never see the long
 * press and no shortcut can repeat while the inputs stay down.
 */
#pragma once

#include "platform/common.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steamshine_gamepad_shortcuts {

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
   * @brief Steam button actions a combination can emulate.
   */
  enum class action_e : std::size_t {
    home,  ///< Press Home once.
    quick_access,  ///< Hold Home and press A, opening Steam's Quick Access menu.
  };

  /**
   * @brief Number of supported actions.
   */
  constexpr std::size_t ACTION_COUNT {2};

  /**
   * @brief One validated input combination.
   */
  struct combo_t {
    std::uint32_t buttons {0};  ///< Required Moonlight button mask bits.
    bool left_trigger {false};  ///< Whether the left trigger must be pressed.
    bool right_trigger {false};  ///< Whether the right trigger must be pressed.
    std::chrono::milliseconds hold {DEFAULT_HOLD};  ///< Continuous hold time before the action fires.

    /**
     * @brief Report whether the combination requires at least one input.
     *
     * @return True when the combination can fire.
     */
    [[nodiscard]] bool enabled() const;

    /**
     * @brief Report whether two combinations require exactly the same inputs.
     *
     * @param other Combination to compare.
     * @return True when the input sets match, regardless of hold time.
     */
    [[nodiscard]] bool same_inputs(const combo_t &other) const;
  };

  /**
   * @brief Combinations for every action, indexed by action_e.
   */
  using shortcuts_t = std::array<combo_t, ACTION_COUNT>;

  /**
   * @brief Per-gamepad state shared by every action.
   */
  struct tracker_t {
    std::uint32_t suppressed_buttons {0};  ///< Buttons hidden from the host until released.
    bool suppress_left_trigger {false};  ///< Whether the left trigger is hidden until released.
    bool suppress_right_trigger {false};  ///< Whether the right trigger is hidden until released.
    std::array<bool, ACTION_COUNT> armed {};  ///< Whether each action's hold timer is pending.
  };

  /**
   * @brief Timer change requested for one action after filtering a gamepad packet.
   */
  enum class timer_action_e {
    none,  ///< Leave any pending timer unchanged.
    arm,  ///< Start the hold timer.
    cancel,  ///< Cancel the pending hold timer.
  };

  /**
   * @brief Timer changes for every action, indexed by action_e.
   */
  using timer_actions_t = std::array<timer_action_e, ACTION_COUNT>;

  /**
   * @brief Return the stable configuration name of an action.
   *
   * @param action Action to name.
   * @return `home` or `quick_access`.
   */
  std::string_view action_name(action_e action);

  /**
   * @brief Look up an action by its configuration name.
   *
   * @param name Action name.
   * @return Matching action, or no value when unknown.
   */
  std::optional<action_e> parse_action(std::string_view name);

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
   * @brief Filter one raw gamepad packet and decide how each hold timer changes.
   *
   * Suppressed inputs are removed from @p state and released from suppression
   * once the client reports them released. An action arms only when none of its
   * inputs are suppressed and no other held action requires a strict superset
   * of its inputs, so Start + Back + A does not also fire Start + Back.
   *
   * @param tracker Per-gamepad shortcut state.
   * @param shortcuts Active combinations.
   * @param state Raw client state on entry; host-visible state on return.
   * @return Timer change required for each action.
   */
  timer_actions_t filter(tracker_t &tracker, const shortcuts_t &shortcuts, platf::gamepad_state_t &state);

  /**
   * @brief Hide a fired action's inputs from the host until they are released.
   *
   * Any other armed action sharing one of those inputs is disarmed, because the
   * client sends no further packet while the inputs stay held.
   *
   * @param tracker Per-gamepad shortcut state.
   * @param shortcuts Active combinations.
   * @param action Action that fired.
   * @param state Host-visible state to release the inputs in.
   */
  void fire(tracker_t &tracker, const shortcuts_t &shortcuts, action_e action, platf::gamepad_state_t &state);

  /**
   * @brief Return the active combinations.
   *
   * The first call loads the persisted configuration; later calls return the
   * values most recently published by set().
   *
   * @return Active combinations.
   */
  shortcuts_t current();

  /**
   * @brief Persist and publish one action's combination without restarting the service.
   *
   * @param action Action to change.
   * @param combo Validated combination; a disabled combination turns the action off.
   * @param error Failure reason when the combination conflicts or cannot be saved.
   * @return True after the configuration file was replaced and the value published.
   */
  bool set(action_e action, const combo_t &combo, std::string &error);

}  // namespace steamshine_gamepad_shortcuts
