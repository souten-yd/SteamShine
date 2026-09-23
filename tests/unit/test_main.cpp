/**
 * @file tests/unit/test_main.cpp
 * @brief Tests for main-loop lifetime decisions.
 */

#include "src/main.h"

#include <gtest/gtest.h>

/**
 * @brief Keep the resident server alive when a desktop event loop disappears.
 */
TEST(MainLoop, WaitsAfterUnexpectedPlatformLoopExit) {
  EXPECT_TRUE(main_loop_requires_shutdown_wait(false));
}

/**
 * @brief Permit normal termination after an explicit shutdown request.
 */
TEST(MainLoop, ExitsAfterRequestedShutdown) {
  EXPECT_FALSE(main_loop_requires_shutdown_wait(true));
}

/**
 * @brief Allow a requested tray when compositor transitions are disabled.
 */
TEST(MainLoop, UsesTrayWithoutSteamOsTransitions) {
  EXPECT_TRUE(main_loop_uses_system_tray(true, false));
}

/**
 * @brief Omit the tray from a transition-capable resident server.
 */
TEST(MainLoop, OmitsTrayWithSteamOsTransitions) {
  EXPECT_FALSE(main_loop_uses_system_tray(true, true));
  EXPECT_FALSE(main_loop_uses_system_tray(false, false));
  EXPECT_FALSE(main_loop_uses_system_tray(false, true));
}

/**
 * @brief Retain the historical watchdog when no SteamOS transition is active.
 */
TEST(MainLoop, KeepsBaselineShutdownWatchdogWithoutTransitions) {
  EXPECT_EQ(shutdown_watchdog_timeout(false, 60, 60), std::chrono::seconds {10});
}

/**
 * @brief Allow both virtual stop and stock restoration before forced exit.
 */
TEST(MainLoop, BudgetsShutdownWatchdogForStockRecovery) {
  EXPECT_EQ(shutdown_watchdog_timeout(true, 5, 15), std::chrono::seconds {30});
  EXPECT_EQ(shutdown_watchdog_timeout(true, 60, 60), std::chrono::seconds {130});
  EXPECT_EQ(shutdown_watchdog_timeout(true, -1, -1), std::chrono::seconds {10});
}

/**
 * @brief Force Gamescope only after real platform initialization failure.
 */
TEST(MainLoop, ForcesVirtualPreflightAfterPhysicalPlatformFailure) {
  EXPECT_TRUE(should_force_virtual_encoder_preflight(false, true, true));
  EXPECT_FALSE(should_force_virtual_encoder_preflight(true, true, true));
  EXPECT_FALSE(should_force_virtual_encoder_preflight(false, false, true));
  EXPECT_FALSE(should_force_virtual_encoder_preflight(false, true, false));
}

/**
 * @brief Verify configured SteamOS transitions always use a prepared endpoint at startup.
 */
TEST(MainLoop, PreparesVirtualEndpointBeforeEncoderProbe) {
  EXPECT_TRUE(should_prepare_virtual_encoder_preflight(false, true, true));
  EXPECT_TRUE(should_prepare_virtual_encoder_preflight(true, false, false));
  EXPECT_FALSE(should_prepare_virtual_encoder_preflight(false, false, true));
  EXPECT_FALSE(should_prepare_virtual_encoder_preflight(false, true, false));
}
