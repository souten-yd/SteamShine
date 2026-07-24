/**
 * @file tests/unit/test_steamos_virtual_session.cpp
 * @brief Unit tests for the SteamOS owned virtual-session lifecycle.
 */
#if defined(__linux__)
  #include <cerrno>
  #include <chrono>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <gtest/gtest.h>
  #include <iterator>
  #include <signal.h>
  #include <src/config.h>
  #include <src/rtsp.h>
  #include <src/steamos_virtual_session.h>
  #include <string_view>
  #include <thread>
  #include <unistd.h>

namespace {
  /**
   * @brief Check whether a terminated process has not yet been reaped by init.
   *
   * @param process Process identifier to inspect.
   * @return True when Linux reports the process as a zombie.
   */
  bool process_is_zombie(const pid_t process) {
    std::ifstream input {std::filesystem::path {"/proc"} / std::to_string(process) / "stat"};
    std::string status;
    std::getline(input, status);
    const auto command_end {status.rfind(')')};
    return command_end != std::string::npos && command_end + 2 < status.size() && status.at(command_end + 2) == 'Z';
  }

  /**
   * @brief Create a fake Gamescope executable that advertises required options and creates readiness.
   *
   * @param directory Directory used to store the fake executable.
   * @param mode Fake lifecycle behavior to implement.
   * @return Executable path.
   */
  std::filesystem::path make_fake_gamescope(const std::filesystem::path &directory, const std::string_view mode = "normal") {
    const auto executable {directory / "gamescope"};
    std::ofstream output {executable};
    output << "#!/bin/sh\n";
    output << "if [ \"$1\" = \"--help\" ]; then echo '--backend headless --nested-width --nested-height --nested-refresh --expose-wayland --scaler --hdr-enabled --prefer-vk-device'; exit 0; fi\n";
    output << "printf '%s\\n' \"$@\" > \"$XDG_RUNTIME_DIR/gamescope-arguments\"\n";
    if (mode == "crash-before-ready") {
      output << "exit 42\n";
      output.close();
      std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      return executable;
    }
    if (mode == "never-ready") {
      output << "trap 'exit 0' TERM INT\n";
      output << "while :; do sleep 1; done\n";
      output.close();
      std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      return executable;
    }
    if (mode == "invalid-socket") {
      output << "touch \"$XDG_RUNTIME_DIR/gamescope-0\"\n";
      output << "trap 'exit 0' TERM INT\n";
      output << "while :; do sleep 1; done\n";
      output.close();
      std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      return executable;
    }
    if (mode == "delayed-ready") {
      output << "sleep 1\n";
    }
    output << "python3 -c 'import os, socket, signal, sys; p=os.path.join(os.environ[\"XDG_RUNTIME_DIR\"], \"gamescope-0\"); s=socket.socket(socket.AF_UNIX); s.bind(p); s.listen(); signal.signal(signal.SIGTERM, lambda *_: sys.exit(0)); signal.signal(signal.SIGINT, lambda *_: sys.exit(0)); [signal.pause() for _ in iter(int, 1)]' &\n";
    output << "socket_child=$!\n";
    if (mode == "leave-child") {
      output << "sh -c 'trap \"\" TERM INT; while :; do sleep 1; done' &\n";
      output << "ignored_child=$!\n";
      output << "printf '%s\\n' \"$ignored_child\" > \"$XDG_RUNTIME_DIR/ignored-child.pid\"\n";
      output << "trap 'exit 0' TERM INT\n";
      output << "while :; do sleep 1; done\n";
      output.close();
      std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      return executable;
    }
    if (mode == "ignore-term") {
      output << "trap '' TERM INT\n";
      output << "while :; do sleep 1; done\n";
      output.close();
      std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      return executable;
    }
    output << "trap 'kill \"$socket_child\" 2>/dev/null; wait \"$socket_child\" 2>/dev/null; exit 0' TERM INT\n";
    output << "wait \"$socket_child\"\n";
    output.close();
    std::filesystem::permissions(executable, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    return executable;
  }

  /**
   * @brief Test fixture that restores global virtual-display configuration.
   */
  class SteamOSVirtualSessionTest: public ::testing::Test {
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
      config::steamos_virtual_display.game_gpu = "1002:9999";
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
  EXPECT_FALSE(steamos_virtual_session::capture_backend_required());
  rtsp_stream::launch_session_t launch {};
  std::string error;
  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Disabled);
  EXPECT_FALSE(steamos_virtual_session::active());
}

TEST_F(SteamOSVirtualSessionTest, FeatureFlagKeepsWaylandCaptureAvailableBeforeLaunch) {
  EXPECT_TRUE(steamos_virtual_session::capture_backend_required());
}

TEST_F(SteamOSVirtualSessionTest, CleansOnlyMarkedOrphanRuntimeDirectories) {
  const auto base {root / "runtime" / "steamshine"};
  const auto owned {base / "session-orphan"};
  const auto foreign {base / "session-foreign"};
  std::filesystem::create_directories(owned);
  std::filesystem::create_directories(foreign);
  {
    std::ofstream marker {owned / "steamshine-owner"};
    marker << "steamshine-steamos-virtual-session-v1\n";
  }

  steamos_virtual_session::cleanup_orphan_sessions();

  EXPECT_FALSE(std::filesystem::exists(owned));
  EXPECT_TRUE(std::filesystem::exists(foreign));
}

TEST_F(SteamOSVirtualSessionTest, GamescopeArgumentsUseAdvertisedHeadlessBackendAndFitPolicy) {
  std::string error;
  const auto arguments {steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --nested-refresh --expose-wayland --scaler --hdr-enabled --prefer-vk-device", 2560, 1440, 120, true, "1002:744c", error)};
  ASSERT_TRUE(error.empty());
  EXPECT_EQ(arguments, (std::vector<std::string> {"--backend", "headless", "--nested-width", "2560", "--nested-height", "1440", "--nested-refresh", "120", "--expose-wayland", "--scaler", "fit", "--hdr-enabled", "--prefer-vk-device", "1002:744c"}));
}

TEST_F(SteamOSVirtualSessionTest, GamescopeArgumentsRejectMissingHeadlessBackend) {
  std::string error;
  const auto arguments {steamos_virtual_session::gamescope_arguments("--nested-width --nested-height --nested-refresh --expose-wayland", 1920, 1080, 60, false, "", error)};
  EXPECT_TRUE(arguments.empty());
  EXPECT_NE(error.find("headless"), std::string::npos);
}

TEST_F(SteamOSVirtualSessionTest, RejectsEncoderOrCaptureOnDifferentGpu) {
  config::steamos_virtual_display.capture_gpu = "1002:744c";
  config::steamos_virtual_display.encoder_gpu = "1002:7550";
  rtsp_stream::launch_session_t launch {};
  std::string error;
  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("one AMD dGPU"), std::string::npos);
}

TEST_F(SteamOSVirtualSessionTest, FakeGamescopeReadinessAndCleanup) {
  rtsp_stream::launch_session_t launch {};
  launch.id = 42;
  launch.width = 1920;
  launch.height = 1080;
  launch.fps = 60;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::WaitingForCapture);
  EXPECT_TRUE(steamos_virtual_session::active());
  EXPECT_TRUE(std::filesystem::exists(config::steamos_virtual_display.runtime_directory));
  std::string runtime_directory;
  std::string wayland_display;
  EXPECT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display));
  const auto expected_runtime_directory {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-42")};
  EXPECT_EQ(runtime_directory, expected_runtime_directory.string());
  EXPECT_EQ(wayland_display, "gamescope-0");
  std::ifstream arguments_file {std::filesystem::path {runtime_directory} / "gamescope-arguments"};
  const std::string arguments {(std::istreambuf_iterator<char> {arguments_file}), std::istreambuf_iterator<char> {}};
  EXPECT_NE(arguments.find("--backend\nheadless\n"), std::string::npos);
  EXPECT_NE(arguments.find("--nested-width\n1920\n"), std::string::npos);
  EXPECT_NE(arguments.find("--nested-height\n1080\n"), std::string::npos);
  EXPECT_NE(arguments.find("--nested-refresh\n60\n"), std::string::npos);
  EXPECT_NE(arguments.find("--prefer-vk-device\n1002:9999\n"), std::string::npos);
  std::string socket_path;
  EXPECT_TRUE(steamos_virtual_session::capture_socket(socket_path));
  EXPECT_TRUE(std::filesystem::is_socket(socket_path));
  steamos_virtual_session::mark_capture_ready();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Ready);
  steamos_virtual_session::stop();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
  EXPECT_FALSE(steamos_virtual_session::active());
  EXPECT_TRUE(std::filesystem::exists(config::steamos_virtual_display.runtime_directory));
}

TEST_F(SteamOSVirtualSessionTest, CleansUpAfterGamescopeStartupTimeout) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "never-ready").string();
  config::steamos_virtual_display.startup_timeout_seconds = 1;
  rtsp_stream::launch_session_t launch {};
  launch.id = 7;
  std::string error;
  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("Timed out"), std::string::npos);
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);
  const auto session_directory {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-7")};
  EXPECT_FALSE(std::filesystem::exists(session_directory));
}

TEST_F(SteamOSVirtualSessionTest, RecoversForReconnectAfterGamescopeStartupFailure) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "crash-before-ready").string();
  rtsp_stream::launch_session_t launch {};
  launch.id = 13;
  std::string error;
  ASSERT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);

  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root).string();
  error.clear();
  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::WaitingForCapture);
}

TEST_F(SteamOSVirtualSessionTest, RejectsRegularFileInsteadOfWaylandSocket) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "invalid-socket").string();
  config::steamos_virtual_display.startup_timeout_seconds = 1;
  rtsp_stream::launch_session_t launch {};
  launch.id = 10;
  std::string error;
  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("Timed out"), std::string::npos);
}

TEST_F(SteamOSVirtualSessionTest, WaitsForDelayedWaylandSocket) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "delayed-ready").string();
  config::steamos_virtual_display.startup_timeout_seconds = 2;
  rtsp_stream::launch_session_t launch {};
  launch.id = 11;
  std::string error;
  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::WaitingForCapture);
}

TEST_F(SteamOSVirtualSessionTest, CleansUpAfterGamescopeEarlyCrash) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "crash-before-ready").string();
  rtsp_stream::launch_session_t launch {};
  launch.id = 8;
  std::string error;
  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("exited"), std::string::npos);
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);
}

TEST_F(SteamOSVirtualSessionTest, ForcedCleanupKillsOwnedChildAfterGamescopeExits) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "leave-child").string();
  rtsp_stream::launch_session_t launch {};
  launch.id = 9;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  std::string runtime_directory;
  std::string wayland_display;
  ASSERT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display));
  std::ifstream input {std::filesystem::path {runtime_directory} / "ignored-child.pid"};
  pid_t child {};
  input >> child;
  ASSERT_GT(child, 0);
  steamos_virtual_session::stop();
  for (int attempt = 0; attempt < 20 && ::kill(child, 0) == 0 && !process_is_zombie(child); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
  }
  const int kill_result {::kill(child, 0)};
  const int kill_error {errno};
  // The test shell can exit before reaping its ignored child. A zombie has
  // already been killed and cannot execute or retain the virtual session;
  // runner PID 1 owns the eventual reap timing.
  EXPECT_TRUE(kill_result == -1 || process_is_zombie(child));
  if (kill_result == -1) {
    EXPECT_EQ(kill_error, ESRCH);
  }
}

TEST_F(SteamOSVirtualSessionTest, ForcedCleanupStopsGamescopeThatIgnoresTerm) {
  config::steamos_virtual_display.gamescope_path = make_fake_gamescope(root, "ignore-term").string();
  config::steamos_virtual_display.shutdown_timeout_seconds = 1;
  rtsp_stream::launch_session_t launch {};
  launch.id = 12;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  steamos_virtual_session::stop();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-12")));
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
