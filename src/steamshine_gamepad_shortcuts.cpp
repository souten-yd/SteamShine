/**
 * @file src/steamshine_gamepad_shortcuts.cpp
 * @brief User-defined controller shortcuts that send other gamepad keys when a combination is held.
 */
#include "steamshine_gamepad_shortcuts.h"

#include "config.h"
#include "file_handler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace steamshine_gamepad_shortcuts {

  namespace {
    /**
     * @brief One named gamepad key.
     */
    struct key_t {
      std::string_view name;  ///< Stable configuration name.
      std::uint32_t button;  ///< Button mask bit, or zero for a trigger.
      int trigger;  ///< 1 for the left trigger, 2 for the right trigger, otherwise 0.
    };

    /**
     * @brief Keys that may be held, in canonical display order.
     */
    constexpr std::array INPUTS {
      key_t {"START", platf::START, 0},
      key_t {"BACK", platf::BACK, 0},
      key_t {"A", platf::A, 0},
      key_t {"B", platf::B, 0},
      key_t {"X", platf::X, 0},
      key_t {"Y", platf::Y, 0},
      key_t {"LB", platf::LEFT_BUTTON, 0},
      key_t {"RB", platf::RIGHT_BUTTON, 0},
      key_t {"LT", 0, 1},
      key_t {"RT", 0, 2},
      key_t {"LS", platf::LEFT_STICK, 0},
      key_t {"RS", platf::RIGHT_STICK, 0},
      key_t {"UP", platf::DPAD_UP, 0},
      key_t {"DOWN", platf::DPAD_DOWN, 0},
      key_t {"LEFT", platf::DPAD_LEFT, 0},
      key_t {"RIGHT", platf::DPAD_RIGHT, 0},
    };

    /**
     * @brief The Home key, which may be sent but not held.
     */
    constexpr key_t HOME_KEY {"HOME", platf::HOME, 0};

    /**
     * @brief Configuration key holding the persisted shortcut list.
     */
    constexpr std::string_view CONFIG_KEY {"steamshine_gamepad_shortcuts"};

    /**
     * @brief Unreleased configuration keys replaced by CONFIG_KEY and removed on save.
     */
    constexpr std::array<std::string_view, 4> LEGACY_KEYS {"steamshine_home_combo", "steamshine_home_combo_hold_ms", "steamshine_quick_access_combo", "steamshine_quick_access_combo_hold_ms"};

    std::mutex g_mutex;  ///< Protects the published shortcuts.
    std::optional<std::vector<shortcut_t>> g_current;  ///< Published shortcuts, loaded on first use.

    /**
     * @brief Uppercase one ASCII token.
     *
     * @param value Token to convert.
     * @return Uppercase copy.
     */
    std::string upper(std::string_view value) {
      std::string result {value};
      std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
      });
      return result;
    }

    /**
     * @brief Trim ASCII whitespace from both ends.
     *
     * @param value Text to trim.
     * @return Trimmed view into @p value.
     */
    std::string_view trim(std::string_view value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      return value;
    }

    /**
     * @brief Split a `+`-separated list into trimmed, uppercase tokens.
     *
     * @param text List to split; must not be empty.
     * @return Tokens, including empty ones for doubled or trailing separators.
     */
    std::vector<std::string> tokens(const std::string_view text) {
      std::vector<std::string> result;
      std::size_t start {0};
      while (start <= text.size()) {
        const auto end {std::min(text.find('+', start), text.size())};
        result.push_back(upper(trim(text.substr(start, end - start))));
        start = end + 1;
      }
      return result;
    }

    /**
     * @brief Find a sendable key by name.
     *
     * @param name Uppercase key name.
     * @return Key, or no value when unknown.
     */
    std::optional<key_t> find_output_key(const std::string_view name) {
      if (name == HOME_KEY.name) {
        return HOME_KEY;
      }
      const auto match {std::ranges::find(INPUTS, name, &key_t::name)};
      return match == INPUTS.end() ? std::nullopt : std::optional {*match};
    }

    /**
     * @brief Test whether a combination uses any input that is currently suppressed.
     *
     * @param combo Combination to test.
     * @param tracker Per-gamepad suppression state.
     * @return True when at least one input is suppressed.
     */
    bool uses_suppressed(const combo_t &combo, const tracker_t &tracker) {
      return (combo.buttons & tracker.suppressed_buttons) != 0 || (combo.left_trigger && tracker.suppress_left_trigger) || (combo.right_trigger && tracker.suppress_right_trigger);
    }

    /**
     * @brief Test whether one combination requires every input of another plus at least one more.
     *
     * @param outer Candidate superset.
     * @param inner Candidate subset.
     * @return True when @p outer strictly contains @p inner.
     */
    bool strict_superset(const combo_t &outer, const combo_t &inner) {
      const bool contains {(outer.buttons & inner.buttons) == inner.buttons && (outer.left_trigger || !inner.left_trigger) && (outer.right_trigger || !inner.right_trigger)};
      return inner.enabled() && contains && !outer.same_inputs(inner);
    }

    /**
     * @brief Accept identifiers made of 1 to 32 lowercase letters and digits.
     *
     * @param id Identifier to test.
     * @return True when valid.
     */
    bool valid_id(const std::string_view id) {
      return !id.empty() && id.size() <= 32 && std::ranges::all_of(id, [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
      });
    }

    /**
     * @brief Create an identifier that no current shortcut uses.
     *
     * @param shortcuts Existing shortcuts.
     * @return New identifier.
     */
    std::string new_id(const std::vector<shortcut_t> &shortcuts) {
      static std::mt19937 generator {std::random_device {}()};
      while (true) {
        const auto candidate {std::format("s{:08x}", generator())};
        if (std::ranges::none_of(shortcuts, [&](const auto &shortcut) {
              return shortcut.id == candidate;
            })) {
          return candidate;
        }
      }
    }

    /**
     * @brief Load the persisted shortcuts on first use; the caller holds g_mutex.
     *
     * @return Published shortcuts.
     */
    std::vector<shortcut_t> &current_locked() {
      if (!g_current) {
        g_current = from_json(config::sunshine.steamshine_gamepad_shortcuts).value_or(std::vector<shortcut_t> {});
      }
      return *g_current;
    }

    /**
     * @brief Replace the configuration file with a new shortcut list and publish it.
     *
     * @param shortcuts Validated list to persist.
     * @param error Failure reason, leaving the previous file and list intact.
     * @return True after the file was atomically replaced.
     */
    bool persist_locked(const std::vector<shortcut_t> &shortcuts, std::string &error) {
      const auto serialized {to_json(shortcuts)};
      std::vector<std::string_view> obsolete {LEGACY_KEYS.begin(), LEGACY_KEYS.end()};
      if (!write_config_value(CONFIG_KEY, serialized, obsolete, error)) {
        return false;
      }
      config::sunshine.steamshine_gamepad_shortcuts = serialized;
      g_current = shortcuts;
      return true;
    }
  }  // namespace

  bool combo_t::enabled() const {
    return buttons != 0 || left_trigger || right_trigger;
  }

  bool combo_t::same_inputs(const combo_t &other) const {
    return buttons == other.buttons && left_trigger == other.left_trigger && right_trigger == other.right_trigger;
  }

  std::optional<std::uint32_t> button_bit(const std::string_view name) {
    const auto match {std::ranges::find(INPUTS, upper(trim(name)), &key_t::name)};
    if (match == INPUTS.end() || match->trigger != 0) {
      return std::nullopt;
    }
    return match->button;
  }

  std::string_view button_name(const std::uint32_t bit) {
    const auto match {std::ranges::find(INPUTS, bit, &key_t::button)};
    return match == INPUTS.end() || bit == 0 ? std::string_view {} : match->name;
  }

  bool write_config_value(const std::string_view key, const std::string &value, const std::vector<std::string_view> &obsolete, std::string &error) {
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    vars[std::string {key}] = value;
    for (const auto name : obsolete) {
      vars.erase(std::string {name});
    }
    std::stringstream config_stream;
    for (const auto &[name, entry] : vars) {
      config_stream << name << " = " << entry << std::endl;
    }
    const fs::path destination {config::sunshine.config_file};
    const auto temporary = std::format("{}.{}.tmp", destination.string(), key);
    {
      std::ofstream output {temporary, std::ios::trunc};
      output << config_stream.str();
      output.close();
      if (!output) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        error = "Unable to save controller settings; existing settings were retained";
        return false;
      }
    }
    std::error_code write_error;
    const auto permissions = fs::exists(destination, write_error) ? fs::status(destination, write_error).permissions() : fs::perms::owner_read | fs::perms::owner_write;
    if (!write_error) {
      fs::permissions(temporary, permissions, write_error);
    }
    if (!write_error) {
      fs::rename(temporary, destination, write_error);
    }
    if (write_error) {
      std::error_code ignored;
      fs::remove(temporary, ignored);
      error = "Unable to replace controller settings; existing settings were retained";
      return false;
    }
    return true;
  }

  const std::vector<std::string_view> &input_names() {
    static const std::vector<std::string_view> names = [] {
      std::vector<std::string_view> result;
      for (const auto &input : INPUTS) {
        result.push_back(input.name);
      }
      return result;
    }();
    return names;
  }

  const std::vector<std::string_view> &output_names() {
    static const std::vector<std::string_view> names = [] {
      std::vector<std::string_view> result {HOME_KEY.name};
      result.insert(result.end(), input_names().begin(), input_names().end());
      return result;
    }();
    return names;
  }

  std::optional<combo_t> parse(const std::string_view inputs, const int hold_ms) {
    if (hold_ms < MIN_HOLD.count() || hold_ms > MAX_HOLD.count()) {
      return std::nullopt;
    }
    combo_t combo;
    combo.hold = std::chrono::milliseconds {hold_ms};
    const auto text {trim(inputs)};
    if (text.empty()) {
      return combo;
    }
    const auto names {tokens(text)};
    if (names.size() > MAX_INPUTS) {
      return std::nullopt;
    }
    for (const auto &name : names) {
      const auto match {std::ranges::find(INPUTS, name, &key_t::name)};
      if (match == INPUTS.end()) {
        return std::nullopt;
      }
      if (match->trigger == 1) {
        if (combo.left_trigger) {
          return std::nullopt;
        }
        combo.left_trigger = true;
      } else if (match->trigger == 2) {
        if (combo.right_trigger) {
          return std::nullopt;
        }
        combo.right_trigger = true;
      } else {
        if (combo.buttons & match->button) {
          return std::nullopt;
        }
        combo.buttons |= match->button;
      }
    }
    return combo;
  }

  std::string format_inputs(const combo_t &combo) {
    std::string result;
    for (const auto &input : INPUTS) {
      const bool selected {input.trigger == 1 ? combo.left_trigger : input.trigger == 2 ? combo.right_trigger :
                                                                                          (combo.buttons & input.button) != 0};
      if (selected) {
        if (!result.empty()) {
          result += '+';
        }
        result += input.name;
      }
    }
    return result;
  }

  std::optional<output_t> parse_output(const std::string_view keys) {
    const auto text {trim(keys)};
    if (text.empty()) {
      return std::nullopt;
    }
    output_t output;
    for (auto &name : tokens(text)) {
      if (!find_output_key(name) || std::ranges::find(output.keys, name) != output.keys.end()) {
        return std::nullopt;
      }
      output.keys.push_back(std::move(name));
    }
    if (output.keys.size() > MAX_INPUTS) {
      return std::nullopt;
    }
    return output;
  }

  std::string format_output(const output_t &output) {
    std::string result;
    for (const auto &key : output.keys) {
      if (!result.empty()) {
        result += '+';
      }
      result += key;
    }
    return result;
  }

  std::vector<platf::gamepad_state_t> output_frames(const output_t &output, const platf::gamepad_state_t &base) {
    std::vector<platf::gamepad_state_t> frames;
    auto state {base};
    const auto apply = [&state, &base](const key_t &key, const bool pressed) {
      if (key.trigger == 1) {
        state.lt = pressed ? 255 : base.lt;
      } else if (key.trigger == 2) {
        state.rt = pressed ? 255 : base.rt;
      } else if (pressed) {
        state.buttonFlags |= key.button;
      } else {
        state.buttonFlags = (state.buttonFlags & ~key.button) | (base.buttonFlags & key.button);
      }
    };
    for (const auto &name : output.keys) {
      apply(*find_output_key(name), true);
      frames.push_back(state);
    }
    for (auto name {output.keys.rbegin()}; name != output.keys.rend(); ++name) {
      apply(*find_output_key(*name), false);
      frames.push_back(state);
    }
    return frames;
  }

  bool held(const combo_t &combo, const platf::gamepad_state_t &state) {
    return combo.enabled() && (state.buttonFlags & combo.buttons) == combo.buttons && (!combo.left_trigger || state.lt >= TRIGGER_THRESHOLD) && (!combo.right_trigger || state.rt >= TRIGGER_THRESHOLD);
  }

  std::vector<timer_change_t> filter(tracker_t &tracker, const std::vector<shortcut_t> &shortcuts, platf::gamepad_state_t &state) {
    std::vector<bool> raw_held;
    raw_held.reserve(shortcuts.size());
    for (const auto &shortcut : shortcuts) {
      raw_held.push_back(shortcut.enabled && held(shortcut.trigger, state));
    }
    tracker.suppressed_buttons &= state.buttonFlags;
    tracker.suppress_left_trigger = tracker.suppress_left_trigger && state.lt >= TRIGGER_THRESHOLD;
    tracker.suppress_right_trigger = tracker.suppress_right_trigger && state.rt >= TRIGGER_THRESHOLD;
    state.buttonFlags &= ~tracker.suppressed_buttons;
    if (tracker.suppress_left_trigger) {
      state.lt = 0;
    }
    if (tracker.suppress_right_trigger) {
      state.rt = 0;
    }

    std::vector<timer_change_t> changes;
    std::set<std::string, std::less<>> still_armed;
    for (std::size_t index {0}; index < shortcuts.size(); ++index) {
      const auto &shortcut {shortcuts[index]};
      // A fired input re-arms nothing until it is released, and a held
      // superset such as Start + Back + A takes precedence over Start + Back.
      bool shadowed {false};
      for (std::size_t other {0}; other < shortcuts.size(); ++other) {
        shadowed = shadowed || (other != index && raw_held[other] && strict_superset(shortcuts[other].trigger, shortcut.trigger));
      }
      const bool arm {raw_held[index] && !uses_suppressed(shortcut.trigger, tracker) && !shadowed};
      const bool was_armed {tracker.armed.contains(shortcut.id)};
      if (arm) {
        still_armed.insert(shortcut.id);
        if (!was_armed) {
          changes.push_back({shortcut.id, true});
        }
      } else if (was_armed) {
        changes.push_back({shortcut.id, false});
      }
    }
    // Shortcuts removed since their timer started are cancelled too.
    for (const auto &id : tracker.armed) {
      if (std::ranges::none_of(shortcuts, [&](const auto &shortcut) {
            return shortcut.id == id;
          })) {
        changes.push_back({id, false});
      }
    }
    tracker.armed = std::move(still_armed);
    return changes;
  }

  void fire(tracker_t &tracker, const std::vector<shortcut_t> &shortcuts, const shortcut_t &fired, platf::gamepad_state_t &state) {
    tracker.armed.erase(fired.id);
    tracker.suppressed_buttons |= fired.trigger.buttons;
    tracker.suppress_left_trigger = tracker.suppress_left_trigger || fired.trigger.left_trigger;
    tracker.suppress_right_trigger = tracker.suppress_right_trigger || fired.trigger.right_trigger;
    state.buttonFlags &= ~fired.trigger.buttons;
    if (fired.trigger.left_trigger) {
      state.lt = 0;
    }
    if (fired.trigger.right_trigger) {
      state.rt = 0;
    }
    for (const auto &shortcut : shortcuts) {
      if (tracker.armed.contains(shortcut.id) && uses_suppressed(shortcut.trigger, tracker)) {
        tracker.armed.erase(shortcut.id);
      }
    }
  }

  bool validate(const std::vector<shortcut_t> &shortcuts, std::string &error) {
    if (shortcuts.size() > MAX_SHORTCUTS) {
      error = std::format("Save up to {} controller shortcuts", MAX_SHORTCUTS);
      return false;
    }
    for (std::size_t index {0}; index < shortcuts.size(); ++index) {
      const auto &shortcut {shortcuts[index]};
      if (!valid_id(shortcut.id)) {
        error = "Controller shortcut identifier is invalid";
        return false;
      }
      if (shortcut.name.size() > MAX_NAME_LENGTH || std::ranges::any_of(shortcut.name, [](const unsigned char character) {
            return std::iscntrl(character);
          })) {
        error = std::format("Shortcut names can be up to {} characters", MAX_NAME_LENGTH);
        return false;
      }
      if (!shortcut.trigger.enabled() || shortcut.trigger.hold < MIN_HOLD || shortcut.trigger.hold > MAX_HOLD) {
        error = "Choose buttons to hold and a hold time between 0.2 and 10 seconds";
        return false;
      }
      if (!parse_output(format_output(shortcut.output))) {
        error = "Choose up to four different keys to send";
        return false;
      }
      for (std::size_t other {0}; other < index; ++other) {
        if (shortcuts[other].id == shortcut.id) {
          error = "Controller shortcut identifiers must be unique";
          return false;
        }
        if (shortcut.enabled && shortcuts[other].enabled && shortcut.trigger.same_inputs(shortcuts[other].trigger)) {
          error = "Two enabled shortcuts cannot use the same buttons";
          return false;
        }
      }
    }
    return true;
  }

  std::string to_json(const std::vector<shortcut_t> &shortcuts) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto &shortcut : shortcuts) {
      array.push_back({
        {"id", shortcut.id},
        {"name", shortcut.name},
        {"inputs", format_inputs(shortcut.trigger)},
        {"hold_ms", shortcut.trigger.hold.count()},
        {"output", format_output(shortcut.output)},
        {"enabled", shortcut.enabled},
      });
    }
    return array.dump();
  }

  std::optional<std::vector<shortcut_t>> from_json(const std::string_view text) {
    if (trim(text).empty()) {
      return std::vector<shortcut_t> {};
    }
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) {
      return std::nullopt;
    }
    std::vector<shortcut_t> result;
    for (const auto &entry : parsed) {
      if (!entry.is_object() || !entry.value("id", nlohmann::json {}).is_string() || !entry.value("inputs", nlohmann::json {}).is_string() || !entry.value("hold_ms", nlohmann::json {}).is_number_integer() || !entry.value("output", nlohmann::json {}).is_string()) {
        return std::nullopt;
      }
      const auto trigger {parse(entry["inputs"].get<std::string>(), entry["hold_ms"].get<int>())};
      const auto output {parse_output(entry["output"].get<std::string>())};
      if (!trigger || !output) {
        return std::nullopt;
      }
      const nlohmann::json name = entry.value("name", nlohmann::json(""));
      const nlohmann::json enabled = entry.value("enabled", nlohmann::json(true));
      if (!name.is_string() || !enabled.is_boolean()) {
        return std::nullopt;
      }
      result.push_back({entry["id"].get<std::string>(), name.get<std::string>(), *trigger, *output, enabled.get<bool>()});
    }
    std::string ignored;
    if (!validate(result, ignored)) {
      return std::nullopt;
    }
    return result;
  }

  std::vector<shortcut_t> current() {
    std::lock_guard lock {g_mutex};
    return current_locked();
  }

  std::optional<shortcut_t> upsert(shortcut_t shortcut, std::string &error) {
    std::lock_guard lock {g_mutex};
    auto shortcuts {current_locked()};
    if (shortcut.id.empty()) {
      shortcut.id = new_id(shortcuts);
      shortcuts.push_back(shortcut);
    } else {
      const auto existing {std::ranges::find(shortcuts, shortcut.id, &shortcut_t::id)};
      if (existing == shortcuts.end()) {
        error = "This controller shortcut no longer exists. Refresh the page";
        return std::nullopt;
      }
      *existing = shortcut;
    }
    if (!validate(shortcuts, error) || !persist_locked(shortcuts, error)) {
      return std::nullopt;
    }
    return shortcut;
  }

  bool remove(const std::string_view id, std::string &error) {
    std::lock_guard lock {g_mutex};
    auto shortcuts {current_locked()};
    const auto existing {std::ranges::find(shortcuts, id, &shortcut_t::id)};
    if (existing == shortcuts.end()) {
      error = "This controller shortcut no longer exists. Refresh the page";
      return false;
    }
    shortcuts.erase(existing);
    return persist_locked(shortcuts, error);
  }

}  // namespace steamshine_gamepad_shortcuts
