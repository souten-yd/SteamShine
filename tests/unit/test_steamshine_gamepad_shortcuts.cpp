/**
 * @file tests/unit/test_steamshine_gamepad_shortcuts.cpp
 * @brief Tests for user-defined controller shortcuts.
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
   * @brief Build one enabled shortcut with a one-second hold.
   *
   * @param id Identifier.
   * @param inputs Held inputs.
   * @param output Keys to send.
   * @param enabled Whether the shortcut is active.
   * @return Shortcut.
   */
  shortcuts::shortcut_t make(const std::string &id, const std::string_view inputs, const std::string_view output = "HOME", const bool enabled = true) {
    return {id, "", *shortcuts::parse(inputs, 1000), *shortcuts::parse_output(output), enabled};
  }

  /**
   * @brief Build a timer change.
   *
   * @param id Shortcut identifier.
   * @param arm True to arm, false to cancel.
   * @return Timer change.
   */
  shortcuts::timer_change_t change(const std::string &id, const bool arm) {
    return {id, arm};
  }

  using changes_t = std::vector<shortcuts::timer_change_t>;  ///< Expected timer changes.

}  // namespace

/**
 * @brief Accept case-insensitive, whitespace-tolerant held inputs in canonical order.
 */
TEST(SteamshineGamepadShortcutsTest, ParsesAndFormatsHeldInputs) {
  const auto parsed {shortcuts::parse(" back + Start ", 3000)};
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->buttons, platf::START | platf::BACK);
  EXPECT_EQ(parsed->hold, 3000ms);
  EXPECT_EQ(shortcuts::format_inputs(*parsed), "START+BACK");

  const auto triggers {shortcuts::parse("RT+lt+Y", 200)};
  ASSERT_TRUE(triggers);
  EXPECT_EQ(shortcuts::format_inputs(*triggers), "Y+LT+RT");
  EXPECT_TRUE(triggers->same_inputs(*shortcuts::parse("LT+Y+RT", 5000)));
  EXPECT_FALSE(triggers->same_inputs(*shortcuts::parse("LT+Y", 200)));

  EXPECT_FALSE(shortcuts::parse("", 1000)->enabled());
  EXPECT_EQ(shortcuts::input_names().size(), 16U);
  EXPECT_EQ(shortcuts::output_names().front(), "HOME");
  EXPECT_EQ(shortcuts::output_names().size(), 17U);

  EXPECT_FALSE(shortcuts::parse("START+HOME", 1000));
  EXPECT_FALSE(shortcuts::parse("START+start", 1000));
  EXPECT_FALSE(shortcuts::parse("LT+LT", 1000));
  EXPECT_FALSE(shortcuts::parse("RT+RT", 1000));
  EXPECT_FALSE(shortcuts::parse("START+", 1000));
  EXPECT_FALSE(shortcuts::parse("A+B+X+Y+START", 1000));
  EXPECT_FALSE(shortcuts::parse("START", 199));
  EXPECT_FALSE(shortcuts::parse("START", 10001));
  EXPECT_TRUE(shortcuts::parse("A+B+X+Y", 10000));
}

/**
 * @brief Keep output order, allow Home, and reject empty, unknown, duplicate, or long lists.
 */
TEST(SteamshineGamepadShortcutsTest, ParsesOrderedOutputs) {
  const auto quick_access {shortcuts::parse_output("home + a")};
  ASSERT_TRUE(quick_access);
  EXPECT_EQ(shortcuts::format_output(*quick_access), "HOME+A");
  EXPECT_EQ(shortcuts::format_output(*shortcuts::parse_output("A+HOME")), "A+HOME");
  EXPECT_FALSE(shortcuts::parse_output(""));
  EXPECT_FALSE(shortcuts::parse_output("HOME+HOME"));
  EXPECT_FALSE(shortcuts::parse_output("HOME+GUIDE"));
  EXPECT_FALSE(shortcuts::parse_output("HOME+A+B+X+Y"));
  EXPECT_TRUE(shortcuts::parse_output("HOME+A+LT+UP"));
}

/**
 * @brief Press output keys in order, release them in reverse, and restore the base state.
 */
TEST(SteamshineGamepadShortcutsTest, BuildsOutputFrames) {
  const auto base {pad(platf::X, 40, 0)};
  const auto frames {shortcuts::output_frames(*shortcuts::parse_output("HOME+A+LT"), base)};
  ASSERT_EQ(frames.size(), 6U);
  EXPECT_EQ(frames[0].buttonFlags, platf::X | platf::HOME);
  EXPECT_EQ(frames[1].buttonFlags, platf::X | platf::HOME | platf::A);
  EXPECT_EQ(frames[2].lt, 255);
  EXPECT_EQ(frames[3].lt, 40);
  EXPECT_EQ(frames[4].buttonFlags, platf::X | platf::HOME);
  EXPECT_EQ(frames[5].buttonFlags, base.buttonFlags);
  EXPECT_EQ(frames[5].lt, base.lt);

  // Releasing an output key that the player is also holding keeps it held.
  const auto held_x {shortcuts::output_frames(*shortcuts::parse_output("X"), pad(platf::X))};
  EXPECT_EQ(held_x.back().buttonFlags, platf::X);
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
  const std::vector configured {make("home", "START+BACK")};
  shortcuts::tracker_t tracker;

  auto state {pad(platf::START)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {});
  EXPECT_EQ(state.buttonFlags, platf::START);

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("home", true)});
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {});
  state = pad(platf::START);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("home", false)});

  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("home", true)});
  auto host {state};
  shortcuts::fire(tracker, configured, configured[0], host);
  EXPECT_EQ(host.buttonFlags, 0U);
  EXPECT_FALSE(tracker.armed.contains("home"));

  // Still holding: hidden from the host and never re-armed.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {});
  EXPECT_EQ(state.buttonFlags, platf::A);

  // Releasing one input keeps the other hidden until it is released too.
  state = pad(platf::START);
  shortcuts::filter(tracker, configured, state);
  EXPECT_EQ(state.buttonFlags, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {});
  EXPECT_EQ(state.buttonFlags, platf::BACK);

  state = pad(0);
  shortcuts::filter(tracker, configured, state);
  EXPECT_EQ(tracker.suppressed_buttons, 0U);
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("home", true)});
  EXPECT_EQ(state.buttonFlags, platf::START | platf::BACK);
}

/**
 * @brief Prefer a held superset, disarm overlaps, and ignore disabled or removed shortcuts.
 */
TEST(SteamshineGamepadShortcutsTest, PrefersSupersetAndDisarmsOverlaps) {
  const std::vector configured {make("home", "START+BACK"), make("qam", "START+BACK+A", "HOME+A")};
  shortcuts::tracker_t tracker;

  auto state {pad(platf::START | platf::BACK)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("home", true)});
  // Adding A switches from Home to Quick Access.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), (changes_t {change("home", false), change("qam", true)}));
  shortcuts::fire(tracker, configured, configured[1], state);
  EXPECT_EQ(state.buttonFlags, 0U);
  // Releasing only A leaves Start + Back suppressed, so Home cannot fire.
  state = pad(platf::START | platf::BACK);
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {});

  // Disjoint shortcuts stay armed when another fires; overlapping ones are disarmed.
  const std::vector disjoint {make("left", "LB"), make("right", "RB")};
  shortcuts::tracker_t both;
  state = pad(platf::LEFT_BUTTON | platf::RIGHT_BUTTON);
  EXPECT_EQ(shortcuts::filter(both, disjoint, state), (changes_t {change("left", true), change("right", true)}));
  shortcuts::fire(both, disjoint, disjoint[0], state);
  EXPECT_TRUE(both.armed.contains("right"));

  const std::vector overlapping {make("x", "LB+X"), make("y", "LB+Y")};
  shortcuts::tracker_t overlap;
  state = pad(platf::LEFT_BUTTON | platf::X | platf::Y);
  shortcuts::filter(overlap, overlapping, state);
  shortcuts::fire(overlap, overlapping, overlapping[0], state);
  EXPECT_FALSE(overlap.armed.contains("y"));
  EXPECT_EQ(state.buttonFlags, platf::Y);

  // A disabled superset does not shadow its subset.
  const std::vector disabled {make("home", "START+BACK"), make("qam", "START+BACK+A", "HOME+A", false)};
  shortcuts::tracker_t quiet;
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(quiet, disabled, state), changes_t {change("home", true)});

  // A shortcut removed while armed is cancelled on the next packet.
  state = pad(platf::START | platf::BACK | platf::A);
  EXPECT_EQ(shortcuts::filter(quiet, {}, state), changes_t {change("home", false)});
  EXPECT_TRUE(quiet.armed.empty());
}

/**
 * @brief Suppress fired triggers until each drops below the threshold.
 */
TEST(SteamshineGamepadShortcutsTest, SuppressesTriggersUntilReleased) {
  const std::vector configured {make("triggers", "LT+RT")};
  shortcuts::tracker_t tracker;
  auto state {pad(0, 255, 255)};
  EXPECT_EQ(shortcuts::filter(tracker, configured, state), changes_t {change("triggers", true)});
  shortcuts::fire(tracker, configured, configured[0], state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 0);

  state = pad(0, 255, 10);
  shortcuts::filter(tracker, configured, state);
  EXPECT_EQ(state.lt, 0);
  EXPECT_EQ(state.rt, 10);
  state = pad(0, 0, 200);
  shortcuts::filter(tracker, configured, state);
  EXPECT_FALSE(tracker.suppress_left_trigger);
  EXPECT_EQ(state.rt, 200);
}

/**
 * @brief Reject incomplete, conflicting, oversized, and malformed shortcut lists.
 */
TEST(SteamshineGamepadShortcutsTest, ValidatesShortcutLists) {
  std::string error;
  EXPECT_TRUE(shortcuts::validate({make("a", "START"), make("b", "START", "HOME+A", false)}, error));
  EXPECT_FALSE(shortcuts::validate({make("a", "START"), make("b", "START", "HOME+A")}, error));
  EXPECT_NE(error.find("same buttons"), std::string::npos);
  EXPECT_FALSE(shortcuts::validate({make("a", "START"), make("a", "BACK")}, error));
  EXPECT_FALSE(shortcuts::validate({make("Bad", "START")}, error));
  auto unnamed {make("a", "START")};
  unnamed.name = std::string(shortcuts::MAX_NAME_LENGTH + 1, 'n');
  EXPECT_FALSE(shortcuts::validate({unnamed}, error));
  unnamed.name = "line\nbreak";
  EXPECT_FALSE(shortcuts::validate({unnamed}, error));
  auto empty {make("a", "START")};
  empty.trigger = shortcuts::combo_t {};
  EXPECT_FALSE(shortcuts::validate({empty}, error));
  auto silent {make("a", "START")};
  silent.output.keys.clear();
  EXPECT_FALSE(shortcuts::validate({silent}, error));
  std::vector<shortcuts::shortcut_t> many;
  for (std::size_t index {0}; index <= shortcuts::MAX_SHORTCUTS; ++index) {
    many.push_back(make(std::format("s{}", index), shortcuts::input_names()[index]));
  }
  EXPECT_FALSE(shortcuts::validate(many, error));

  const auto round_trip {shortcuts::from_json(shortcuts::to_json({make("a", "START+BACK", "HOME+A", false)}))};
  ASSERT_TRUE(round_trip);
  ASSERT_EQ(round_trip->size(), 1U);
  EXPECT_FALSE(round_trip->front().enabled);
  EXPECT_EQ(shortcuts::format_output(round_trip->front().output), "HOME+A");
  EXPECT_TRUE(shortcuts::from_json("")->empty());
  EXPECT_FALSE(shortcuts::from_json("{}"));
  EXPECT_FALSE(shortcuts::from_json(R"([{"id":"a","inputs":"START","hold_ms":1000}])"));
  EXPECT_FALSE(shortcuts::from_json(R"([{"id":"a","inputs":"START","hold_ms":1000,"output":"HOME","enabled":"yes"}])"));
  EXPECT_FALSE(shortcuts::from_json(R"([{"id":"a","inputs":"START","hold_ms":1000,"output":"HOME"},{"id":"b","inputs":"START","hold_ms":500,"output":"A"}])"));
}

/**
 * @brief Create, update, disable, and delete shortcuts with immediate publication.
 */
TEST(SteamshineGamepadShortcutsTest, PersistsChangesWithoutRestart) {
  const auto directory {std::filesystem::temp_directory_path() / std::format("steamshine-shortcuts-{}", std::chrono::steady_clock::now().time_since_epoch().count())};
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  const auto file {directory / "sunshine.conf"};
  {
    std::ofstream output {file};
    output << "locale = ja\nsteamshine_home_combo = BACK\nsteamshine_home_combo_hold_ms = 1000\n";
  }
  const auto previous_file {config::sunshine.config_file};
  config::sunshine.config_file = file.string();
  for (const auto &existing : shortcuts::current()) {
    std::string ignored;
    shortcuts::remove(existing.id, ignored);
  }

  std::string error;
  auto home {shortcuts::upsert({"", "Home", *shortcuts::parse("START+BACK", 3000), *shortcuts::parse_output("HOME"), true}, error)};
  ASSERT_TRUE(home) << error;
  EXPECT_FALSE(home->id.empty());
  const auto quick {shortcuts::upsert({"", "Decky", *shortcuts::parse("START+BACK+A", 1500), *shortcuts::parse_output("HOME+A"), true}, error)};
  ASSERT_TRUE(quick) << error;
  EXPECT_NE(quick->id, home->id);
  EXPECT_FALSE(shortcuts::upsert({"", "", *shortcuts::parse("BACK+START", 500), *shortcuts::parse_output("A"), true}, error));
  EXPECT_NE(error.find("same buttons"), std::string::npos);
  EXPECT_FALSE(shortcuts::upsert({"missing", "", *shortcuts::parse("Y", 500), *shortcuts::parse_output("A"), true}, error));

  std::stringstream saved;
  saved << std::ifstream {file}.rdbuf();
  EXPECT_NE(saved.str().find("locale = ja"), std::string::npos);
  EXPECT_NE(saved.str().find("steamshine_gamepad_shortcuts = [{"), std::string::npos);
  EXPECT_NE(saved.str().find(R"("output":"HOME+A")"), std::string::npos);
  EXPECT_EQ(saved.str().find("steamshine_home_combo"), std::string::npos);
  EXPECT_EQ(shortcuts::current().size(), 2U);
  EXPECT_EQ(config::sunshine.steamshine_gamepad_shortcuts, shortcuts::to_json(shortcuts::current()));

  home->enabled = false;
  ASSERT_TRUE(shortcuts::upsert(*home, error)) << error;
  EXPECT_FALSE(shortcuts::current().front().enabled);
  ASSERT_TRUE(shortcuts::remove(home->id, error)) << error;
  EXPECT_EQ(shortcuts::current().size(), 1U);
  EXPECT_FALSE(shortcuts::remove(home->id, error));

  // A missing directory leaves the published list unchanged.
  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  EXPECT_FALSE(shortcuts::remove(quick->id, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(shortcuts::current().size(), 1U);

  config::sunshine.config_file = file.string();
  ASSERT_TRUE(shortcuts::remove(quick->id, error)) << error;
  config::sunshine.config_file = previous_file;
  std::filesystem::remove_all(directory);
}
