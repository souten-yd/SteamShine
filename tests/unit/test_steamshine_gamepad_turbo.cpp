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
   * @brief Build enabled settings with up to two used presets.
   *
   * @param first First preset modifier.
   * @param first_hz First preset frequency.
   * @param second Second preset modifier.
   * @param second_hz Second preset frequency.
   * @return Settings.
   */
  turbo::settings_t settings(const std::string_view first, const int first_hz, const std::string_view second = {}, const int second_hz = 20) {
    turbo::settings_t result;
    result.enabled = true;
    result.presets[0] = {*shortcuts::parse(first, 1000), first_hz};
    result.presets[1] = {*shortcuts::parse(second, 1000), second_hz};
    return result;
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
 * @brief Offer every non-trigger input as a turbo target.
 */
TEST(SteamshineGamepadTurboTest, ListsTargets) {
  EXPECT_EQ(turbo::target_names().size(), 14U);
  EXPECT_EQ(std::ranges::find(turbo::target_names(), "LT"sv), turbo::target_names().end());
  EXPECT_EQ(shortcuts::button_bit("a"), platf::A);
  EXPECT_FALSE(shortcuts::button_bit("RT"));
  EXPECT_EQ(shortcuts::button_name(platf::DPAD_UP), "UP");
  EXPECT_EQ(shortcuts::button_name(platf::HOME), "");
}

/**
 * @brief Toggle turbo on, pulse while held, and toggle it off with the same gesture.
 */
TEST(SteamshineGamepadTurboTest, TogglesAndPulsesHeldButtons) {
  const auto configured {settings("BACK+RB", 10)};
  turbo::tracker_t tracker;
  const auto start {turbo::clock_t::now()};
  platf::gamepad_state_t state {};

  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON, start), toggles_t {});
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON | platf::A, start, &state), (toggles_t {{platf::A, 10}}));
  // The gesture press never reaches the game.
  EXPECT_EQ(state.buttonFlags, platf::BACK | platf::RIGHT_BUTTON);
  EXPECT_FALSE(turbo::ticking(tracker));
  send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON, start);
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
  EXPECT_EQ(turbo::render(tracker, start + 150ms).buttonFlags, 0U);

  // A new press restarts the phase so the first press is never lost.
  send(tracker, configured, platf::A, start + 170ms);
  EXPECT_EQ(turbo::render(tracker, start + 170ms).buttonFlags, platf::A);
  send(tracker, configured, 0, start + 180ms);

  send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON, start + 200ms);
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON | platf::A, start + 200ms), (toggles_t {{platf::A, 0}}));
  send(tracker, configured, 0, start + 210ms);
  send(tracker, configured, platf::A, start + 220ms);
  EXPECT_EQ(turbo::render(tracker, start + 280ms).buttonFlags, platf::A);
}

/**
 * @brief Change frequency with another preset and prefer the preset with more inputs.
 */
TEST(SteamshineGamepadTurboTest, ChangesFrequencyAndPrefersLargerPreset) {
  const auto configured {settings("BACK", 5, "BACK+RB", 20)};
  turbo::tracker_t tracker;
  const auto now {turbo::clock_t::now()};

  send(tracker, configured, platf::BACK, now);
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::X | platf::Y, now), (toggles_t {{platf::X, 5}, {platf::Y, 5}}));
  send(tracker, configured, platf::BACK, now);
  // RB completes the larger preset, so it is a modifier rather than a target.
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON, now), toggles_t {});
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::RIGHT_BUTTON | platf::X, now), (toggles_t {{platf::X, 20}}));
  EXPECT_EQ(tracker.active.at(platf::X), 20);
  EXPECT_EQ(tracker.active.at(platf::Y), 5);
}

/**
 * @brief Turning turbo off clears every turbo button and passes input through.
 */
TEST(SteamshineGamepadTurboTest, DisabledSettingsClearTurbo) {
  auto configured {settings("BACK", 10)};
  turbo::tracker_t tracker;
  const auto now {turbo::clock_t::now()};
  send(tracker, configured, platf::BACK, now);
  send(tracker, configured, platf::BACK | platf::A, now);
  send(tracker, configured, 0, now);

  configured.enabled = false;
  platf::gamepad_state_t state {};
  EXPECT_EQ(send(tracker, configured, platf::BACK | platf::A, now, &state), (toggles_t {{platf::A, 0}}));
  EXPECT_EQ(state.buttonFlags, platf::BACK | platf::A);
  EXPECT_TRUE(tracker.active.empty());
  EXPECT_FALSE(turbo::ticking(tracker));
}

/**
 * @brief Validate ranges and duplicate presets, and round-trip the persisted JSON.
 */
TEST(SteamshineGamepadTurboTest, ValidatesAndSerializes) {
  std::string error;
  EXPECT_TRUE(turbo::validate(turbo::settings_t {}, error));
  EXPECT_FALSE(turbo::validate(settings("BACK", 0), error));
  EXPECT_FALSE(turbo::validate(settings("BACK", 31), error));
  EXPECT_FALSE(turbo::validate(settings("BACK+RB", 10, "RB+BACK", 20), error));
  EXPECT_NE(error.find("same buttons"), std::string::npos);

  const auto round_trip {turbo::from_json(turbo::to_json(settings("BACK+RB", 12, "LT", 25)))};
  ASSERT_TRUE(round_trip);
  EXPECT_TRUE(round_trip->enabled);
  EXPECT_EQ(round_trip->presets[0].hz, 12);
  EXPECT_EQ(shortcuts::format_inputs(round_trip->presets[1].modifier), "LT");
  EXPECT_FALSE(round_trip->presets[2].modifier.enabled());
  EXPECT_EQ(turbo::from_json("")->presets[3].hz, 20);
  EXPECT_FALSE(turbo::from_json("[]"));
  EXPECT_FALSE(turbo::from_json(R"({"enabled":true,"presets":[]})"));
  EXPECT_FALSE(turbo::from_json(R"({"enabled":true,"presets":[{"inputs":"BACK","hz":10},{"inputs":"","hz":10},{"inputs":"","hz":10},{"inputs":"HOME","hz":10}]})"));
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
  ASSERT_TRUE(turbo::set(settings("BACK+RB", 15), error)) << error;
  std::stringstream saved;
  saved << std::ifstream {file}.rdbuf();
  EXPECT_NE(saved.str().find("locale = ja"), std::string::npos);
  EXPECT_NE(saved.str().find(R"(steamshine_gamepad_turbo = {"enabled":true)"), std::string::npos);
  EXPECT_EQ(turbo::current().presets[0].hz, 15);
  EXPECT_EQ(config::sunshine.steamshine_gamepad_turbo, turbo::to_json(turbo::current()));
  EXPECT_FALSE(turbo::set(settings("BACK", 99), error));
  EXPECT_EQ(turbo::current().presets[0].hz, 15);

  config::sunshine.config_file = (directory / "missing" / "sunshine.conf").string();
  EXPECT_FALSE(turbo::set(turbo::settings_t {}, error));
  EXPECT_TRUE(turbo::current().enabled);

  config::sunshine.config_file = file.string();
  ASSERT_TRUE(turbo::set(turbo::settings_t {}, error)) << error;
  config::sunshine.config_file = previous_file;
  std::filesystem::remove_all(directory);
}
