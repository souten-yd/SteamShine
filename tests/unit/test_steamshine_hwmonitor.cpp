/**
 * @file tests/unit/test_steamshine_hwmonitor.cpp
 * @brief Tests for SteamShine Monitor telemetry presentation policy.
 */

#include "../tests_common.h"

#include <limits>
#include <src/config.h>
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
