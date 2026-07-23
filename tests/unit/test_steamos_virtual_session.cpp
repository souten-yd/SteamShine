/**
 * @file tests/unit/test_steamos_virtual_session.cpp
 * @brief Unit tests for the SteamOS owned virtual-session lifecycle.
 */
#include "../tests_common.h"

#if defined(__linux__)
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <src/config.h>
  #include <src/rtsp.h>
  #include <src/steamos_virtual_session.h>

namespace {
  /**
   * @brief Create a fake Gamescope executable that advertises required options and creates readiness.
   *
   * @param directory Directory used to store the fake executable.
   * @return Executable path.
   */
  std::filesystem::path make_fake_gamescope(const std::filesystem::path &directory) {
    const auto executable {directory / "gamescope"};
    std::ofstream output {executable};
    output << "#!/bin/sh\n";
    output << "if [ \"$1\" = \"--help\" ]; then echo '--headless --nested-width --nested-height --nested-refresh'; exit 0; fi\n";
    output << "touch \"$XDG_RUNTIME_DIR/wayland-0\"\n";
    output << "trap 'exit 0' TERM INT\n";
    output << "while :; do sleep 1; done\n";
    output.close();
    std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    return executable;
  }

  /**
   * @brief Test fixture that restores global virtual-display configuration.
   */
  class SteamOSVirtualSessionTest: public BaseTest {
  protected:
    config::steamos_virtual_display_t saved {config::steamos_virtual_display};  ///< Configuration restored after each test.
    std::filesystem::path root {std::filesystem::temp_directory_path() / "steamshine-virtual-session-test"};  ///< Test-owned temporary directory.

    /**
     * @brief Set up a fake Gamescope and a test-only runtime base.
     */
    void SetUp() override {
      std::filesystem::remove_all(root);
      std::filesystem::create_directories(root / "runtime");
      config::steamos_virtual_display.enabled = true;
      config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root).string();
      config::steamos_virtual_display.runtime_directory = (root / "runtime" / "steamshine").string();
      config::steamos_virtual_display.startup_timeout_seconds = 2;
      config::steamos_virtual_display.shutdown_timeout_seconds = 1;
    }

    /**
     * @brief Stop owned children and restore global configuration.
     */
    void TearDown() override {
      steamos_virtual_session::stop();
      config::steamos_virtual_display = saved;
      std::filesystem::remove_all(root);
    }
  };
}  // namespace

TEST_F(SteamOSVirtualSessionTest, FeatureFlagDisabledPreservesNormalLaunch) {
  config::steamos_virtual_display.enabled = false;
  rtsp_stream::launch_session_t launch {};
  std::string error;
  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Disabled);
}

TEST_F(SteamOSVirtualSessionTest, FakeGamescopeReadinessAndCleanup) {
  rtsp_stream::launch_session_t launch {};
  launch.id = 42;
  launch.width = 1920;
  launch.height = 1080;
  launch.fps = 60;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Ready);
  EXPECT_TRUE(std::filesystem::exists(config::steamos_virtual_display.runtime_directory));
  std::string runtime_directory;
  std::string wayland_display;
  EXPECT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display));
  const auto expected_runtime_directory {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-42")};
  EXPECT_EQ(runtime_directory, expected_runtime_directory.string());
  EXPECT_EQ(wayland_display, "wayland-0");
  steamos_virtual_session::stop();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
  EXPECT_TRUE(std::filesystem::exists(config::steamos_virtual_display.runtime_directory));
}

TEST_F(SteamOSVirtualSessionTest, RejectsDuplicateOwnedSession) {
  rtsp_stream::launch_session_t launch {};
  launch.id = 1;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("already active"), std::string::npos);
}
#endif
