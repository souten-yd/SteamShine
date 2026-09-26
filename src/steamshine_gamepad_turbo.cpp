/**
 * @file src/steamshine_gamepad_turbo.cpp
 * @brief Controller turbo (rapid fire) toggled per button from the gamepad.
 */
#include "steamshine_gamepad_turbo.h"

#include "config.h"

#include <algorithm>
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
        for (const auto name : button_names()) {
          result |= shortcuts::button_bit(name).value_or(0);
        }
        return result;
      }();
      return mask;
    }
  }  // namespace

  const std::vector<std::string_view> &button_names() {
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
    tracker.hz = settings.hz;
    if (!settings.enabled) {
      for (auto remaining {tracker.active}; remaining;) {
        const std::uint32_t button {remaining & (~remaining + 1)};
        remaining &= remaining - 1;
        changes.push_back({button, false});
      }
      tracker.active = 0;
    } else if (const auto modifier {shortcuts::button_bit(settings.modifier)}; modifier && (raw & *modifier)) {
      auto targets {raw & ~tracker.previous_buttons & ~*modifier & target_mask()};
      while (targets) {
        const std::uint32_t button {targets & (~targets + 1)};
        targets &= targets - 1;
        tracker.active ^= button;
        tracker.suppressed_targets |= button;
        changes.push_back({button, (tracker.active & button) != 0});
      }
    }
    tracker.suppressed_targets &= raw;
    state.buttonFlags &= ~tracker.suppressed_targets;
    for (auto entry {tracker.pressed_since.begin()}; entry != tracker.pressed_since.end();) {
      entry = (tracker.active & entry->first) && (state.buttonFlags & entry->first) ? std::next(entry) : tracker.pressed_since.erase(entry);
    }
    for (auto held {tracker.active & state.buttonFlags}; held;) {
      const std::uint32_t button {held & (~held + 1)};
      held &= held - 1;
      tracker.pressed_since.try_emplace(button, now);
    }
    tracker.previous_buttons = raw;
    tracker.input = state;
    return changes;
  }

  void clear(tracker_t &tracker) {
    tracker.active = 0;
    tracker.pressed_since.clear();
  }

  platf::gamepad_state_t render(const tracker_t &tracker, const clock_t::time_point now) {
    auto state {tracker.input};
    // Pressed for the first half of each 1/hz period.
    const std::chrono::nanoseconds half_period {std::chrono::nanoseconds {std::chrono::seconds {1}} / (2 * std::max(tracker.hz, MIN_HZ))};
    for (const auto &[button, since] : tracker.pressed_since) {
      if ((tracker.active & button) && (state.buttonFlags & button) && ((now - since) / half_period) % 2 != 0) {
        state.buttonFlags &= ~button;
      }
    }
    return state;
  }

  bool ticking(const tracker_t &tracker) {
    return (tracker.active & tracker.input.buttonFlags) != 0;
  }

  bool validate(const settings_t &settings, std::string &error) {
    if (!shortcuts::button_bit(settings.modifier)) {
      error = "Choose one combination button for turbo";
      return false;
    }
    if (settings.hz < MIN_HZ || settings.hz > MAX_HZ) {
      error = std::format("Turbo speed must be between {} and {} presses per second", MIN_HZ, MAX_HZ);
      return false;
    }
    return true;
  }

  std::string to_json(const settings_t &settings) {
    const nlohmann::json result = {{"enabled", settings.enabled}, {"modifier", settings.modifier}, {"hz", settings.hz}};
    return result.dump();
  }

  std::optional<settings_t> from_json(const std::string_view text) {
    settings_t settings;
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return settings;
    }
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("enabled") || !parsed["enabled"].is_boolean() || !parsed.contains("modifier") || !parsed["modifier"].is_string() || !parsed.contains("hz") || !parsed["hz"].is_number_integer()) {
      return std::nullopt;
    }
    settings.enabled = parsed["enabled"].get<bool>();
    settings.modifier = parsed["modifier"].get<std::string>();
    settings.hz = parsed["hz"].get<int>();
    std::string ignored;
    if (!validate(settings, ignored)) {
      return std::nullopt;
    }
    settings.modifier = std::string {shortcuts::button_name(*shortcuts::button_bit(settings.modifier))};
    return settings;
  }

  settings_t current() {
    std::lock_guard lock {g_mutex};
    if (!g_current) {
      g_current = from_json(config::sunshine.steamshine_gamepad_turbo).value_or(settings_t {});
    }
    return *g_current;
  }

  bool set(settings_t settings, std::string &error) {
    if (!validate(settings, error)) {
      return false;
    }
    settings.modifier = std::string {shortcuts::button_name(*shortcuts::button_bit(settings.modifier))};
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
