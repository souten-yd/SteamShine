/**
 * @file tests/unit/test_steamshine_gamepad_shortcuts.cpp
 * @brief Tests for configurable Home and Quick Access controller shortcuts.
 */

#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <src/config.h>
#include <src/steamshine_gamepad_shortcuts.h>
#include <sstream>
#include <string>

using namespace std::literals;
namespace shortcuts = steamshine_gamepad_shortcuts;

namespace {

  constexpr auto HOME {static_cast<std::size_t>(shortcuts::action_e::home)};  ///< Home action index.
  constexpr auto QUICK_ACCESS {static_cast<std::size_t>(shortcuts::action_e::quick_access)};  ///< Quick Access action index.

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

  /**
   * @brief Build shortcuts from input lists with a one-second hold.
   *
   * @param home Home inputs.
   * @param quick_access Quick Access inputs.
   * @return Parsed shortcuts.
   */
  shortcuts::shortcuts_t make(const std::string_view home, const std::string_view quick_access = {}) {
    return {*shortcuts::parse(home, 1000), *shortcuts::parse(quick_access, 1000)};
  }

  /**
   * @brief Build a timer-action array with one action set.
   *
   * @param home Home timer change.
   * @param quick_access Quick Access timer change.
   * @return Timer changes indexed by action.
   */
  shortcuts::timer_actions_t actions(const shortcuts::timer_action_e home, const shortcuts::timer_action_e quick_access = shortcuts::timer_action_e::none) {
    return {home, quick_access};
  }

  constexpr auto NONE {shortcuts::timer_action_e::none};  ///< No timer change.
  constexpr auto ARM {shortcuts::timer_action_e::arm};  ///< Start a timer.
  constexpr auto CANCEL {shortcuts::timer_action_e::cancel};  ///< Cancel a timer.

}  // namespace

/**
 * @brief Accept case-insensitive, whitespace-tolerant input lists in canonical order.
 */
TEST(SteamshineGamepadShortcutsTest, ParsesAndFormatsCombinations) {
  const auto parsed {shortcuts::parse(" back + Start ", 3000)};
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->buttons, platf::START | platf::BACK);
  EXPECT_EQ(parsed->hold, 3000ms);
  EXPECT_EQ(shortcuts::format_inputs(*parsed), "START+BACK");

  const auto triggers {shortcuts::parse("RT+lt+Y", 200)};
  ASSERT_TRUE(triggers);
  EXPECT_TRUE(triggers->left_trigger);
  EXPECT_TRUE(triggers->right_trigger);
  EXPECT_EQ(shortcuts::format_inputs(*triggers), "Y+LT+RT");
  EXPECT_TRUE(triggers->same_inputs(*shortcuts::parse("LT+Y+RT", 5000)));
  EXPECT_FALSE(triggers->same_inputs(*shortcuts::parse("LT+Y", 200)));

  const auto disabled {shortcuts::parse("", 1000)};
  ASSERT_TRUE(disabled);
  EXPECT_FALSE(disabled->enabled());
  EXPECT_EQ(shortcuts::format_inputs(*disabled), "");
  EXPECT_EQ(shortcuts::input_names().front(), "START");
  EXPECT_EQ(shortcuts::input_names().size(), 16U);
}

/**
 * @brief Map action names both ways and reject unknown names.
 */
TEST(SteamshineGamepadShortcutsTest, NamesActions) {
  EXPECT_EQ(shortcuts::action_name(shortcuts::action_e::home), "home");
  EXPECT_EQ(shortcuts::action_name(shortcuts::action_e::quick_access), "quick_access");
  EXPECT_EQ(shortcuts::parse_action("quick_access"), shortcuts::action_e::quick_access);
  EXPECT_EQ(shortcuts::parse_action("home"), shortcuts::action_e::home);
  EXPECT_FALSE(shortcuts::parse_action("power"));
}

/**
 * @brief Reject unknown, duplicate, excessive, empty, and out-of-range values.
 */
TEST(SteamshineGamepadShortcutsTest, RejectsInvalidCombinations) {
  EXPECT_FALSE(shortcuts::parse("START+HOME", 1000));
  EXPECT_FALSE(shortcuts::parse("START+start", 1000));
  EXPECT_FALSE(shortcuts::parse("LT+LT", 1000));
  EXPECT_FALSE(shortcuts::parse("RT+RT", 1000));
  EXPECT_FALSE(shortcuts::parse("START+", 1000));
  EXPECT_FALSE(shortcuts::parse("A+B+X+Y+START", 1000));
  EXPECT_TRUE(shortcuts::parse("A+B+X+Y", 1000));
  EXPECT_FALSE(shortcuts::parse("START", 199));
  EXPECT_FALSE(shortcuts::parse("START", 10001));
  EXPECT_TRUE(shortcuts::parse("START", 10000));
}

/**
 * @brief Require every button and trigger to be held at the threshold.
 */
TEST(SteamshineGamepadShortcutsTest, DetectsHeldCombination) {
  const auto start_back {*shortcuts::parse("START+BACK", 1000)};
  EXPECT_TRUE(shortcuts::held(start_back, pad(platf::START | platf::BACK | platf::A)));
  EXPECT_FALSE(shortcuts::held(start_back, pad(platf::START)));
  const auto triggers {*shortcuts::parse("LT+RT", 1000)};
  EXPECT_TRUE(shortcuts::held(triggers, pad(0, shortcuts::TRIGGER_THRESHOLD, 255)));
  EXPECT_FALSE(shortcuts::held(triggers, pad(0, shortcuts::TRIGGER_THRESHOLD - 1, 255)));
  EXPECT_FALSE(shortcuts::held(shortcuts::combo_t {}, pad(platf::START | platf::BACK)));
}

/**
 * @brief Arm on hold, cancel on early release, and suppress fired inputs until released.
 */
TEST(SteamshineGamepadShortcutsTest, ArmsCancelsAndSuppressesUntilRelease) {
  const auto configured {make("START+BACK")};
  shortcuts::tracker_t tracker;

  auto state {pad(platf::START)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(state.buttonFlags, platf::START);

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(ARM));
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  state = pad(platf::START);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(CANCEL));

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(ARM));
  auto host {state};
  shortcuts::fire(tracker, configured, shortcuts::action_e::home, host);
  EXPECT_EQ(host.buttonFlags, 0U);
  EXPECT_FALSE(tracker.armed[HOME]);

  // Still holding: hidden from the host and never re-armed.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(state.buttonFlags, platf::A);

  // Releasing one input keeps the other hidden until it is released too.
  state = pad(platf::START);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(state.buttonFlags, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(state.buttonFlags, platf::BACK);

  state = pad(0);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(tracker.suppressed_buttons, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(ARM));
  EXPECT_EQ(state.buttonFlags, platf::START | platf::BACK);
}

/**
 * @brief Prefer a held superset and disarm overlapping shortcuts when one fires.
 */
TEST(SteamshineGamepadShortcutsTest, PrefersSupersetAndDisarmsOverlaps) {
  const auto configured {make("START+BACK", "START+BACK+A")};
  shortcuts::tracker_t tracker;

  auto state {pad(platf::START | platf::BACK)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(ARM));
  // Adding A switches from Home to Quick Access.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(CANCEL, ARM));
  shortcuts::fire(tracker, configured, shortcuts::action_e::quick_access, state);
  EXPECT_EQ(state.buttonFlags, 0U);

  // Releasing only A leaves Start + Back suppressed, so Home cannot fire.
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(NONE));
  EXPECT_EQ(state.buttonFlags, 0U);

  // Two disjoint shortcuts armed together: firing one leaves the other armed,
  // while an overlapping armed shortcut is disarmed immediately.
  const auto disjoint {make("LB", "RB")};
  shortcuts::tracker_t both;
  state = pad(platf::LEFT_BUTTON | platf::RIGHT_BUTTON);
  EXPECT_EQ(shortcuts::filter(both, disjoint, state), actions(ARM, ARM));
  shortcuts::fire(both, disjoint, shortcuts::action_e::home, state);
  EXPECT_TRUE(both.armed[QUICK_ACCESS]);

  const auto overlapping {make("LB+X", "LB+Y")};
  shortcuts::tracker_t overlap;
  state = pad(platf::LEFT_BUTTON | platf::X | platf::Y);
  EXPECT_EQ(shortcuts::filter(overlap, overlapping, state), actions(ARM, ARM));
  shortcuts::fire(overlap, overlapping, shortcuts::action_e::home, state);
  EXPECT_FALSE(overlap.armed[QUICK_ACCESS]);
  EXPECT_EQ(state.buttonFlags, platf::Y);
}

/**
 * @brief Suppress fired triggers until each drops below the threshold.
 */
TEST(SteamshineGamepadShortcutsTest, SuppressesTriggersUntilReleased) {
  const auto configured {make("LT+RT")};
  shortcuts::tracker_t tracker;
  auto state {pad(0, 255, 255)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), actions(ARM));
  shortcuts::fire(tracker, configured, shortcuts::action_e::home, state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 0);

  state = pad(0, 255, 10);
  shortcuts::filter(tracker, configured, state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 10);
  EXPECT_FALSE(tracker.suppress_right_trigger);
  state = pad(0, 0, 200);
  shortcuts::filter(tracker, configured, state);
  EXPECT_FALSE(tracker.suppress_left_trigger);
  EXPECT_EQ(state.rt, 200);
}

/**
 * @brief Disabled shortcuts leave input untouched and cancel stale timers.
 */
TEST(SteamshineGamepadShortcutsTest, DisabledShortcutsPassInputThrough) {
  shortcuts::tracker_t tracker;
  tracker.armed = {true, true};
  auto state {pad(platf::START | platf::BACK, 255, 255)};
  EXPECT_EQ(shortcuts::filter(tracker, shortcuts::shortcuts_t {}, state), actions(CANCEL, CANCEL));
  EXPECT_EQ(state.buttonFlags, platf::START | platf::BACK);
  EXPECT_EQ(state.lt, 255);
}

/**
 * @brief Persist each action separately, reject identical inputs, and publish without restart.
 */
TEST(SteamshineGamepadShortcutsTest, PersistsAndPublishesWithoutRestart) {
  const auto directory {std::filesystem::temp_directory_path() / std::format("steamshine-shortcuts-{}", std::chrono::steady_clock::now().time_since_epoch().count())};
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  const auto file {directory / "sunshine.conf"};
  {
    std::ofstream output {file};
    output << "locale = ja\nsteamshine_home_combo = BACK\n";
  }
  const auto previous_file {config::sunshine.config_file};
  config::sunshine.config_file = file.string();

  std::string error;
  ASSERT_TRUE(shortcuts::set(shortcuts::action_e::home, *shortcuts::parse("START+BACK", 3000), error)) << error;
  ASSERT_TRUE(shortcuts::set(shortcuts::action_e::quick_access, *shortcuts::parse("START+BACK+A", 1500), error)) << error;
  std::stringstream saved;
  saved << std::ifstream {file}.rdbuf();
  EXPECT_NE(saved.str().find("locale = ja"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_home_combo = START+BACK\n"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_home_combo_hold_ms = 3000"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_quick_access_combo = START+BACK+A"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_quick_access_combo_hold_ms = 1500"), std::string::npos);
  EXPECT_EQ(shortcuts::format_inputs(shortcuts::current()[HOME]), "START+BACK");
  EXPECT_EQ(shortcuts::current()[QUICK_ACCESS].hold, 1500ms);
  EXPECT_EQ(config::sunshine.steamshine_quick_access_combo, "START+BACK+A");

  EXPECT_FALSE(shortcuts::set(shortcuts::action_e::quick_access, *shortcuts::parse("BACK+START", 500), error));
  EXPECT_NE(error.find("different"), std::string::npos);
  EXPECT_EQ(shortcuts::format_inputs(shortcuts::current()[QUICK_ACCESS]), "START+BACK+A");

  ASSERT_TRUE(shortcuts::set(shortcuts::action_e::home, shortcuts::combo_t {}, error)) << error;
  std::stringstream disabled;
  disabled << std::ifstream {file}.rdbuf();
  EXPECT_EQ(disabled.str().find("steamshine_home_combo = "), std::string::npos);
  EXPECT_FALSE(shortcuts::current()[HOME].enabled());
  EXPECT_TRUE(shortcuts::current()[QUICK_ACCESS].enabled());

  // A missing directory leaves the published values unchanged.
  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  EXPECT_FALSE(shortcuts::set(shortcuts::action_e::home, *shortcuts::parse("A", 1000), error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(shortcuts::current()[HOME].enabled());

  ASSERT_TRUE(std::filesystem::create_directories(directory / "missing"));
  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  ASSERT_TRUE(shortcuts::set(shortcuts::action_e::quick_access, shortcuts::combo_t {}, error)) << error;
  config::sunshine.config_file = previous_file;
  std::filesystem::remove_all(directory);
}
