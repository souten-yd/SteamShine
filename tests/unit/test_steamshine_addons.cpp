/**
 * @file tests/unit/test_steamshine_addons.cpp
 * @brief Tests for allow-listed SteamShine addon lifecycle operations.
 */

#include "../tests_common.h"

#include <src/steamshine_addons.h>

/**
 * @brief Verify the Addon API cannot turn arbitrary text into a command.
 */
TEST(SteamshineAddonsTest, ParsesOnlySupportedDeckyActions) {
  using steamshine_addons::decky_action_e;
  EXPECT_EQ(steamshine_addons::parse_decky_action("install"), decky_action_e::install);
  EXPECT_EQ(steamshine_addons::parse_decky_action("update"), decky_action_e::update);
  EXPECT_EQ(steamshine_addons::parse_decky_action("uninstall"), decky_action_e::uninstall);
  EXPECT_FALSE(steamshine_addons::parse_decky_action("start"));
  EXPECT_FALSE(steamshine_addons::parse_decky_action("install; id"));
  EXPECT_FALSE(steamshine_addons::parse_decky_action(""));
}

/**
 * @brief Verify lifecycle actions produce exact shell-free sudo arguments.
 */
TEST(SteamshineAddonsTest, BuildsFixedDeckyHelperArguments) {
  using steamshine_addons::decky_action_e;
  const std::string helper {"/root-owned/decky-helper"};

  EXPECT_EQ(
    steamshine_addons::decky_helper_arguments(decky_action_e::install, helper),
    (std::vector<std::string> {"/usr/bin/sudo", "-n", helper, "install"})
  );
  EXPECT_EQ(
    steamshine_addons::decky_helper_arguments(decky_action_e::update, helper),
    (std::vector<std::string> {"/usr/bin/sudo", "-n", helper, "update"})
  );
  EXPECT_EQ(
    steamshine_addons::decky_helper_arguments(decky_action_e::uninstall, helper),
    (std::vector<std::string> {"/usr/bin/sudo", "-n", helper, "uninstall"})
  );
  EXPECT_EQ(
    steamshine_addons::decky_helper_arguments(decky_action_e::start, helper),
    (std::vector<std::string> {"/usr/bin/sudo", "-n", helper, "start"})
  );
}

/**
 * @brief Verify Decky status and action results expose stable JSON fields.
 */
TEST(SteamshineAddonsTest, SerializesDeckyState) {
  const steamshine_addons::decky_status_t status {
    .installed = true,
    .version = "v3.2.9",
    .service_enabled = true,
    .service_active = false,
    .management_available = true,
  };
  const nlohmann::json status_json = status;
  EXPECT_TRUE(status_json.at("installed"));
  EXPECT_EQ(status_json.at("version"), "v3.2.9");
  EXPECT_FALSE(status_json.at("service_active"));
  EXPECT_TRUE(status_json.at("management_available"));

  const steamshine_addons::decky_action_result_t result {
    .success = false,
    .exit_code = 1,
    .message = "failed",
    .status = status,
  };
  const nlohmann::json result_json = result;
  EXPECT_FALSE(result_json.at("success"));
  EXPECT_EQ(result_json.at("exit_code"), 1);
  EXPECT_EQ(result_json.at("status").at("version"), "v3.2.9");
}

/**
 * @brief Verify owned Gamescope starts only an installed inactive Decky service.
 */
TEST(SteamshineAddonsTest, StartsInactiveInstalledDeckyForOwnedSession) {
  steamshine_addons::decky_status_t status;
  EXPECT_FALSE(steamshine_addons::decky_start_required(status));
  status.installed = true;
  EXPECT_TRUE(steamshine_addons::decky_start_required(status));
  status.service_active = true;
  EXPECT_FALSE(steamshine_addons::decky_start_required(status));
}
