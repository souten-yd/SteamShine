/**
 * @file src/steamshine_gamepad_turbo.cpp
 * @brief Controller turbo (rapid fire) toggled from the gamepad with configurable presets.
 */
#include "steamshine_gamepad_turbo.h"

#include "config.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>

namespace steamshine_gamepad_turbo {

  namespace shortcuts = steamshine_gamepad_shortcuts;

  namespace {
    /**
     * @brief Configuration key holding the persisted settings.
     */
    constexpr std::string_view CONFIG_KEY {"steamshine_gamepad_turbo"};

    std::mutex g_mutex;  ///< Protects the published settings.
    std::optional<settings_t> g_current;  ///< Published settings, loaded on first use.

    /**
     * @brief Return the mask of every button that may be put into turbo.
     *
     * @return Button mask.
     */
    std::uint32_t target_mask() {
      static const std::uint32_t mask = [] {
        std::uint32_t result {0};
        for (const auto name : target_names()) {
          result |= shortcuts::button_bit(name).value_or(0);
        }
        return result;
      }();
      return mask;
    }

    /**
     * @brief Count the inputs a modifier requires.
     *
     * @param combo Modifier combination.
     * @return Number of buttons and triggers.
     */
    int input_count(const shortcuts::combo_t &combo) {
      return std::popcount(combo.buttons) + (combo.left_trigger ? 1 : 0) + (combo.right_trigger ? 1 : 0);
    }
  }  // namespace

  const std::vector<std::string_view> &target_names() {
    static const std::vector<std::string_view> names = [] {
      std::vector<std::string_view> result;
      for (const auto name : shortcuts::input_names()) {
        if (shortcuts::button_bit(name)) {
          result.push_back(name);
        }
      }
      return result;
    }();
    return names;
  }

  std::vector<toggle_t> update(tracker_t &tracker, const settings_t &settings, platf::gamepad_state_t &state, const clock_t::time_point now) {
    const auto raw {state.buttonFlags};
    std::vector<toggle_t> changes;
    if (!settings.enabled) {
      for (const auto &[button, hz] : tracker.active) {
        (void) hz;
        changes.push_back({button, 0});
      }
      tracker.active.clear();
    } else {
      // The held preset with the most inputs wins, so Back + RB can coexist with Back.
      const preset_t *chosen {nullptr};
      for (const auto &preset : settings.presets) {
        if (shortcuts::held(preset.modifier, state) && (!chosen || input_count(preset.modifier) > input_count(chosen->modifier))) {
          chosen = &preset;
        }
      }
      if (chosen) {
        auto targets {raw & ~tracker.previous_buttons & ~chosen->modifier.buttons & target_mask()};
        while (targets) {
          const std::uint32_t button {targets & (~targets + 1)};
          targets &= targets - 1;
          if (const auto existing {tracker.active.find(button)}; existing != tracker.active.end() && existing->second == chosen->hz) {
            tracker.active.erase(existing);
            changes.push_back({button, 0});
          } else {
            tracker.active[button] = chosen->hz;
            changes.push_back({button, chosen->hz});
          }
          tracker.suppressed_targets |= button;
        }
      }
    }
    tracker.suppressed_targets &= raw;
    state.buttonFlags &= ~tracker.suppressed_targets;
    for (auto entry {tracker.pressed_since.begin()}; entry != tracker.pressed_since.end();) {
      entry = tracker.active.contains(entry->first) && (state.buttonFlags & entry->first) ? std::next(entry) : tracker.pressed_since.erase(entry);
    }
    for (const auto &[button, hz] : tracker.active) {
      (void) hz;
      if ((state.buttonFlags & button) && !tracker.pressed_since.contains(button)) {
        tracker.pressed_since[button] = now;
      }
    }
    tracker.previous_buttons = raw;
    tracker.input = state;
    return changes;
  }

  platf::gamepad_state_t render(const tracker_t &tracker, const clock_t::time_point now) {
    auto state {tracker.input};
    for (const auto &[button, hz] : tracker.active) {
      const auto since {tracker.pressed_since.find(button)};
      if (!(state.buttonFlags & button) || since == tracker.pressed_since.end()) {
        continue;
      }
      // Pressed for the first half of each 1/hz period.
      const std::chrono::nanoseconds half_period {std::chrono::nanoseconds {std::chrono::seconds {1}} / (2 * hz)};
      const auto phase {(now - since->second) / half_period};
      if (phase % 2 != 0) {
        state.buttonFlags &= ~button;
      }
    }
    return state;
  }

  bool ticking(const tracker_t &tracker) {
    return std::ranges::any_of(tracker.active, [&tracker](const auto &entry) {
      return (tracker.input.buttonFlags & entry.first) != 0;
    });
  }

  bool validate(const settings_t &settings, std::string &error) {
    for (std::size_t index {0}; index < PRESET_COUNT; ++index) {
      const auto &preset {settings.presets[index]};
      if (preset.hz < MIN_HZ || preset.hz > MAX_HZ) {
        error = std::format("Turbo frequencies must be between {} and {} presses per second", MIN_HZ, MAX_HZ);
        return false;
      }
      for (std::size_t other {0}; other < index; ++other) {
        if (preset.modifier.enabled() && preset.modifier.same_inputs(settings.presets[other].modifier)) {
          error = "Two turbo presets cannot use the same buttons";
          return false;
        }
      }
    }
    return true;
  }

  std::string to_json(const settings_t &settings) {
    nlohmann::json presets = nlohmann::json::array();
    for (const auto &preset : settings.presets) {
      presets.push_back({{"inputs", shortcuts::format_inputs(preset.modifier)}, {"hz", preset.hz}});
    }
    nlohmann::json result = {{"enabled", settings.enabled}, {"presets", presets}};
    return result.dump();
  }

  std::optional<settings_t> from_json(const std::string_view text) {
    settings_t settings;
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return settings;
    }
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("enabled") || !parsed["enabled"].is_boolean() || !parsed.contains("presets") || !parsed["presets"].is_array() || parsed["presets"].size() != PRESET_COUNT) {
      return std::nullopt;
    }
    settings.enabled = parsed["enabled"].get<bool>();
    for (std::size_t index {0}; index < PRESET_COUNT; ++index) {
      const nlohmann::json &entry = parsed["presets"][index];
      if (!entry.is_object() || !entry.contains("inputs") || !entry["inputs"].is_string() || !entry.contains("hz") || !entry["hz"].is_number_integer()) {
        return std::nullopt;
      }
      const auto modifier {shortcuts::parse(entry["inputs"].get<std::string>(), 1000)};
      if (!modifier) {
        return std::nullopt;
      }
      settings.presets[index] = {*modifier, entry["hz"].get<int>()};
    }
    std::string ignored;
    if (!validate(settings, ignored)) {
      return std::nullopt;
    }
    return settings;
  }

  settings_t current() {
    std::lock_guard lock {g_mutex};
    if (!g_current) {
      g_current = from_json(config::sunshine.steamshine_gamepad_turbo).value_or(settings_t {});
    }
    return *g_current;
  }

  bool set(const settings_t &settings, std::string &error) {
    if (!validate(settings, error)) {
      return false;
    }
    std::lock_guard lock {g_mutex};
    const auto serialized {to_json(settings)};
    if (!shortcuts::write_config_value(CONFIG_KEY, serialized, {}, error)) {
      return false;
    }
    config::sunshine.steamshine_gamepad_turbo = serialized;
    g_current = settings;
    return true;
  }

}  // namespace steamshine_gamepad_turbo
