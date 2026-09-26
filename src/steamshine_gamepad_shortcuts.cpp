/**
 * @file src/steamshine_gamepad_shortcuts.cpp
 * @brief Configurable button combinations that emulate Steam's Home and Quick Access buttons.
 */
#include "steamshine_gamepad_shortcuts.h"

#include "config.h"
#include "file_handler.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace steamshine_gamepad_shortcuts {

  namespace {
    /**
     * @brief One named input that may appear in a combination.
     */
    struct input_t {
      std::string_view name;  ///< Stable configuration name.
      std::uint32_t button;  ///< Button mask bit, or zero for a trigger.
      int trigger;  ///< 1 for the left trigger, 2 for the right trigger, otherwise 0.
    };

    /**
     * @brief Inputs in canonical display order.
     */
    constexpr std::array INPUTS {
      input_t {"START", platf::START, 0},
      input_t {"BACK", platf::BACK, 0},
      input_t {"A", platf::A, 0},
      input_t {"B", platf::B, 0},
      input_t {"X", platf::X, 0},
      input_t {"Y", platf::Y, 0},
      input_t {"LB", platf::LEFT_BUTTON, 0},
      input_t {"RB", platf::RIGHT_BUTTON, 0},
      input_t {"LT", 0, 1},
      input_t {"RT", 0, 2},
      input_t {"LS", platf::LEFT_STICK, 0},
      input_t {"RS", platf::RIGHT_STICK, 0},
      input_t {"UP", platf::DPAD_UP, 0},
      input_t {"DOWN", platf::DPAD_DOWN, 0},
      input_t {"LEFT", platf::DPAD_LEFT, 0},
      input_t {"RIGHT", platf::DPAD_RIGHT, 0},
    };

    /**
     * @brief Configuration keys and storage for one action.
     */
    struct action_config_t {
      std::string_view name;  ///< Stable action name.
      std::string_view inputs_key;  ///< Configuration key holding the input list.
      std::string_view hold_key;  ///< Configuration key holding the hold time.
      std::string config::sunshine_t::*inputs;  ///< Parsed input-list member.
      int config::sunshine_t::*hold_ms;  ///< Parsed hold-time member.
    };

    /**
     * @brief Per-action configuration, indexed by action_e.
     */
    const std::array<action_config_t, ACTION_COUNT> ACTIONS {
      action_config_t {"home", "steamshine_home_combo", "steamshine_home_combo_hold_ms", &config::sunshine_t::steamshine_home_combo, &config::sunshine_t::steamshine_home_combo_hold_ms},
      action_config_t {"quick_access", "steamshine_quick_access_combo", "steamshine_quick_access_combo_hold_ms", &config::sunshine_t::steamshine_quick_access_combo, &config::sunshine_t::steamshine_quick_access_combo_hold_ms},
    };

    std::mutex g_mutex;  ///< Protects the published combinations.
    std::optional<shortcuts_t> g_current;  ///< Published combinations, loaded on first use.

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
     * @brief Replace the configuration file with one action's keys updated.
     *
     * @param action Action whose keys change.
     * @param combo Combination to persist.
     * @param error Failure reason, leaving the previous file intact.
     * @return True after the file was atomically replaced.
     */
    bool persist(const action_e action, const combo_t &combo, std::string &error) {
      const auto &keys {ACTIONS[static_cast<std::size_t>(action)]};
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      const auto inputs {format_inputs(combo)};
      if (inputs.empty()) {
        vars.erase(std::string {keys.inputs_key});
      } else {
        vars[std::string {keys.inputs_key}] = inputs;
      }
      vars[std::string {keys.hold_key}] = std::to_string(combo.hold.count());
      std::stringstream config_stream;
      for (const auto &[key, value] : vars) {
        config_stream << key << " = " << value << std::endl;
      }
      const fs::path destination {config::sunshine.config_file};
      const auto temporary = destination.string() + ".gamepad-shortcuts.tmp";
      {
        std::ofstream output {temporary, std::ios::trunc};
        output << config_stream.str();
        output.close();
        if (!output) {
          std::error_code ignored;
          fs::remove(temporary, ignored);
          error = "Unable to save the controller shortcut; existing settings were retained";
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
        error = "Unable to replace the controller shortcut; existing settings were retained";
        return false;
      }
      config::sunshine.*keys.inputs = inputs;
      config::sunshine.*keys.hold_ms = static_cast<int>(combo.hold.count());
      return true;
    }

    /**
     * @brief Load the persisted combinations on first use; the caller holds g_mutex.
     *
     * @return Published combinations.
     */
    shortcuts_t &current_locked() {
      if (!g_current) {
        shortcuts_t loaded {};
        for (std::size_t index {0}; index < ACTION_COUNT; ++index) {
          const auto &keys {ACTIONS[index]};
          const auto configured_hold {config::sunshine.*keys.hold_ms};
          const auto hold {configured_hold > 0 ? configured_hold : static_cast<int>(DEFAULT_HOLD.count())};
          loaded[index] = parse(config::sunshine.*keys.inputs, hold).value_or(combo_t {});
        }
        // Identical input sets would fire both actions at once; keep only Home.
        if (loaded[0].enabled() && loaded[0].same_inputs(loaded[1])) {
          loaded[1] = combo_t {};
        }
        g_current = loaded;
      }
      return *g_current;
    }
  }  // namespace

  bool combo_t::enabled() const {
    return buttons != 0 || left_trigger || right_trigger;
  }

  bool combo_t::same_inputs(const combo_t &other) const {
    return buttons == other.buttons && left_trigger == other.left_trigger && right_trigger == other.right_trigger;
  }

  std::string_view action_name(const action_e action) {
    return ACTIONS[static_cast<std::size_t>(action)].name;
  }

  std::optional<action_e> parse_action(const std::string_view name) {
    for (std::size_t index {0}; index < ACTION_COUNT; ++index) {
      if (ACTIONS[index].name == name) {
        return static_cast<action_e>(index);
      }
    }
    return std::nullopt;
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
    std::size_t count {0};
    std::size_t start {0};
    while (start <= text.size()) {
      const auto end {std::min(text.find('+', start), text.size())};
      const auto token {upper(trim(text.substr(start, end - start)))};
      const auto match {std::ranges::find(INPUTS, token, &input_t::name)};
      if (match == INPUTS.end() || ++count > MAX_INPUTS) {
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
      start = end + 1;
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

  bool held(const combo_t &combo, const platf::gamepad_state_t &state) {
    return combo.enabled() && (state.buttonFlags & combo.buttons) == combo.buttons && (!combo.left_trigger || state.lt >= TRIGGER_THRESHOLD) && (!combo.right_trigger || state.rt >= TRIGGER_THRESHOLD);
  }

  timer_actions_t filter(tracker_t &tracker, const shortcuts_t &shortcuts, platf::gamepad_state_t &state) {
    std::array<bool, ACTION_COUNT> raw_held {};
    for (std::size_t index {0}; index < ACTION_COUNT; ++index) {
      raw_held[index] = held(shortcuts[index], state);
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

    timer_actions_t result {};
    result.fill(timer_action_e::none);
    for (std::size_t index {0}; index < ACTION_COUNT; ++index) {
      const auto &combo {shortcuts[index]};
      // A fired input re-arms nothing until it is released, and a held
      // superset such as Start + Back + A takes precedence over Start + Back.
      bool shadowed {false};
      for (std::size_t other {0}; other < ACTION_COUNT; ++other) {
        shadowed = shadowed || (other != index && raw_held[other] && strict_superset(shortcuts[other], combo));
      }
      const bool arm {raw_held[index] && !uses_suppressed(combo, tracker) && !shadowed};
      if (arm && !tracker.armed[index]) {
        tracker.armed[index] = true;
        result[index] = timer_action_e::arm;
      } else if (!arm && tracker.armed[index]) {
        tracker.armed[index] = false;
        result[index] = timer_action_e::cancel;
      }
    }
    return result;
  }

  void fire(tracker_t &tracker, const shortcuts_t &shortcuts, const action_e action, platf::gamepad_state_t &state) {
    const auto &combo {shortcuts[static_cast<std::size_t>(action)]};
    tracker.armed[static_cast<std::size_t>(action)] = false;
    tracker.suppressed_buttons |= combo.buttons;
    tracker.suppress_left_trigger = tracker.suppress_left_trigger || combo.left_trigger;
    tracker.suppress_right_trigger = tracker.suppress_right_trigger || combo.right_trigger;
    state.buttonFlags &= ~combo.buttons;
    if (combo.left_trigger) {
      state.lt = 0;
    }
    if (combo.right_trigger) {
      state.rt = 0;
    }
    for (std::size_t index {0}; index < ACTION_COUNT; ++index) {
      if (tracker.armed[index] && uses_suppressed(shortcuts[index], tracker)) {
        tracker.armed[index] = false;
      }
    }
  }

  shortcuts_t current() {
    std::lock_guard lock {g_mutex};
    return current_locked();
  }

  bool set(const action_e action, const combo_t &combo, std::string &error) {
    std::lock_guard lock {g_mutex};
    const auto shortcuts {current_locked()};
    const auto index {static_cast<std::size_t>(action)};
    for (std::size_t other {0}; other < ACTION_COUNT; ++other) {
      if (other != index && combo.enabled() && combo.same_inputs(shortcuts[other])) {
        error = "Home and Quick Access need different button combinations";
        return false;
      }
    }
    if (!persist(action, combo, error)) {
      return false;
    }
    (*g_current)[index] = combo;
    return true;
  }

}  // namespace steamshine_gamepad_shortcuts
