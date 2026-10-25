/**
 * @file tests/unit/test_steamshine_x52.cpp
 * @brief Validation tests for standard X52 HOTAS profiles.
 */
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <src/steamshine_x52.h>

/** @brief Check the recommended profile is safe and has the hardware-supported fields. */
TEST(SteamshineX52Test, RecommendedProfileIsValidAndDisabled) {
  std::string error;
  const auto recommended = steamshine_x52::recommended_profile();
  ASSERT_TRUE(recommended.is_object()) << recommended.dump();
  EXPECT_FALSE(recommended["enabled"].get<bool>());
  EXPECT_TRUE(recommended["buttons"].empty());
  EXPECT_EQ(recommended["display"]["lines"].size(), 3);
  EXPECT_EQ(steamshine_x52::validate_profile(recommended, error), recommended);
}

/** @brief Keep the built-in profile available through the Addon status API. */
TEST(SteamshineX52Test, StatusContainsRecommendedProfileAndCapabilities) {
  const auto status = steamshine_x52::status();
  ASSERT_TRUE(status.is_object());
  ASSERT_TRUE(status["profiles"].is_array());
  ASSERT_FALSE(status["profiles"].empty());
  EXPECT_EQ(status["active"], "Recommended");
  EXPECT_EQ(status["profiles"][0]["name"], "Recommended");
  EXPECT_EQ(status["capabilities"]["individual_led_colors"], false);
  EXPECT_TRUE(status["connected"].is_boolean());
  EXPECT_TRUE(status["input_enabled"].is_boolean());
}

/** @brief Reject unsupported LED, display, and button values before reaching the device. */
TEST(SteamshineX52Test, RejectsInvalidHardwareSettings) {
  const auto base = steamshine_x52::recommended_profile();
  std::string error;
  auto invalid = base;
  invalid["lighting"]["brightness"] = 129;
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["lighting"]["brightness"] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["display"]["lines"][0] = "This line is longer than sixteen";
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["display"]["lines"][0] = "日本語";
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["display"]["date_format"] = "invalid";
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["display"]["clock_offsets_minutes"][0] = 1024;
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["display"]["clock_offsets_minutes"][0] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["buttons"]["34"] = "A";
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
  invalid = base;
  invalid["buttons"]["0"] = "DELETE_ALL";
  EXPECT_TRUE(steamshine_x52::validate_profile(invalid, error).is_discarded());
}

/** @brief Preserve only supported keyboard assignments in a normalized profile. */
TEST(SteamshineX52Test, AcceptsSupportedButtonKeys) {
  auto candidate = steamshine_x52::recommended_profile();
  candidate["name"] = "Flight";
  candidate["buttons"] = {{"0", "SPACE"}, {"33", "F12"}, {"12", "NONE"}};
  std::string error;
  const auto validated = steamshine_x52::validate_profile(candidate, error);
  ASSERT_FALSE(validated.is_discarded()) << error;
  EXPECT_EQ(validated["buttons"].size(), 2);
  EXPECT_EQ(validated["buttons"]["0"], "SPACE");
  EXPECT_EQ(validated["buttons"]["33"], "F12");
}
