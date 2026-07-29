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
