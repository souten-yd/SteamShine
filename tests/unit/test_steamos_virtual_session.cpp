/**
 * @file tests/unit/test_steamos_virtual_session.cpp
 * @brief Unit tests for the SteamOS owned virtual-session lifecycle.
 */
#if defined(__linux__)
  #include <cerrno>
  #include <chrono>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <gtest/gtest.h>
  #include <iterator>
  #include <signal.h>
  #include <src/config.h>
  #include <src/rtsp.h>
  #include <src/steamos_virtual_session.h>
  #include <string_view>
  #include <sys/socket.h>
  #include <sys/un.h>
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
    output << "printf 'runtime=%s\\nremote=%s\\nsession_type=%s\\n' \"$PIPEWIRE_RUNTIME_DIR\" \"$PIPEWIRE_REMOTE\" \"${XDG_SESSION_TYPE-unset}\" > \"$XDG_RUNTIME_DIR/gamescope-pipewire-environment\"\n";
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
    std::string saved_xdg_runtime_directory;  ///< XDG runtime environment restored after each test.
    bool had_xdg_runtime_directory {false};  ///< Whether XDG runtime was set before test setup.
    std::string saved_xdg_session_type;  ///< Desktop session type restored after each test.
    bool had_xdg_session_type {false};  ///< Whether XDG_SESSION_TYPE was set before test setup.
    std::vector<int> pipewire_sockets;  ///< Test-owned host PipeWire UNIX sockets.

    /**
     * @brief Create a connectable host PipeWire socket below the test runtime.
     */
    void create_pipewire_socket(const std::string_view remote = "pipewire-0") {
      const auto socket_path {root / "runtime" / remote};
      const int pipewire_socket {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      ASSERT_GE(pipewire_socket, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
      ASSERT_EQ(::bind(pipewire_socket, reinterpret_cast<const sockaddr *>(&address), offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1), 0);
      ASSERT_EQ(::listen(pipewire_socket, 1), 0);
      pipewire_sockets.emplace_back(pipewire_socket);
    }

    /**
     * @brief Set up a fake Gamescope and a test-only runtime base.
     */
    void SetUp() override {
      std::filesystem::remove_all(root);
      std::filesystem::create_directories(root / "runtime");
      if (const auto *runtime {std::getenv("XDG_RUNTIME_DIR")}) {
        saved_xdg_runtime_directory = runtime;
        had_xdg_runtime_directory = true;
      }
      if (const auto *session_type {std::getenv("XDG_SESSION_TYPE")}) {
        saved_xdg_session_type = session_type;
        had_xdg_session_type = true;
      }
      ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", (root / "runtime").c_str(), 1), 0);
      create_pipewire_socket();
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
      for (const int pipewire_socket : pipewire_sockets) {
        ::close(pipewire_socket);
      }
      pipewire_sockets.clear();
      config::steamos_virtual_display = saved;
      if (had_xdg_runtime_directory) {
        (void) ::setenv("XDG_RUNTIME_DIR", saved_xdg_runtime_directory.c_str(), 1);
      } else {
        (void) ::unsetenv("XDG_RUNTIME_DIR");
      }
      if (had_xdg_session_type) {
        (void) ::setenv("XDG_SESSION_TYPE", saved_xdg_session_type.c_str(), 1);
      } else {
        (void) ::unsetenv("XDG_SESSION_TYPE");
      }
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

/**
 * @brief Verify the off policy preserves the normal launch path even when enabled.
 */
TEST_F(SteamOSVirtualSessionTest, OffModePreservesNormalLaunch) {
  config::steamos_virtual_display.mode = steamos_virtual_session::virtual_display_mode_e::off;
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Disabled);
  EXPECT_FALSE(steamos_virtual_session::capture_backend_required());
}

/**
 * @brief Verify an explicit resident-Gamescope policy fails closed without spawning one.
 */
TEST_F(SteamOSVirtualSessionTest, ExistingGamescopePolicyDoesNotCreateOwnedFallback) {
  config::steamos_virtual_display.session_source = steamos_virtual_session::session_source_policy_e::existing_gamescope;
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("existing Gamescope"), std::string::npos);
  const auto session_directory {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-0")};
  EXPECT_FALSE(std::filesystem::exists(session_directory));
}

TEST_F(SteamOSVirtualSessionTest, FeatureFlagKeepsWaylandCaptureAvailableBeforeLaunch) {
  EXPECT_TRUE(steamos_virtual_session::capture_backend_required());
}

TEST_F(SteamOSVirtualSessionTest, RejectsRuntimeDirectoryOutsideUserRuntime) {
  config::steamos_virtual_display.runtime_directory = "/tmp/steamshine-foreign-runtime";
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("XDG_RUNTIME_DIR"), std::string::npos);
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);
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

TEST_F(SteamOSVirtualSessionTest, DoesNotCleanOrphansOutsideUserRuntime) {
  const auto external_base {root / "external-runtime"};
  const auto external_owned {external_base / "session-orphan"};
  std::filesystem::create_directories(external_owned);
  {
    std::ofstream marker {external_owned / "steamshine-owner"};
    marker << "steamshine-steamos-virtual-session-v1\n";
  }
  config::steamos_virtual_display.runtime_directory = external_base.string();

  steamos_virtual_session::cleanup_orphan_sessions();

  EXPECT_TRUE(std::filesystem::exists(external_owned));
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

/**
 * @brief Verify whitespace-only configuration cannot disable automatic GPU selection.
 */
TEST_F(SteamOSVirtualSessionTest, TrimsGpuSelectorWhitespace) {
  config::steamos_virtual_display.game_gpu = "  1002:9999\t";
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_TRUE(steamos_virtual_session::prepare(launch, error));
  EXPECT_TRUE(error.empty());
}

/**
 * @brief Verify owned Gamescope retains the host PipeWire endpoint.
 */
TEST_F(SteamOSVirtualSessionTest, SeparatesPrivateWaylandAndHostPipeWireRuntimes) {
  const auto host_runtime {root / "runtime"};
  config::steamos_virtual_display.pipewire_runtime = host_runtime.string();
  config::steamos_virtual_display.pipewire_remote = "pipewire-test";
  create_pipewire_socket("pipewire-test");
  rtsp_stream::launch_session_t launch {};
  launch.id = 77;
  launch.width = 1920;
  launch.height = 1080;
  launch.fps = 60;
  std::string error;

  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  const auto session_runtime {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-77")};
  std::ifstream environment {session_runtime / "gamescope-pipewire-environment"};
  const std::string contents {(std::istreambuf_iterator<char> {environment}), std::istreambuf_iterator<char> {}};
  EXPECT_NE(contents.find("runtime=" + host_runtime.string()), std::string::npos);
  EXPECT_NE(contents.find("remote=pipewire-test"), std::string::npos);
  std::string runtime_directory;
  std::string wayland_display;
  std::string application_pipewire_runtime;
  std::string application_pipewire_remote;
  std::string application_pulse_runtime;
  ASSERT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display, application_pipewire_runtime, application_pipewire_remote, application_pulse_runtime));
  EXPECT_EQ(application_pipewire_runtime, host_runtime.string());
  EXPECT_EQ(application_pipewire_remote, "pipewire-test");
  EXPECT_EQ(application_pulse_runtime, (host_runtime / "pulse").string());
  int gamescope_pid {};
  ASSERT_TRUE(steamos_virtual_session::gamescope_pipewire_endpoint(application_pipewire_runtime, application_pipewire_remote, gamescope_pid));
  EXPECT_EQ(application_pipewire_runtime, host_runtime.string());
  EXPECT_EQ(application_pipewire_remote, "pipewire-test");
  EXPECT_GT(gamescope_pid, 0);
  const auto snapshot {steamos_virtual_session::status_snapshot()};
  EXPECT_EQ(snapshot.origin, steamos_virtual_session::session_origin_e::owned_private);
  EXPECT_TRUE(snapshot.process_owned);
  EXPECT_TRUE(snapshot.runtime_owned);
  EXPECT_EQ(snapshot.source_description, "SteamShine-owned private Gamescope");
  EXPECT_GT(snapshot.source_process_start_time, 0U);
  EXPECT_EQ(snapshot.width, 1920);
  EXPECT_EQ(snapshot.height, 1080);
  EXPECT_EQ(snapshot.fps, 60);
  steamos_virtual_session::mark_gamescope_pipewire_node(123, 456, gamescope_pid);
  const auto node_snapshot {steamos_virtual_session::status_snapshot()};
  EXPECT_EQ(node_snapshot.pipewire_node_id, 123U);
  EXPECT_EQ(node_snapshot.pipewire_object_serial, 456U);
  EXPECT_EQ(node_snapshot.pipewire_producer_pid, gamescope_pid);
}

/**
 * @brief Verify an unset optional PipeWire runtime retains the original login runtime.
 */
TEST_F(SteamOSVirtualSessionTest, UnsetPipeWireRuntimeUsesOriginalXdgRuntime) {
  config::steamos_virtual_display.pipewire_runtime.clear();
  rtsp_stream::launch_session_t launch {};
  std::string error;

  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  const auto snapshot {steamos_virtual_session::status_snapshot()};
  EXPECT_EQ(snapshot.pipewire_runtime, (root / "runtime").string());
  EXPECT_EQ(snapshot.pipewire_remote, "pipewire-0");
}

/**
 * @brief Verify a PipeWire capture completion contributes to session metrics.
 */
TEST_F(SteamOSVirtualSessionTest, TracksCapturedPipeWireFrameAfterStreamingStarts) {
  rtsp_stream::launch_session_t launch {};
  std::string error;

  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  steamos_virtual_session::mark_streaming();
  steamos_virtual_session::mark_captured_frame();
  EXPECT_EQ(steamos_virtual_session::status_snapshot().captured_frames, 1U);
}

/**
 * @brief Verify a disconnect retains the owned display for a later resume.
 */
TEST_F(SteamOSVirtualSessionTest, RetainsVirtualDisplayAcrossStreamDisconnect) {
  rtsp_stream::launch_session_t launch {};
  std::string error;

  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  steamos_virtual_session::mark_capture_ready();
  steamos_virtual_session::mark_streaming();
  steamos_virtual_session::mark_captured_frame();
  steamos_virtual_session::mark_streaming_disconnected();

  const auto disconnected {steamos_virtual_session::status_snapshot()};
  EXPECT_EQ(disconnected.state, steamos_virtual_session::state_e::Ready);
  EXPECT_EQ(disconnected.captured_frames, 1U);
  EXPECT_TRUE(steamos_virtual_session::active());

  steamos_virtual_session::mark_streaming();
  steamos_virtual_session::mark_captured_frame();
  EXPECT_EQ(steamos_virtual_session::status_snapshot().captured_frames, 2U);
}

/**
 * @brief Verify an external host PipeWire runtime is rejected before Gamescope starts.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsHostPipeWireRuntimeOutsideLoginRuntime) {
  config::steamos_virtual_display.pipewire_runtime = "/tmp/steamshine-pipewire";
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("PipeWire runtime"), std::string::npos);
}

/**
 * @brief Verify the PipeWire parent runtime cannot be confused with private Wayland state.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsHostPipeWireRuntimeInsidePrivateRuntime) {
  std::filesystem::create_directories(config::steamos_virtual_display.runtime_directory);
  config::steamos_virtual_display.pipewire_runtime = config::steamos_virtual_display.runtime_directory;
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_EQ(error, "Host PipeWire runtime must not be inside the private Wayland runtime");
}

/**
 * @brief Verify a configured host runtime cannot escape through a symbolic link.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsSymbolicLinkHostPipeWireRuntime) {
  const auto runtime_link {root / "runtime-link"};
  std::error_code filesystem_error;
  std::filesystem::create_directory_symlink(root / "runtime", runtime_link, filesystem_error);
  ASSERT_FALSE(filesystem_error) << filesystem_error.message();
  config::steamos_virtual_display.pipewire_runtime = runtime_link.string();
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_EQ(error, "Host PipeWire runtime must not be a symbolic link");
}

/**
 * @brief Verify a regular file cannot impersonate the configured PipeWire socket.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsRegularFileInsteadOfPipeWireSocket) {
  const auto fake_socket {root / "runtime" / "not-a-socket"};
  std::ofstream output {fake_socket};
  output << "not a socket";
  output.close();
  config::steamos_virtual_display.pipewire_remote = fake_socket.filename().string();
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_EQ(error, "Host PipeWire path is not a UNIX socket");
}

/**
 * @brief Verify PipeWire remotes cannot escape the selected host runtime.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsPipeWireRemotePath) {
  config::steamos_virtual_display.pipewire_remote = "../pipewire-0";
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_NE(error.find("PipeWire remote"), std::string::npos);
}

/**
 * @brief Verify an invalid DRM node remains a specific, fail-closed GPU error.
 */
TEST_F(SteamOSVirtualSessionTest, RejectsNonexistentExplicitRenderNode) {
  config::steamos_virtual_display.game_gpu = "/dev/dri/renderD999";
  rtsp_stream::launch_session_t launch {};
  std::string error;

  EXPECT_FALSE(steamos_virtual_session::prepare(launch, error));
  EXPECT_EQ(error, "Configured SteamOS game GPU is not an accessible AMD DRM render node");
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
  std::string pipewire_runtime;
  std::string pipewire_remote;
  std::string pulse_runtime;
  EXPECT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display, pipewire_runtime, pipewire_remote, pulse_runtime));
  const auto expected_runtime_directory {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-42")};
  EXPECT_EQ(runtime_directory, expected_runtime_directory.string());
  EXPECT_EQ(wayland_display, "gamescope-0");
  const auto snapshot {steamos_virtual_session::status_snapshot()};
  EXPECT_EQ(snapshot.state, steamos_virtual_session::state_e::WaitingForCapture);
  EXPECT_EQ(snapshot.socket_path, (std::filesystem::path {runtime_directory} / "gamescope-0").string());
  EXPECT_GT(snapshot.gamescope_pid, 0);
  std::ifstream pid_file {std::filesystem::path {runtime_directory} / "gamescope.pid"};
  pid_t gamescope_pid {};
  pid_file >> gamescope_pid;
  EXPECT_GT(gamescope_pid, 0);
  EXPECT_EQ(::getpgid(gamescope_pid), gamescope_pid);
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
  EXPECT_TRUE(steamos_virtual_session::capture_socket_required());
  std::filesystem::remove(socket_path);
  EXPECT_FALSE(steamos_virtual_session::capture_socket(socket_path));
  EXPECT_TRUE(steamos_virtual_session::capture_socket_required());
  steamos_virtual_session::mark_capture_ready();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Ready);
  steamos_virtual_session::stop();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
  EXPECT_FALSE(steamos_virtual_session::active());
  EXPECT_TRUE(std::filesystem::exists(config::steamos_virtual_display.runtime_directory));
}

TEST_F(SteamOSVirtualSessionTest, CaptureLossRetainsOwnershipForSafeCleanup) {
  rtsp_stream::launch_session_t launch {};
  launch.id = 43;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;
  steamos_virtual_session::mark_capture_ready();

  steamos_virtual_session::mark_capture_lost();

  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);
  EXPECT_TRUE(steamos_virtual_session::active());
  steamos_virtual_session::mark_capture_lost();
  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);

  steamos_virtual_session::stop();

  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
  EXPECT_FALSE(steamos_virtual_session::active());
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-43")));
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

/**
 * @brief Verify the owned compositor does not inherit the desktop session type.
 */
TEST_F(SteamOSVirtualSessionTest, DoesNotInheritDesktopSessionTypeIntoGamescope) {
  ASSERT_EQ(::setenv("XDG_SESSION_TYPE", "wayland", 1), 0);
  rtsp_stream::launch_session_t launch {};
  launch.id = 13;
  std::string error;
  ASSERT_TRUE(steamos_virtual_session::prepare(launch, error)) << error;

  const auto session_runtime {std::filesystem::path {config::steamos_virtual_display.runtime_directory} / ("session-" + std::to_string(::getpid()) + "-13")};
  std::ifstream environment {session_runtime / "gamescope-pipewire-environment"};
  const std::string contents {(std::istreambuf_iterator<char> {environment}), std::istreambuf_iterator<char> {}};
  EXPECT_NE(contents.find("session_type=unset"), std::string::npos);
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
  std::string pipewire_runtime;
  std::string pipewire_remote;
  std::string pulse_runtime;
  ASSERT_TRUE(steamos_virtual_session::application_environment(runtime_directory, wayland_display, pipewire_runtime, pipewire_remote, pulse_runtime));
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
