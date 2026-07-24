/**
 * @file tests/unit/test_steamos_virtual_session_core.cpp
 * @brief Tests for standalone SteamOS virtual-session request helpers.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <src/steamos_virtual_session_core.h>

namespace {
  /**
   * @brief Verify missing and out-of-range client values use safe display bounds.
   */
  TEST(SteamOSVirtualSessionCore, NormalizesDisplayRequest) {
    const auto defaults {steamos_virtual_session::normalize_display_request(0, -1, 0, 1920, 1080, 60)};
    EXPECT_EQ(defaults.width, 1920);
    EXPECT_EQ(defaults.height, 1080);
    EXPECT_EQ(defaults.fps, 60);

    const auto bounds {steamos_virtual_session::normalize_display_request(1, 9000, 999, 1920, 1080, 60)};
    EXPECT_EQ(bounds.width, 640);
    EXPECT_EQ(bounds.height, 4320);
    EXPECT_EQ(bounds.fps, 240);
  }

  /**
   * @brief Verify command generation only uses Gamescope-advertised options.
   */
  TEST(SteamOSVirtualSessionCore, BuildsHeadlessGamescopeCommand) {
    std::string error;
    const auto arguments {steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --nested-refresh --expose-wayland --scaler --hdr-enabled --prefer-vk-device", 2560, 1440, 120, true, "1002:744c", error)};
    EXPECT_TRUE(error.empty());
    EXPECT_THAT(arguments, ::testing::ElementsAre("--backend", "headless", "--nested-width", "2560", "--nested-height", "1440", "--nested-refresh", "120", "--expose-wayland", "--scaler", "fit", "--hdr-enabled", "--prefer-vk-device", "1002:744c"));
  }

  /**
   * @brief Verify missing required Gamescope capability fails closed.
   */
  TEST(SteamOSVirtualSessionCore, RejectsUnsupportedGamescope) {
    std::string error;
    EXPECT_TRUE(steamos_virtual_session::gamescope_arguments("--nested-width --nested-height", 1920, 1080, 60, false, {}, error).empty());
    EXPECT_FALSE(error.empty());
  }
}  // namespace
