/**
 * @file src/steamshine_home_combo.cpp
 * @brief Configurable button combinations that emulate the gamepad Home button.
 */
#include "steamshine_home_combo.h"

#include "config.h"
#include "file_handler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace steamshine_home_combo {

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

    std::mutex g_mutex;  ///< Protects the published combination.
    std::optional<combo_t> g_current;  ///< Published combination, loaded on first use.

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
     * @brief Replace the configuration file with both combination keys updated.
     *
     * @param combo Combination to persist.
     * @param error Failure reason, leaving the previous file intact.
     * @return True after the file was atomically replaced.
     */
    bool persist(const combo_t &combo, std::string &error) {
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      const auto inputs {format_inputs(combo)};
      if (inputs.empty()) {
        vars.erase("steamshine_home_combo");
      } else {
        vars["steamshine_home_combo"] = inputs;
      }
      vars["steamshine_home_combo_hold_ms"] = std::to_string(combo.hold.count());
      std::stringstream config_stream;
      for (const auto &[key, value] : vars) {
        config_stream << key << " = " << value << std::endl;
      }
      const fs::path destination {config::sunshine.config_file};
      const auto temporary = destination.string() + ".home-combo.tmp";
      {
        std::ofstream output {temporary, std::ios::trunc};
        output << config_stream.str();
        output.close();
        if (!output) {
          std::error_code ignored;
          fs::remove(temporary, ignored);
          error = "Unable to save the Home button setting; existing settings were retained";
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
        error = "Unable to replace the Home button setting; existing settings were retained";
        return false;
      }
      config::sunshine.steamshine_home_combo = inputs;
      config::sunshine.steamshine_home_combo_hold_ms = static_cast<int>(combo.hold.count());
      return true;
    }
  }  // namespace

  bool combo_t::enabled() const {
    return buttons != 0 || left_trigger || right_trigger;
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

  timer_action_e filter(tracker_t &tracker, const combo_t &combo, platf::gamepad_state_t &state) {
    const bool raw_held {held(combo, state)};
    tracker.suppressed_buttons &= state.buttonFlags;
    tracker.suppress_left_trigger = tracker.suppress_left_trigger && state.lt >= TRIGGER_THRESHOLD;
    tracker.suppress_right_trigger = tracker.suppress_right_trigger && state.rt >= TRIGGER_THRESHOLD;
    const bool suppressing {tracker.suppressed_buttons != 0 || tracker.suppress_left_trigger || tracker.suppress_right_trigger};
    state.buttonFlags &= ~tracker.suppressed_buttons;
    if (tracker.suppress_left_trigger) {
      state.lt = 0;
    }
    if (tracker.suppress_right_trigger) {
      state.rt = 0;
    }
    // A fired combination re-arms only after every one of its inputs was released.
    const bool arm {raw_held && !suppressing};
    if (arm && !tracker.timer_armed) {
      tracker.timer_armed = true;
      return timer_action_e::arm;
    }
    if (!arm && tracker.timer_armed) {
      tracker.timer_armed = false;
      return timer_action_e::cancel;
    }
    return timer_action_e::none;
  }

  void fire(tracker_t &tracker, const combo_t &combo, platf::gamepad_state_t &state) {
    tracker.timer_armed = false;
    tracker.suppressed_buttons = combo.buttons;
    tracker.suppress_left_trigger = combo.left_trigger;
    tracker.suppress_right_trigger = combo.right_trigger;
    state.buttonFlags &= ~combo.buttons;
    if (combo.left_trigger) {
      state.lt = 0;
    }
    if (combo.right_trigger) {
      state.rt = 0;
    }
  }

  combo_t current() {
    std::lock_guard lock {g_mutex};
    if (!g_current) {
      const auto hold {config::sunshine.steamshine_home_combo_hold_ms > 0 ? config::sunshine.steamshine_home_combo_hold_ms : static_cast<int>(DEFAULT_HOLD.count())};
      g_current = parse(config::sunshine.steamshine_home_combo, hold).value_or(combo_t {});
    }
    return *g_current;
  }

  bool set(const combo_t &combo, std::string &error) {
    std::lock_guard lock {g_mutex};
    if (!persist(combo, error)) {
      return false;
    }
    g_current = combo;
    return true;
  }

}  // namespace steamshine_home_combo
