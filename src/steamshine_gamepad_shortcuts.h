/**
 * @file src/steamshine_gamepad_shortcuts.h
 * @brief User-defined controller shortcuts that send other gamepad keys when a combination is held.
 *
 * Touch controllers in mobile Moonlight clients usually have no Guide button.
 * Each shortcut turns a held input combination, such as Start + Back for three
 * seconds, into a short sequence of other keys, such as Home or Home + A (Steam's
 * Quick Access menu, where Decky Loader lives). After a shortcut fires, its
 * inputs are held released for the host until the player lets go, so games
 * never see the long press and no shortcut repeats while the inputs stay down.
 */
#pragma once

#include "platform/common.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
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
   * @brief Largest number of inputs in one combination or output.
   */
  constexpr std::size_t MAX_INPUTS {4};

  /**
   * @brief Largest number of saved shortcuts.
   */
  constexpr std::size_t MAX_SHORTCUTS {8};

  /**
   * @brief Longest accepted shortcut name, in bytes.
   */
  constexpr std::size_t MAX_NAME_LENGTH {40};

  /**
   * @brief Trigger value at or above which a trigger counts as pressed.
   */
  constexpr std::uint8_t TRIGGER_THRESHOLD {128};

  /**
   * @brief Delay between consecutive output key changes.
   */
  constexpr std::chrono::milliseconds OUTPUT_STEP {100};

  /**
   * @brief One validated set of held inputs.
   */
  struct combo_t {
    std::uint32_t buttons {0};  ///< Required Moonlight button mask bits.
    bool left_trigger {false};  ///< Whether the left trigger must be pressed.
    bool right_trigger {false};  ///< Whether the right trigger must be pressed.
    std::chrono::milliseconds hold {1000};  ///< Continuous hold time before the shortcut fires.

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
   * @brief Ordered keys a shortcut sends, pressed in order and released in reverse.
   */
  struct output_t {
    std::vector<std::string> keys;  ///< Canonical key names such as `HOME` and `A`.
  };

  /**
   * @brief One user-defined shortcut.
   */
  struct shortcut_t {
    std::string id;  ///< Stable identifier used by the Web API.
    std::string name;  ///< Optional display name.
    combo_t trigger;  ///< Inputs to hold and for how long.
    output_t output;  ///< Keys sent when the shortcut fires.
    bool enabled {true};  ///< Whether the shortcut is active.
  };

  /**
   * @brief Per-gamepad state shared by every shortcut.
   */
  struct tracker_t {
    std::uint32_t suppressed_buttons {0};  ///< Buttons hidden from the host until released.
    bool suppress_left_trigger {false};  ///< Whether the left trigger is hidden until released.
    bool suppress_right_trigger {false};  ///< Whether the right trigger is hidden until released.
    std::set<std::string, std::less<>> armed;  ///< Identifiers of shortcuts whose hold timer is pending.
  };

  /**
   * @brief Timer change requested for one shortcut after filtering a gamepad packet.
   */
  struct timer_change_t {
    std::string id;  ///< Shortcut identifier.
    bool arm {false};  ///< True to start the hold timer, false to cancel it.

    /**
     * @brief Compare two timer changes.
     *
     * @param other Change to compare.
     * @return True when both refer to the same shortcut and change.
     */
    bool operator==(const timer_change_t &other) const = default;
  };

  /**
   * @brief Look up the button mask bit of a non-trigger input name.
   *
   * @param name Input name such as `A`; case-insensitive.
   * @return Button bit, or no value for triggers and unknown names.
   */
  std::optional<std::uint32_t> button_bit(std::string_view name);

  /**
   * @brief Return the input name of a single button mask bit.
   *
   * @param bit One button bit.
   * @return Input name, or an empty view when the bit is not a named input.
   */
  std::string_view button_name(std::uint32_t bit);

  /**
   * @brief Atomically replace one configuration value, keeping every other setting.
   *
   * Shared by controller features that own a single configuration key.
   *
   * @param key Configuration key to write.
   * @param value New value.
   * @param obsolete Keys to remove in the same replacement.
   * @param error Failure reason, leaving the previous file intact.
   * @return True after the file was replaced.
   */
  bool write_config_value(std::string_view key, const std::string &value, const std::vector<std::string_view> &obsolete, std::string &error);

  /**
   * @brief Return the stable input names accepted in a held combination, in display order.
   *
   * @return Input names such as `START`, `BACK`, `LT`.
   */
  const std::vector<std::string_view> &input_names();

  /**
   * @brief Return the key names a shortcut may send, in display order.
   *
   * @return `HOME` followed by every held-input name.
   */
  const std::vector<std::string_view> &output_names();

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
   * @brief Parse an ordered, `+`-separated key list that a shortcut sends.
   *
   * @param keys Key list such as `HOME+A`; order is preserved.
   * @return Parsed output, or no value when empty, unknown, duplicated, or too long.
   */
  std::optional<output_t> parse_output(std::string_view keys);

  /**
   * @brief Format an output's keys in send order.
   *
   * @param output Output to format.
   * @return `+`-separated key names.
   */
  std::string format_output(const output_t &output);

  /**
   * @brief Build the host-visible states that send an output.
   *
   * Keys are pressed one per step in order, then released one per step in
   * reverse order. Callers send each state OUTPUT_STEP apart.
   *
   * @param output Keys to send.
   * @param base Host-visible state to add the keys to.
   * @return States to send, ending with @p base restored.
   */
  std::vector<platf::gamepad_state_t> output_frames(const output_t &output, const platf::gamepad_state_t &base);

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
   * once the client reports them released. An enabled shortcut arms only when
   * none of its inputs are suppressed and no other held shortcut requires a
   * strict superset of its inputs, so Start + Back + A does not also fire
   * Start + Back. Armed shortcuts that were removed or disabled are cancelled.
   *
   * @param tracker Per-gamepad shortcut state.
   * @param shortcuts Configured shortcuts.
   * @param state Raw client state on entry; host-visible state on return.
   * @return Timer changes in shortcut order.
   */
  std::vector<timer_change_t> filter(tracker_t &tracker, const std::vector<shortcut_t> &shortcuts, platf::gamepad_state_t &state);

  /**
   * @brief Hide a fired shortcut's inputs from the host until they are released.
   *
   * Any other armed shortcut sharing one of those inputs is disarmed, because
   * the client sends no further packet while the inputs stay held.
   *
   * @param tracker Per-gamepad shortcut state.
   * @param shortcuts Configured shortcuts.
   * @param fired Shortcut that fired.
   * @param state Host-visible state to release the inputs in.
   */
  void fire(tracker_t &tracker, const std::vector<shortcut_t> &shortcuts, const shortcut_t &fired, platf::gamepad_state_t &state);

  /**
   * @brief Validate a complete shortcut list.
   *
   * @param shortcuts Candidate list.
   * @param error Reason the list is invalid.
   * @return True when every shortcut is complete, identifiers are unique, the
   * count is within MAX_SHORTCUTS, and no two enabled shortcuts hold the same inputs.
   */
  bool validate(const std::vector<shortcut_t> &shortcuts, std::string &error);

  /**
   * @brief Serialize shortcuts to the persisted JSON array.
   *
   * @param shortcuts Shortcuts to serialize.
   * @return Compact JSON text.
   */
  std::string to_json(const std::vector<shortcut_t> &shortcuts);

  /**
   * @brief Parse and validate the persisted JSON array.
   *
   * @param text JSON text; empty yields no shortcuts.
   * @return Shortcuts, or no value when the text is malformed or invalid.
   */
  std::optional<std::vector<shortcut_t>> from_json(std::string_view text);

  /**
   * @brief Return the configured shortcuts.
   *
   * The first call loads the persisted configuration; later calls return the
   * list most recently published by a successful change.
   *
   * @return Configured shortcuts.
   */
  std::vector<shortcut_t> current();

  /**
   * @brief Create or replace one shortcut, persist the list, and apply it without restarting.
   *
   * @param shortcut Shortcut to save; an empty identifier creates a new shortcut.
   * @param error Failure reason when the list would be invalid or cannot be saved.
   * @return Saved shortcut with its identifier, or no value on failure.
   */
  std::optional<shortcut_t> upsert(shortcut_t shortcut, std::string &error);

  /**
   * @brief Delete one shortcut, persist the list, and apply it without restarting.
   *
   * @param id Identifier of the shortcut to delete.
   * @param error Failure reason when the shortcut is unknown or cannot be saved.
   * @return True after the shortcut was removed.
   */
  bool remove(std::string_view id, std::string &error);

}  // namespace steamshine_gamepad_shortcuts
