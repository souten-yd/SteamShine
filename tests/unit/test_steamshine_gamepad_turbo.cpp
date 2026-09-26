/**
 * @file tests/unit/test_steamshine_gamepad_turbo.cpp
 * @brief Tests for controller turbo gestures, pulsing, and persistence.
 */

#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <src/config.h>
#include <src/steamshine_gamepad_turbo.h>
#include <sstream>
#include <string>

using namespace std::literals;
namespace turbo = steamshine_gamepad_turbo;
namespace shortcuts = steamshine_gamepad_shortcuts;

namespace {

  /**
   * @brief Build a gamepad state with the given buttons.
   *
   * @param buttons Button mask.
   * @return Gamepad state with released triggers and centered sticks.
   */
  platf::gamepad_state_t pad(const std::uint32_t buttons) {
    return platf::gamepad_state_t {buttons, 0, 0, 0, 0, 0, 0};
  }

  /**
   * @brief Build enabled settings.
   *
   * @param modifier Combination button.
   * @param hz Presses per second.
   * @return Settings.
   */
  turbo::settings_t settings(const std::string_view modifier = "START", const int hz = 10) {
    return {true, std::string {modifier}, hz};
  }

  /**
   * @brief Send one packet through the turbo gesture filter.
   *
   * @param tracker Turbo state.
   * @param configured Settings.
   * @param buttons Held buttons.
   * @param now Packet time.
   * @param state Host-visible input after gesture filtering.
   * @return Turbo changes.
   */
  std::vector<turbo::toggle_t> send(turbo::tracker_t &tracker, const turbo::settings_t &configured, const std::uint32_t buttons, const turbo::clock_t::time_point now, platf::gamepad_state_t *state = nullptr) {
    auto packet {pad(buttons)};
    auto changes {turbo::update(tracker, configured, packet, now)};
    if (state) {
      *state = packet;
    }
    return changes;
  }

  using toggles_t = std::vector<turbo::toggle_t>;  ///< Expected turbo changes.

}  // namespace

/**
 * @brief Offer every non-trigger input as the combination or a target.
 */
TEST(SteamshineGamepadTurboTest, ListsButtons) {
  EXPECT_EQ(turbo::button_names().size(), 14U);
  EXPECT_EQ(std::ranges::find(turbo::button_names(), "LT"sv), turbo::button_names().end());
  EXPECT_EQ(shortcuts::button_bit("a"), platf::A);
  EXPECT_FALSE(shortcuts::button_bit("RT"));
  EXPECT_EQ(shortcuts::button_name(platf::DPAD_UP), "UP");
  EXPECT_EQ(shortcuts::button_name(platf::HOME), "");
  EXPECT_EQ(turbo::settings_t {}.modifier, "START");
  EXPECT_FALSE(turbo::settings_t {}.enabled);
}

/**
 * @brief Start + A turns A's turbo on; A then pulses while held; Start + A turns it off.
 */
TEST(SteamshineGamepadTurboTest, TogglesAndPulsesHeldButtons) {
  const auto configured {settings()};
  turbo::tracker_t tracker;
  const auto start {turbo::clock_t::now()};
  platf::gamepad_state_t state {};

  EXPECT_EQ(send(tracker, configured, platf::START, start), toggles_t {});
  EXPECT_EQ(send(tracker, configured, platf::START | platf::A, start, &state), (toggles_t {{platf::A, true}}));
  // The gesture press never reaches the game.
  EXPECT_EQ(state.buttonFlags, platf::START);
  EXPECT_FALSE(turbo::ticking(tracker));
  send(tracker, configured, platf::START, start);
  send(tracker, configured, 0, start);

  // Holding A now pulses at 10 Hz: 50 ms pressed, 50 ms released.
  send(tracker, configured, platf::A, start);
  EXPECT_TRUE(turbo::ticking(tracker));
  EXPECT_EQ(turbo::render(tracker, start).buttonFlags, platf::A);
  EXPECT_EQ(turbo::render(tracker, start + 49ms).buttonFlags, platf::A);
  EXPECT_EQ(turbo::render(tracker, start + 50ms).buttonFlags, 0U);
  EXPECT_EQ(turbo::render(tracker, start + 100ms).buttonFlags, platf::A);
  send(tracker, configured, 0, start + 120ms);
  EXPECT_FALSE(turbo::ticking(tracker));

  // A new press restarts the phase so the first press is never lost.
  send(tracker, configured, platf::A, start + 170ms);
  EXPECT_EQ(turbo::render(tracker, start + 170ms).buttonFlags, platf::A);
  // Pressing Start while A is already held does not toggle A.
  EXPECT_EQ(send(tracker, configured, platf::A | platf::START, start + 175ms), toggles_t {});
  send(tracker, configured, 0, start + 180ms);

  send(tracker, configured, platf::START, start + 200ms);
  EXPECT_EQ(send(tracker, configured, platf::START | platf::A, start + 200ms), (toggles_t {{platf::A, false}}));
  send(tracker, configured, 0, start + 210ms);
  send(tracker, configured, platf::A, start + 220ms);
  EXPECT_FALSE(turbo::ticking(tracker));
  EXPECT_EQ(turbo::render(tracker, start + 280ms).buttonFlags, platf::A);
}

/**
 * @brief Toggle several buttons at once, follow speed changes, and use another combination button.
 */
TEST(SteamshineGamepadTurboTest, TogglesSeveralButtonsAndFollowsSpeed) {
  turbo::tracker_t tracker;
  const auto now {turbo::clock_t::now()};
  send(tracker, settings("BACK"), platf::BACK, now);
  EXPECT_EQ(send(tracker, settings("BACK"), platf::BACK | platf::X | platf::Y, now), (toggles_t {{platf::X, true}, {platf::Y, true}}));
  send(tracker, settings("BACK"), 0, now);
  // Start is an ordinary button when Back is the combination button.
  EXPECT_EQ(send(tracker, settings("BACK"), platf::START, now), toggles_t {});

  send(tracker, settings("BACK", 20), platf::X, now);
  EXPECT_EQ(tracker.hz, 20);
  EXPECT_EQ(turbo::render(tracker, now + 25ms).buttonFlags, 0U);
  EXPECT_EQ(turbo::render(tracker, now + 50ms).buttonFlags, platf::X);
}

/**
 * @brief Turning turbo off clears every turbo button and passes input through.
 */
TEST(SteamshineGamepadTurboTest, DisabledSettingsClearTurbo) {
  auto configured {settings()};
  turbo::tracker_t tracker;
  const auto now {turbo::clock_t::now()};
  send(tracker, configured, platf::START, now);
  send(tracker, configured, platf::START | platf::A, now);
  send(tracker, configured, 0, now);

  configured.enabled = false;
  platf::gamepad_state_t state {};
  EXPECT_EQ(send(tracker, configured, platf::START | platf::A, now, &state), (toggles_t {{platf::A, false}}));
  EXPECT_EQ(state.buttonFlags, platf::START | platf::A);
  EXPECT_EQ(tracker.active, 0U);

  tracker.active = platf::B;
  tracker.pressed_since[platf::B] = now;
  turbo::clear(tracker);
  EXPECT_EQ(tracker.active, 0U);
  EXPECT_TRUE(tracker.pressed_since.empty());
}

/**
 * @brief Validate the combination button and speed, and round-trip the persisted JSON.
 */
TEST(SteamshineGamepadTurboTest, ValidatesAndSerializes) {
  std::string error;
  EXPECT_TRUE(turbo::validate(turbo::settings_t {}, error));
  EXPECT_FALSE(turbo::validate(settings("START", 0), error));
  EXPECT_FALSE(turbo::validate(settings("START", 31), error));
  EXPECT_FALSE(turbo::validate(settings("LT"), error));
  EXPECT_FALSE(turbo::validate(settings("START+A"), error));
  EXPECT_FALSE(turbo::validate(settings("HOME"), error));

  const auto round_trip {turbo::from_json(turbo::to_json(settings("back", 12)))};
  ASSERT_TRUE(round_trip);
  EXPECT_TRUE(round_trip->enabled);
  EXPECT_EQ(round_trip->modifier, "BACK");
  EXPECT_EQ(round_trip->hz, 12);
  EXPECT_EQ(turbo::from_json("")->modifier, "START");
  EXPECT_FALSE(turbo::from_json("[]"));
  EXPECT_FALSE(turbo::from_json(R"({"enabled":true,"modifier":"START"})"));
  EXPECT_FALSE(turbo::from_json(R"({"enabled":true,"modifier":"RT","hz":10})"));
}

/**
 * @brief Persist settings without restarting and keep other configuration values.
 */
TEST(SteamshineGamepadTurboTest, PersistsWithoutRestart) {
  const auto directory {std::filesystem::temp_directory_path() / std::format("steamshine-turbo-{}", std::chrono::steady_clock::now().time_since_epoch().count())};
  ASSERT_TRUE(std::filesystem::create_directories(directory));
  const auto file {directory / "sunshine.conf"};
  {
    std::ofstream output {file};
    output << "locale = ja\n";
  }
  const auto previous_file {config::sunshine.config_file};
  config::sunshine.config_file = file.string();

  std::string error;
  ASSERT_TRUE(turbo::set(settings("back", 15), error)) << error;
  std::stringstream saved;
  saved << std::ifstream {file}.rdbuf();
  EXPECT_NE(saved.str().find("locale = ja"), std::string::npos);
  EXPECT_NE(saved.str().find(R"(steamshine_gamepad_turbo = {"enabled":true,"hz":15,"modifier":"BACK"})"), std::string::npos);
  EXPECT_EQ(turbo::current().modifier, "BACK");
  EXPECT_EQ(config::sunshine.steamshine_gamepad_turbo, turbo::to_json(turbo::current()));
  EXPECT_FALSE(turbo::set(settings("START", 99), error));
  EXPECT_EQ(turbo::current().hz, 15);

  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  EXPECT_FALSE(turbo::set(turbo::settings_t {}, error));
  EXPECT_TRUE(turbo::current().enabled);

  config::sunshine.config_file = file.string();
  ASSERT_TRUE(turbo::set(turbo::settings_t {}, error)) << error;
  config::sunshine.config_file = previous_file;
  std::filesystem::remove_all(directory);
}
