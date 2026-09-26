/**
 * @file tests/unit/test_steamshine_addons.cpp
 * @brief Tests for allow-listed SteamShine addon lifecycle operations.
 */

#include "../tests_common.h"

#include <src/steamshine_addons.h>
#include <src/steamshine_gpuctl.h>

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

/**
 * @brief Reject password values that could inject additional sudo input or exceed the request budget.
 */
TEST(SteamshineAddonsTest, ValidatesTransientAdministratorPassword) {
  EXPECT_TRUE(steamshine_addons::management_password_valid("a valid password"));
  EXPECT_TRUE(steamshine_addons::management_password_valid(std::string(1024, 'x')));
  EXPECT_FALSE(steamshine_addons::management_password_valid(""));
  EXPECT_FALSE(steamshine_addons::management_password_valid(std::string(1025, 'x')));
  EXPECT_FALSE(steamshine_addons::management_password_valid("line\ncommand"));
  EXPECT_FALSE(steamshine_addons::management_password_valid("line\rcommand"));
  EXPECT_FALSE(steamshine_addons::management_password_valid(std::string("a\0b", 3)));
  EXPECT_FALSE(steamshine_addons::management_ready("arbitrary"));
  EXPECT_FALSE(steamshine_addons::authorize_management("").value("success", true));
}

/**
 * @brief Cache setup accepts opaque library IDs and cannot become an arbitrary command.
 */
TEST(SteamshineAddonsTest, RestrictsSteamCacheHelperArguments) {
  const std::string helper {"/package/scripts/steamshine-steam-cache.py"};
  ASSERT_TRUE(steamshine_addons::steam_cache_helper_arguments(helper));
  EXPECT_EQ(steamshine_addons::steam_cache_helper_arguments(helper)->back(), "status");
  const auto valid {steamshine_addons::steam_cache_helper_arguments(helper, "0123456789abcdef01234567")};
  ASSERT_TRUE(valid);
  EXPECT_EQ(*valid, (std::vector<std::string> {"/usr/bin/python3", helper, "configure", "0123456789abcdef01234567"}));
  EXPECT_FALSE(steamshine_addons::steam_cache_helper_arguments(helper, "../../home"));
  EXPECT_FALSE(steamshine_addons::steam_cache_helper_arguments(helper, "0123456789abcdef0123456;"));
  EXPECT_FALSE(steamshine_addons::configure_steam_cache("").at("success"));
}

/**
 * @brief Storage recovery permits only fixed operations and exact UUIDs.
 */
TEST(SteamshineAddonsTest, RestrictsStorageHelperArguments) {
  EXPECT_TRUE(steamshine_addons::storage_helper_arguments("status"));
  EXPECT_TRUE(steamshine_addons::storage_helper_arguments("remember"));
  const auto restore {steamshine_addons::storage_helper_arguments("restore", "19d3483c-a24c-4c26-a7fc-e5d622399d1d")};
  ASSERT_TRUE(restore);
  EXPECT_EQ(restore->size(), 5);
  EXPECT_FALSE(steamshine_addons::storage_helper_arguments("restore", "/dev/sda"));
  EXPECT_FALSE(steamshine_addons::storage_helper_arguments("status", "extra"));
  EXPECT_FALSE(steamshine_addons::storage_helper_arguments("format"));
  EXPECT_FALSE(steamshine_addons::storage_action("format").at("success"));
  EXPECT_FALSE(steamshine_addons::management_ready("arbitrary-helper"));
}

/**
 * @brief Management JSON remains an object even when packaged helpers are unavailable.
 */
TEST(SteamshineAddonsTest, ReturnsObjectResponsesWithoutInstalledHelpers) {
  const auto cache = steamshine_addons::steam_cache_status();
  ASSERT_TRUE(cache.is_object());
  EXPECT_TRUE(cache.at("libraries").is_array());
  const auto storage = steamshine_addons::storage_status();
  ASSERT_TRUE(storage.is_object());
  EXPECT_TRUE(storage.at("volumes").is_array());
  EXPECT_FALSE(steamshine_addons::authorize_management("").at("success"));
}
