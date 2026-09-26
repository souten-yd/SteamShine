/**
 * @file src/steamshine_gamepad_turbo.h
 * @brief Controller turbo (rapid fire) toggled from the gamepad with configurable presets.
 *
 * Each preset pairs a modifier combination with a frequency. Holding a preset's
 * modifier and pressing another button turns turbo on for that button at the
 * preset's frequency; repeating the gesture turns it off, and using another
 * preset changes the frequency. While the player holds a turbo button, the host
 * sees it pressed and released at that frequency. Turbo state belongs to one
 * connected controller and resets when it reconnects.
 */
#pragma once

#include "platform/common.h"
#include "steamshine_gamepad_shortcuts.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steamshine_gamepad_turbo {

  /**
   * @brief Number of configurable presets.
   */
  constexpr std::size_t PRESET_COUNT {4};

  /**
   * @brief Lowest accepted frequency in presses per second.
   */
  constexpr int MIN_HZ {1};

  /**
   * @brief Highest accepted frequency in presses per second.
   */
  constexpr int MAX_HZ {30};

  /**
   * @brief Interval at which held turbo buttons are re-rendered.
   */
  constexpr std::chrono::milliseconds TICK {5};

  /**
   * @brief Monotonic clock used for turbo phases.
   */
  using clock_t = std::chrono::steady_clock;

  /**
   * @brief One modifier combination and the frequency it assigns.
   */
  struct preset_t {
    steamshine_gamepad_shortcuts::combo_t modifier;  ///< Inputs held while pressing the target; empty leaves the preset unused.
    int hz {10};  ///< Presses per second.
  };

  /**
   * @brief Persisted turbo configuration.
   */
  struct settings_t {
    bool enabled {false};  ///< Whether turbo gestures and output are active.
    std::array<preset_t, PRESET_COUNT> presets {preset_t {{}, 5}, preset_t {{}, 10}, preset_t {{}, 15}, preset_t {{}, 20}};  ///< Presets in display order.
  };

  /**
   * @brief One turbo change made by a gesture.
   */
  struct toggle_t {
    std::uint32_t button {0};  ///< Target button bit.
    int hz {0};  ///< New frequency, or zero when turbo was turned off.

    /**
     * @brief Compare two toggles.
     *
     * @param other Toggle to compare.
     * @return True when both change the same button to the same frequency.
     */
    bool operator==(const toggle_t &other) const = default;
  };

  /**
   * @brief Per-gamepad turbo state.
   */
  struct tracker_t {
    std::map<std::uint32_t, int> active;  ///< Turbo frequency by button bit.
    std::map<std::uint32_t, clock_t::time_point> pressed_since;  ///< Press time of each held turbo button.
    std::uint32_t suppressed_targets {0};  ///< Gesture targets hidden from the host until released.
    std::uint32_t previous_buttons {0};  ///< Buttons held in the previous packet, for press detection.
    platf::gamepad_state_t input {};  ///< Latest filtered client state that turbo renders from.
  };

  /**
   * @brief Return the button names that can be put into turbo, in display order.
   *
   * @return Held-input names except triggers.
   */
  const std::vector<std::string_view> &target_names();

  /**
   * @brief Process one gamepad packet: apply gestures and remember the input to render.
   *
   * @param tracker Per-gamepad turbo state.
   * @param settings Active settings; when disabled, every turbo button is turned off.
   * @param state Client state after shortcut filtering; gesture targets are removed on return.
   * @param now Packet time.
   * @return Turbo changes made by this packet.
   */
  std::vector<toggle_t> update(tracker_t &tracker, const settings_t &settings, platf::gamepad_state_t &state, clock_t::time_point now);

  /**
   * @brief Build the host-visible state, pulsing held turbo buttons.
   *
   * A turbo button is pressed for the first half of each period, starting when
   * the player pressed it.
   *
   * @param tracker Per-gamepad turbo state.
   * @param now Render time.
   * @return Host-visible state.
   */
  platf::gamepad_state_t render(const tracker_t &tracker, clock_t::time_point now);

  /**
   * @brief Report whether a held turbo button needs periodic re-rendering.
   *
   * @param tracker Per-gamepad turbo state.
   * @return True while at least one turbo button is held.
   */
  bool ticking(const tracker_t &tracker);

  /**
   * @brief Validate a turbo configuration.
   *
   * @param settings Candidate settings.
   * @param error Reason the settings are invalid.
   * @return True when every frequency is in range and no two used presets hold the same inputs.
   */
  bool validate(const settings_t &settings, std::string &error);

  /**
   * @brief Serialize settings to the persisted JSON object.
   *
   * @param settings Settings to serialize.
   * @return Compact JSON text.
   */
  std::string to_json(const settings_t &settings);

  /**
   * @brief Parse and validate the persisted JSON object.
   *
   * @param text JSON text; empty yields the defaults.
   * @return Settings, or no value when malformed or invalid.
   */
  std::optional<settings_t> from_json(std::string_view text);

  /**
   * @brief Return the active settings, loading them from the configuration on first use.
   *
   * @return Active settings.
   */
  settings_t current();

  /**
   * @brief Validate, persist, and apply new settings without restarting.
   *
   * @param settings Settings to save.
   * @param error Failure reason.
   * @return True after the configuration was replaced and the settings published.
   */
  bool set(const settings_t &settings, std::string &error);

}  // namespace steamshine_gamepad_turbo
