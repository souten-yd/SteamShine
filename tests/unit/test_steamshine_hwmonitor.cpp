/**
 * @file tests/unit/test_steamshine_hwmonitor.cpp
 * @brief Tests for persisted GPU profiles and SteamShine Monitor telemetry presentation.
 */

#include "../tests_common.h"

#include <filesystem>
#include <limits>
#include <src/config.h>
#include <src/file_handler.h>
#include <src/steamshine_gpuctl.h>
#include <src/steamshine_hwmonitor.h>
#include <src/utility.h>

/**
 * @brief Verify the GPU tab's stored selection resolves its named preset.
 */
TEST(SteamshineGpuControlTest, ResolvesSelectedProfile) {
  const auto saved_active {config::sunshine.steamshine_gpu_active_profile};
  const auto restore {util::fail_guard([&]() {
    config::sunshine.steamshine_gpu_active_profile = saved_active;
  })};
  config::sunshine.steamshine_gpu_active_profile = "Balanced";

  const auto selected {steamshine_gpuctl::active_profile()};
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->name, "Balanced");

  config::sunshine.steamshine_gpu_active_profile = "Deleted preset";
  EXPECT_FALSE(steamshine_gpuctl::active_profile());

  const auto missing {steamshine_gpuctl::activate_profile("Deleted preset")};
  EXPECT_FALSE(missing.success);
  EXPECT_EQ(missing.error, "Profile not found");
  EXPECT_TRUE(missing.applied.empty());
  EXPECT_TRUE(missing.skipped.empty());

  const auto reapplied {steamshine_gpuctl::reapply_active_profile()};
  EXPECT_TRUE(reapplied.success);
  EXPECT_TRUE(reapplied.error.empty());
}

/**
 * @brief Load an existing custom profile without changing its settings or selection.
 */
TEST(SteamshineGpuControlTest, LoadsSavedAlternativeProfile) {
  const auto saved_profiles {config::sunshine.steamshine_gpu_profiles};
  const auto saved_active {config::sunshine.steamshine_gpu_active_profile};
  const auto restore {util::fail_guard([&]() {
    config::sunshine.steamshine_gpu_profiles = saved_profiles;
    config::sunshine.steamshine_gpu_active_profile = saved_active;
  })};
  const std::string stored {R"([{"builtin":false,"cpu_governor":"powersave","cpu_max_freq_mhz":3600.0,"description":"","gpu_clock_offset_mhz":0,"gpu_voltage_offset_mv":0,"name":"Alternative","power_cap_watts":260.0}])"};
  config::sunshine.steamshine_gpu_profiles = stored;
  config::sunshine.steamshine_gpu_active_profile = "Alternative";

  const auto profiles {steamshine_gpuctl::custom_profiles()};
  ASSERT_EQ(profiles.size(), 1);
  EXPECT_EQ(profiles.front().name, "Alternative");
  EXPECT_FALSE(profiles.front().builtin);
  EXPECT_EQ(profiles.front().cpu_governor, "powersave");
  EXPECT_DOUBLE_EQ(profiles.front().power_cap_watts, 260.0);
  EXPECT_DOUBLE_EQ(profiles.front().cpu_max_freq_mhz, 3600.0);
  EXPECT_EQ(profiles.front().gpu_clock_offset_mhz, 0);
  EXPECT_EQ(profiles.front().gpu_voltage_offset_mv, 0);
  const auto selected {steamshine_gpuctl::active_profile()};
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->name, "Alternative");
  EXPECT_EQ(config::sunshine.steamshine_gpu_profiles, stored);
  EXPECT_EQ(config::sunshine.steamshine_gpu_active_profile, "Alternative");
}

/**
 * @brief Preserve existing custom profiles through save, edit, reload, and delete operations.
 */
TEST(SteamshineGpuControlTest, PreservesCustomProfilesAcrossEdits) {
  namespace fs = std::filesystem;
  const auto saved_profiles {config::sunshine.steamshine_gpu_profiles};
  const auto saved_config_file {config::sunshine.config_file};
  const auto temporary_config {fs::temp_directory_path() / "steamshine-gpu-profile-regression.conf"};
  const auto restore {util::fail_guard([&]() {
    config::sunshine.steamshine_gpu_profiles = saved_profiles;
    config::sunshine.config_file = saved_config_file;
    fs::remove(temporary_config);
  })};
  ASSERT_EQ(file_handler::write_file(temporary_config.string().c_str(), "custom_option = preserved\n"), 0);
  config::sunshine.config_file = temporary_config.string();
  config::sunshine.steamshine_gpu_profiles = "[]";
  steamshine_gpuctl::profile_t profile {
    .name = "Alternative",
    .power_cap_watts = 260.0,
    .cpu_governor = "powersave",
    .cpu_max_freq_mhz = 3600.0,
  };
  std::string error;
  ASSERT_TRUE(steamshine_gpuctl::save_custom_profile(profile, error)) << error;
  profile.name = "Second profile";
  ASSERT_TRUE(steamshine_gpuctl::save_custom_profile(profile, error)) << error;
  auto profiles {steamshine_gpuctl::custom_profiles()};
  ASSERT_EQ(profiles.size(), 2);
  EXPECT_EQ(profiles.front().name, "Alternative");
  EXPECT_EQ(profiles.back().name, "Second profile");

  profile.description = "Edited profile";
  ASSERT_TRUE(steamshine_gpuctl::save_custom_profile(profile, error)) << error;
  const auto vars = config::parse_config(file_handler::read_file(temporary_config.string().c_str()));
  EXPECT_EQ(vars.at("custom_option"), "preserved");
  config::sunshine.steamshine_gpu_profiles = vars.at("steamshine_gpu_profiles");
  profiles = steamshine_gpuctl::custom_profiles();
  ASSERT_EQ(profiles.size(), 2);
  EXPECT_EQ(profiles.back().description, "Edited profile");

  ASSERT_TRUE(steamshine_gpuctl::delete_custom_profile("Second profile", error)) << error;
  profiles = steamshine_gpuctl::custom_profiles();
  ASSERT_EQ(profiles.size(), 1);
  EXPECT_EQ(profiles.front().name, "Alternative");
  ASSERT_TRUE(steamshine_gpuctl::delete_custom_profile("Alternative", error)) << error;
  EXPECT_TRUE(steamshine_gpuctl::custom_profiles().empty());
  EXPECT_EQ(config::sunshine.steamshine_gpu_profiles, "[]");
}

/**
 * @brief Verify Monitor preserves the live cap and reports a matching selected preset.
 */
TEST(SteamshineHardwareMonitorTest, AnnotatesAppliedSelectedPresetPowerCap) {
  steamshine_hwmonitor::metrics_snapshot_t snapshot;
  snapshot.gpu.emplace();
  snapshot.gpu->power_cap_watts = 225.5;

  steamshine_hwmonitor::annotate_selected_profile_power_cap(snapshot, 225.0);

  ASSERT_TRUE(snapshot.gpu->power_cap_watts);
  EXPECT_DOUBLE_EQ(*snapshot.gpu->power_cap_watts, 225.5);
  ASSERT_TRUE(snapshot.gpu->selected_power_cap_watts);
  EXPECT_DOUBLE_EQ(*snapshot.gpu->selected_power_cap_watts, 225.0);
  ASSERT_TRUE(snapshot.gpu->selected_power_cap_applied);
  EXPECT_TRUE(*snapshot.gpu->selected_power_cap_applied);
}

/**
 * @brief Verify Monitor exposes a selected limit that the driver did not apply.
 */
TEST(SteamshineHardwareMonitorTest, ReportsUnappliedSelectedPresetPowerCap) {
  steamshine_hwmonitor::metrics_snapshot_t snapshot;
  snapshot.gpu.emplace();
  snapshot.gpu->power_cap_watts = 330.0;

  steamshine_hwmonitor::annotate_selected_profile_power_cap(snapshot, 247.35);

  EXPECT_DOUBLE_EQ(*snapshot.gpu->power_cap_watts, 330.0);
  EXPECT_DOUBLE_EQ(*snapshot.gpu->selected_power_cap_watts, 247.35);
  ASSERT_TRUE(snapshot.gpu->selected_power_cap_applied);
  EXPECT_FALSE(*snapshot.gpu->selected_power_cap_applied);
}

/**
 * @brief Verify invalid or absent preset limits retain live hwmon telemetry.
 */
TEST(SteamshineHardwareMonitorTest, PreservesHardwareCapForUnresolvedPreset) {
  steamshine_hwmonitor::metrics_snapshot_t snapshot;
  snapshot.gpu.emplace();
  snapshot.gpu->power_cap_watts = 300.0;

  for (const auto cap : {
         std::optional<double> {},
         std::optional<double> {0.0},
         std::optional<double> {-1.0},
         std::optional<double> {std::numeric_limits<double>::quiet_NaN()},
       }) {
    steamshine_hwmonitor::annotate_selected_profile_power_cap(snapshot, cap);
    ASSERT_TRUE(snapshot.gpu->power_cap_watts);
    EXPECT_DOUBLE_EQ(*snapshot.gpu->power_cap_watts, 300.0);
    EXPECT_FALSE(snapshot.gpu->selected_power_cap_watts);
    EXPECT_FALSE(snapshot.gpu->selected_power_cap_applied);
  }

  steamshine_hwmonitor::metrics_snapshot_t without_gpu;
  steamshine_hwmonitor::annotate_selected_profile_power_cap(without_gpu, 225.0);
  EXPECT_FALSE(without_gpu.gpu);
}

/**
 * @brief Refuse to overwrite unreadable profile lists, including partially valid arrays.
 */
TEST(SteamshineGpuControlTest, RejectsMutationOfMalformedStoredProfiles) {
  const auto saved = config::sunshine.steamshine_gpu_profiles;
  const auto restore = util::fail_guard([&]() {
    config::sunshine.steamshine_gpu_profiles = saved;
  });
  for (const std::string value : {"not-json", "{}", R"([{"name":"Existing"},[]])"}) {
    config::sunshine.steamshine_gpu_profiles = value;
    steamshine_gpuctl::profile_t profile {.name = "New"};
    std::string error;
    EXPECT_FALSE(steamshine_gpuctl::save_custom_profile(profile, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(steamshine_gpuctl::delete_custom_profile("Existing", error));
    EXPECT_EQ(config::sunshine.steamshine_gpu_profiles, value);
  }
}

/**
 * @brief A failed disk write must not report success or change the in-memory profile list.
 */
TEST(SteamshineGpuControlTest, FailedSaveAndDeletePreserveProfiles) {
  const auto saved = config::sunshine.steamshine_gpu_profiles;
  const auto saved_path = config::sunshine.config_file;
  const auto restore = util::fail_guard([&]() {
    config::sunshine.steamshine_gpu_profiles = saved;
    config::sunshine.config_file = saved_path;
  });
  const std::string existing = R"([{"name":"Existing","power_cap_watts":250}])";
  config::sunshine.steamshine_gpu_profiles = existing;
  config::sunshine.config_file = "/dev/null/cannot-save.conf";
  steamshine_gpuctl::profile_t profile {.name = "New"};
  std::string error;
  EXPECT_FALSE(steamshine_gpuctl::save_custom_profile(profile, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(config::sunshine.steamshine_gpu_profiles, existing);
  EXPECT_FALSE(steamshine_gpuctl::delete_custom_profile("Existing", error));
  EXPECT_EQ(config::sunshine.steamshine_gpu_profiles, existing);
}
