/**
 * @file tests/unit/test_steamshine_home_combo.cpp
 * @brief Tests for configurable Home-button combinations.
 */

#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <src/config.h>
#include <src/steamshine_home_combo.h>
#include <sstream>
#include <string>

using namespace std::literals;
namespace combo = steamshine_home_combo;

namespace {

  /**
   * @brief Build a gamepad state with the given buttons and triggers.
   *
   * @param buttons Button mask.
   * @param lt Left trigger value.
   * @param rt Right trigger value.
   * @return Gamepad state with centered sticks.
   */
  platf::gamepad_state_t pad(const std::uint32_t buttons, const std::uint8_t lt = 0, const std::uint8_t rt = 0) {
    return platf::gamepad_state_t {buttons, lt, rt, 0, 0, 0, 0};
  }

}  // namespace

/**
 * @brief Accept case-insensitive, whitespace-tolerant input lists in canonical order.
 */
TEST(SteamshineHomeComboTest, ParsesAndFormatsCombinations) {
  const auto parsed {combo::parse(" back + Start ", 3000)};
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->buttons, platf::START | platf::BACK);
  EXPECT_EQ(parsed->hold, 3000ms);
  EXPECT_EQ(combo::format_inputs(*parsed), "START+BACK");

  const auto triggers {combo::parse("RT+lt+Y", 200)};
  ASSERT_TRUE(triggers);
  EXPECT_TRUE(triggers->left_trigger);
  EXPECT_TRUE(triggers->right_trigger);
  EXPECT_EQ(combo::format_inputs(*triggers), "Y+LT+RT");

  const auto disabled {combo::parse("", 1000)};
  ASSERT_TRUE(disabled);
  EXPECT_FALSE(disabled->enabled());
  EXPECT_EQ(combo::format_inputs(*disabled), "");
  EXPECT_EQ(combo::input_names().front(), "START");
  EXPECT_EQ(combo::input_names().size(), 16U);
}

/**
 * @brief Reject unknown, duplicate, excessive, empty, and out-of-range values.
 */
TEST(SteamshineHomeComboTest, RejectsInvalidCombinations) {
  EXPECT_FALSE(combo::parse("START+HOME", 1000));
  EXPECT_FALSE(combo::parse("START+start", 1000));
  EXPECT_FALSE(combo::parse("LT+LT", 1000));
  EXPECT_FALSE(combo::parse("RT+RT", 1000));
  EXPECT_FALSE(combo::parse("START+", 1000));
  EXPECT_FALSE(combo::parse("A+B+X+Y+START", 1000));
  EXPECT_TRUE(combo::parse("A+B+X+Y", 1000));
  EXPECT_FALSE(combo::parse("START", 199));
  EXPECT_FALSE(combo::parse("START", 10001));
  EXPECT_TRUE(combo::parse("START", 10000));
}

/**
 * @brief Require every button and trigger to be held at the threshold.
 */
TEST(SteamshineHomeComboTest, DetectsHeldCombination) {
  const auto start_back {*combo::parse("START+BACK", 1000)};
  EXPECT_TRUE(combo::held(start_back, pad(platf::START | platf::BACK | platf::A)));
  EXPECT_FALSE(combo::held(start_back, pad(platf::START)));
  const auto triggers {*combo::parse("LT+RT", 1000)};
  EXPECT_TRUE(combo::held(triggers, pad(0, combo::TRIGGER_THRESHOLD, 255)));
  EXPECT_FALSE(combo::held(triggers, pad(0, combo::TRIGGER_THRESHOLD - 1, 255)));
  EXPECT_FALSE(combo::held(combo::combo_t {}, pad(platf::START | platf::BACK)));
}

/**
 * @brief Arm on hold, cancel on early release, and suppress fired inputs until released.
 */
TEST(SteamshineHomeComboTest, ArmsCancelsAndSuppressesUntilRelease) {
  const auto start_back {*combo::parse("START+BACK", 1000)};
  combo::tracker_t tracker;

  auto state {pad(platf::START)};
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  EXPECT_EQ(state.buttonFlags, platf::START);

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::arm);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  state = pad(platf::START);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::cancel);

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::arm);
  auto host {state};
  combo::fire(tracker, start_back, host);
  EXPECT_EQ(host.buttonFlags, 0U);
  EXPECT_FALSE(tracker.timer_armed);

  // Still holding: hidden from the host and never re-armed.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  EXPECT_EQ(state.buttonFlags, platf::A);

  // Releasing one input keeps the other hidden until it is released too.
  state = pad(platf::START);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  EXPECT_EQ(state.buttonFlags, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  EXPECT_EQ(state.buttonFlags, platf::BACK);

  state = pad(0);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::none);
  EXPECT_EQ(tracker.suppressed_buttons, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(combo::filter(tracker, start_back, state), combo::timer_action_e::arm);
  EXPECT_EQ(state.buttonFlags, platf::START | platf::BACK);
}

/**
 * @brief Suppress fired triggers until each drops below the threshold.
 */
TEST(SteamshineHomeComboTest, SuppressesTriggersUntilReleased) {
  const auto triggers {*combo::parse("LT+RT", 500)};
  combo::tracker_t tracker;
  auto state {pad(0, 255, 255)};
  EXPECT_EQ(combo::filter(tracker, triggers, state), combo::timer_action_e::arm);
  combo::fire(tracker, triggers, state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 0);

  state = pad(0, 255, 10);
  combo::filter(tracker, triggers, state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 10);
  EXPECT_FALSE(tracker.suppress_right_trigger);
  state = pad(0, 0, 200);
  combo::filter(tracker, triggers, state);
  EXPECT_FALSE(tracker.suppress_left_trigger);
  EXPECT_EQ(state.rt, 200);
}

/**
 * @brief A disabled combination leaves input untouched and cancels a stale timer.
 */
TEST(SteamshineHomeComboTest, DisabledCombinationPassesInputThrough) {
  combo::tracker_t tracker;
  tracker.timer_armed = true;
  auto state {pad(platf::START | platf::BACK, 255, 255)};
  EXPECT_EQ(combo::filter(tracker, combo::combo_t {}, state), combo::timer_action_e::cancel);
  EXPECT_EQ(state.buttonFlags, platf::START | platf::BACK);
  EXPECT_EQ(state.lt, 255);
}

/**
 * @brief Persist a combination, keep other settings, and publish it immediately.
 */
TEST(SteamshineHomeComboTest, PersistsAndPublishesWithoutRestart) {
  const auto directory {std::filesystem::temp_directory_path() / std::format("steamshine-home-combo-{}", std::chrono::steady_clock::now().time_since_epoch().count())};
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  const auto file {directory / "sunshine.conf"};
  {
    std::ofstream output {file};
    output << "locale = ja\nsteamshine_home_combo = BACK\n";
  }
  const auto previous_file {config::sunshine.config_file};
  config::sunshine.config_file = file.string();

  std::string error;
  ASSERT_TRUE(combo::set(*combo::parse("START+BACK", 3000), error)) << error;
  std::stringstream saved;
  saved << std::ifstream {file}.rdbuf();
  EXPECT_NE(saved.str().find("locale = ja"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_home_combo = START+BACK"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_home_combo_hold_ms = 3000"), std::string::npos);
  EXPECT_EQ(combo::format_inputs(combo::current()), "START+BACK");
  EXPECT_EQ(combo::current().hold, 3000ms);
  EXPECT_EQ(config::sunshine.steamshine_home_combo, "START+BACK");

  ASSERT_TRUE(combo::set(combo::combo_t {}, error)) << error;
  std::stringstream disabled;
  disabled << std::ifstream {file}.rdbuf();
  EXPECT_EQ(disabled.str().find("steamshine_home_combo = "), std::string::npos);
  EXPECT_FALSE(combo::current().enabled());

  // A missing directory leaves the published value unchanged.
  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  EXPECT_FALSE(combo::set(*combo::parse("A", 1000), error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(combo::current().enabled());

  config::sunshine.config_file = previous_file;
  std::filesystem::remove_all(directory);
}
