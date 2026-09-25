/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test imports
#include "../tests_common.h"

// standard imports
#include <filesystem>
#include <fstream>

// local imports
#include <src/process.h>

#if defined(__linux__)
  #include <src/platform/linux/gamescope_wsi_socket.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

#if defined(__linux__)
/**
 * @brief Own two UNIX sockets for testing Gamescope and nested-compositor identities.
 */
class GamescopeWsiSocketTest: public testing::Test {
protected:
  fs::path root;  ///< Isolated runtime directory owned by the fixture.
  std::array<int, 2> sockets {-1, -1};  ///< Socket descriptors retained until teardown.

  /**

   * @brief Create distinct Gamescope and nested-compositor socket endpoints.

   */
  void SetUp() override {
    char directory[] = "/tmp/steamshine-wsi-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    root = directory;
    for (size_t index = 0; index < sockets.size(); ++index) {
      sockets[index] = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT_GE(sockets[index], 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto path {(root / (index == 0 ? "gamescope-0" : "nested-0")).string()};
      ASSERT_LT(path.size(), sizeof(address.sun_path));
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      ASSERT_EQ(bind(sockets[index], reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
    }
    fs::create_symlink(root / "gamescope-0", root / "wayland-0");
    std::ofstream(root / "regular-file") << "not a socket";
  }

  /**

   * @brief Close descriptors and remove only this fixture's runtime directory.

   */
  void TearDown() override {
    for (const int descriptor : sockets) {
      if (descriptor >= 0) {
        close(descriptor);
      }
    }
    if (!root.empty()) {
      std::error_code error;
      fs::remove_all(root, error);
    }
  }
};

/**

 * @brief Preserve established unset, empty, and identical socket-name behavior.

 */
TEST_F(GamescopeWsiSocketTest, PreservesExistingNameRules) {
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session(nullptr, nullptr, nullptr, false));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("", nullptr, nullptr, false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", nullptr, nullptr, false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", "", nullptr, false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", "gamescope-0", nullptr, false));
}

/**

 * @brief Accept relative and absolute aliases that identify the same live socket.

 */
TEST_F(GamescopeWsiSocketTest, AcceptsAliasesOfSameSocket) {
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", "wayland-0", root.c_str(), false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session((root / "gamescope-0").c_str(), "wayland-0", root.c_str(), false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session((root / "gamescope-0").c_str(), (root / "wayland-0").c_str(), nullptr, false));
}

/**

 * @brief Never mistake another live compositor or inherited connection for Gamescope.

 */
TEST_F(GamescopeWsiSocketTest, RejectsDifferentCompositorAndInheritedConnection) {
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "nested-0", root.c_str(), false));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "wayland-0", root.c_str(), true));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "regular-file", root.c_str(), false));
}

/**

 * @brief Require a runtime directory for relative names and a valid Gamescope socket.

 */
TEST_F(GamescopeWsiSocketTest, RejectsUnverifiableGamescopeEndpoint) {
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "wayland-0", nullptr, false));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "wayland-0", "", false));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("absent", "wayland-0", root.c_str(), false));
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("regular-file", "wayland-0", root.c_str(), false));
}

/**

 * @brief Accept pressure-vessel's missing display alias only with a live Gamescope socket.

 */
TEST_F(GamescopeWsiSocketTest, AcceptsMissingWaylandAliasButRejectsOtherErrors) {
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", "absent", root.c_str(), false));
  EXPECT_TRUE(gamescope_wsi::is_gamescope_session("gamescope-0", "regular-file/absent", root.c_str(), false));
  fs::create_symlink("loop", root / "loop");
  EXPECT_FALSE(gamescope_wsi::is_gamescope_session("gamescope-0", "loop", root.c_str(), false));
}
#endif

TEST(ProcessCommandSelectionTest, KeepsConfiguredApplicationCommand) {
  EXPECT_EQ(proc::select_effective_command("game --launch", true, "virtual-desktop"), "game --launch");
}

TEST(ProcessCommandSelectionTest, KeepsPhysicalDesktopAsCaptureOnly) {
  EXPECT_TRUE(proc::select_effective_command("", false, "virtual-desktop").empty());
}

TEST(ProcessCommandSelectionTest, LaunchesConfiguredOwnedVirtualDesktop) {
  EXPECT_EQ(proc::select_effective_command("", true, "virtual-desktop"), "virtual-desktop");
}

/**
 * @brief Verify only the explicit automatic physical-display protocol requests fallback.
 */
TEST(ProcessCommandSelectionTest, RecognizesOwnedVirtualDisplayFallbackProtocol) {
  using steamos_virtual_session::session_origin_e;
  using steamos_virtual_session::virtual_display_mode_e;

  EXPECT_TRUE(proc::should_retry_owned_virtual_display(75, true, virtual_display_mode_e::auto_detect, session_origin_e::none));
  EXPECT_FALSE(proc::should_retry_owned_virtual_display(1, true, virtual_display_mode_e::auto_detect, session_origin_e::none));
  EXPECT_FALSE(proc::should_retry_owned_virtual_display(75, false, virtual_display_mode_e::auto_detect, session_origin_e::none));
  EXPECT_FALSE(proc::should_retry_owned_virtual_display(75, true, virtual_display_mode_e::off, session_origin_e::none));
  EXPECT_FALSE(proc::should_retry_owned_virtual_display(75, true, virtual_display_mode_e::auto_detect, session_origin_e::attached_existing));
}

/**
 * @brief Verify a commandless physical Desktop ignores only the explicit mode fallback result.
 */
TEST(ProcessCommandSelectionTest, KeepsPhysicalDesktopWhenModePreparationIsUnavailable) {
  using steamos_virtual_session::session_origin_e;

  EXPECT_TRUE(proc::should_keep_physical_desktop(75, true, session_origin_e::none));
  EXPECT_FALSE(proc::should_keep_physical_desktop(1, true, session_origin_e::none));
  EXPECT_FALSE(proc::should_keep_physical_desktop(75, false, session_origin_e::none));
  EXPECT_FALSE(proc::should_keep_physical_desktop(75, true, session_origin_e::owned_private));
  EXPECT_FALSE(proc::should_keep_physical_desktop(75, true, session_origin_e::attached_existing));
}

/**
 * @brief Verify the packaged KDE surface uses Gamescope's cursor-capable XWayland path.
 */
TEST(ProcessCommandSelectionTest, ConstrainsPackagedVirtualDesktopToXwayland) {
  EXPECT_EQ(
    proc::select_effective_command("", true, "plasmawindowed org.kde.plasma.folder"),
    "env QT_QPA_PLATFORM=xcb plasmawindowed org.kde.plasma.folder"
  );
}

TEST(ProcessCommandSelectionTest, RestoresSafeOwnedVirtualDesktopDefault) {
  EXPECT_EQ(proc::select_effective_command("", true, ""), "env QT_QPA_PLATFORM=xcb plasmawindowed org.kde.plasma.folder");
}

/**
 * @brief Verify virtual-session teardown suppresses only the Big Picture close undo.
 */
TEST(ProcessCommandSelectionTest, SkipsOnlyVirtualSessionBigPictureCloseUndo) {
  EXPECT_TRUE(proc::should_skip_undo_command(true, "setsid steam steam://close/bigpicture"));
  EXPECT_FALSE(proc::should_skip_undo_command(false, "setsid steam steam://close/bigpicture"));
  EXPECT_FALSE(proc::should_skip_undo_command(true, "setsid steam steam://open/bigpicture"));
  EXPECT_FALSE(proc::should_skip_undo_command(true, "steam -shutdown"));
}

/**
 * @brief Verify physical Desktop launches preserve their inherited display environment.
 */
TEST(ProcessDisplayEnvironmentTest, MissingEndpointPreservesPhysicalDesktopEnvironment) {
  boost::process::v1::environment environment;
  environment["DISPLAY"] = ":0";
  environment["WAYLAND_DISPLAY"] = "wayland-0";
  environment["ENABLE_GAMESCOPE_WSI"] = "0";
  environment["DISABLE_GAMESCOPE_WSI"] = "1";
  environment["DXVK_HDR"] = "0";

  proc::apply_session_display_environment(environment, std::nullopt, true);

  EXPECT_EQ(environment["DISPLAY"].to_string(), ":0");
  EXPECT_EQ(environment["WAYLAND_DISPLAY"].to_string(), "wayland-0");
  EXPECT_TRUE(environment.find("GAMESCOPE_WAYLAND_DISPLAY") == environment.end());
  EXPECT_EQ(environment["ENABLE_GAMESCOPE_WSI"].to_string(), "0");
  EXPECT_EQ(environment["DISABLE_GAMESCOPE_WSI"].to_string(), "1");
  EXPECT_EQ(environment["DXVK_HDR"].to_string(), "0");
}

/**
 * @brief Verify owned and attached Gamescope applications receive WSI and session HDR settings.
 */
TEST(ProcessDisplayEnvironmentTest, EnablesGamescopeWsiForHdrAndSdrApplications) {
  using steamos_virtual_session::session_origin_e;
  for (const auto origin : {session_origin_e::owned_private, session_origin_e::attached_existing}) {
    for (const bool enable_hdr : {true, false}) {
      SCOPED_TRACE(enable_hdr);
      boost::process::v1::environment environment;
      environment["ENABLE_GAMESCOPE_WSI"] = "0";
      environment["DISABLE_GAMESCOPE_WSI"] = "1";
      environment["DXVK_HDR"] = enable_hdr ? "0" : "1";
      const steamos_virtual_session::session_display_endpoint_t endpoint {
        .origin = origin,
        .gamescope_wayland_display = "gamescope-0",
        .verification = steamos_virtual_session::display_verification_e::verified,
      };

      proc::apply_session_display_environment(environment, endpoint, enable_hdr);

      EXPECT_EQ(environment["ENABLE_GAMESCOPE_WSI"].to_string(), "1");
      EXPECT_TRUE(environment.find("DISABLE_GAMESCOPE_WSI") == environment.end());
      EXPECT_EQ(environment["DXVK_HDR"].to_string(), enable_hdr ? "1" : "0");
    }
  }
}

/**
 * @brief Verify HDR settings are not invented without a verified Gamescope socket.
 */
TEST(ProcessDisplayEnvironmentTest, MissingGamescopeSocketPreservesHdrEnvironment) {
  boost::process::v1::environment environment;
  environment["ENABLE_GAMESCOPE_WSI"] = "0";
  environment["DISABLE_GAMESCOPE_WSI"] = "1";
  environment["DXVK_HDR"] = "0";
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .verification = steamos_virtual_session::display_verification_e::verified,
  };

  proc::apply_session_display_environment(environment, endpoint, true);

  EXPECT_EQ(environment["ENABLE_GAMESCOPE_WSI"].to_string(), "0");
  EXPECT_EQ(environment["DISABLE_GAMESCOPE_WSI"].to_string(), "1");
  EXPECT_EQ(environment["DXVK_HDR"].to_string(), "0");
}

/**
 * @brief Verify the bundled WSI layer preserves Wayland and existing loader additions.
 */
TEST(ProcessDisplayEnvironmentTest, PrependsBundledWsiWithoutChangingDisplayChoice) {
  for (const bool enable_hdr : {true, false}) {
    for (const std::string_view previous : {"", "/custom/layers:/another/layers"}) {
      SCOPED_TRACE(previous);
      boost::process::v1::environment environment;
      environment["VK_ADD_IMPLICIT_LAYER_PATH"] = std::string {previous};
      const steamos_virtual_session::session_display_endpoint_t endpoint {
        .wayland_display = "gamescope-0",
        .gamescope_wayland_display = "gamescope-0",
        .verification = steamos_virtual_session::display_verification_e::verified,
      };
      proc::apply_session_display_environment(environment, endpoint, enable_hdr, "/artifact/layers");
      EXPECT_EQ(environment["VK_ADD_IMPLICIT_LAYER_PATH"].to_string(), previous.empty() ? "/artifact/layers" : "/artifact/layers:" + std::string {previous});
      EXPECT_EQ(environment["WAYLAND_DISPLAY"].to_string(), "gamescope-0");
      EXPECT_EQ(environment["DXVK_HDR"].to_string(), enable_hdr ? "1" : "0");
    }
  }
}

/**
 * @brief Verify a bundled manifest directory can initialize an absent loader search path.
 */
TEST(ProcessDisplayEnvironmentTest, AddsBundledWsiToEmptyEnvironment) {
  boost::process::v1::environment environment;
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .gamescope_wayland_display = "gamescope-0",
    .verification = steamos_virtual_session::display_verification_e::verified,
  };
  proc::apply_session_display_environment(environment, endpoint, true, "/artifact/layers");
  EXPECT_EQ(environment["VK_ADD_IMPLICIT_LAYER_PATH"].to_string(), "/artifact/layers");
}

/**
 * @brief Verify bundled layers do not affect physical, rejected, or socket-less endpoints.
 */
TEST(ProcessDisplayEnvironmentTest, BundledWsiRequiresVerifiedGamescopeSocket) {
  boost::process::v1::environment environment;
  environment["VK_ADD_IMPLICIT_LAYER_PATH"] = "/custom/layers";
  proc::apply_session_display_environment(environment, std::nullopt, true, "/artifact/layers");
  EXPECT_EQ(environment["VK_ADD_IMPLICIT_LAYER_PATH"].to_string(), "/custom/layers");
  for (const auto verification : {steamos_virtual_session::display_verification_e::unavailable, steamos_virtual_session::display_verification_e::rejected, steamos_virtual_session::display_verification_e::verified}) {
    const steamos_virtual_session::session_display_endpoint_t endpoint {
      .verification = verification,
    };
    proc::apply_session_display_environment(environment, endpoint, true, "/artifact/layers");
    EXPECT_EQ(environment["VK_ADD_IMPLICIT_LAYER_PATH"].to_string(), "/custom/layers");
  }
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .gamescope_wayland_display = "gamescope-0",
    .verification = steamos_virtual_session::display_verification_e::verified,
  };
  proc::apply_session_display_environment(environment, endpoint, true);
  EXPECT_EQ(environment["VK_ADD_IMPLICIT_LAYER_PATH"].to_string(), "/custom/layers");
}

/**
 * @brief Verify every allow-listed Gamescope endpoint variable reaches an application.
 */
TEST(ProcessDisplayEnvironmentTest, AppliesVerifiedDynamicGamescopeEndpoint) {
  for (const std::string_view display : {":1", ":2", ":27"}) {
    boost::process::v1::environment environment;
    const steamos_virtual_session::session_display_endpoint_t endpoint {
      .origin = steamos_virtual_session::session_origin_e::owned_private,
      .xdg_runtime_directory = "/run/user/1000/steamshine/session-7",
      .wayland_display = "gamescope-0",
      .gamescope_wayland_display = "gamescope-0",
      .x11_display = std::string {display},
      .xauthority = "/run/user/1000/steamshine/session-7/xauthority",
      .pipewire_runtime_directory = "/run/user/1000",
      .pipewire_remote = "pipewire-0",
      .pulse_runtime_path = "/run/user/1000/pulse",
      .dbus_session_bus_address = "unix:path=/run/user/1000/bus",
      .xdg_session_type = "wayland",
      .xdg_current_desktop = "gamescope",
      .verification = steamos_virtual_session::display_verification_e::verified,
    };

    proc::apply_session_display_environment(environment, endpoint, true);

    EXPECT_EQ(environment["DISPLAY"].to_string(), display);
    EXPECT_EQ(environment["XAUTHORITY"].to_string(), endpoint.xauthority);
    EXPECT_EQ(environment["GAMESCOPE_WAYLAND_DISPLAY"].to_string(), "gamescope-0");
    EXPECT_EQ(environment["DBUS_SESSION_BUS_ADDRESS"].to_string(), endpoint.dbus_session_bus_address);
    EXPECT_EQ(environment["XDG_SESSION_TYPE"].to_string(), "wayland");
    EXPECT_EQ(environment["XDG_CURRENT_DESKTOP"].to_string(), "gamescope");
    EXPECT_EQ(environment["ENABLE_GAMESCOPE_WSI"].to_string(), "1");
    EXPECT_EQ(environment["DXVK_HDR"].to_string(), "1");
  }
}

/**
 * @brief Verify stock SteamOS auth-less Xwayland launches do not inherit stale desktop credentials.
 */
TEST(ProcessDisplayEnvironmentTest, RemovesXauthorityForVerifiedAuthlessGamescopeEndpoint) {
  boost::process::v1::environment environment;
  environment["XAUTHORITY"] = "/run/user/1000/desktop-xauthority";
  environment["WAYLAND_DISPLAY"] = "wayland-0";
  environment["XDG_SESSION_TYPE"] = "wayland";
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .origin = steamos_virtual_session::session_origin_e::attached_existing,
    .xdg_runtime_directory = "/run/user/1000",
    .wayland_display = "",
    .gamescope_wayland_display = "gamescope-0",
    .x11_display = ":0",
    .pipewire_runtime_directory = "/run/user/1000",
    .pipewire_remote = "pipewire-0",
    .pulse_runtime_path = "/run/user/1000/pulse",
    .xdg_session_type = "x11",
    .xdg_current_desktop = "gamescope",
    .verification = steamos_virtual_session::display_verification_e::verified,
  };

  proc::apply_session_display_environment(environment, endpoint, true);

  EXPECT_TRUE(environment.find("XAUTHORITY") == environment.end());
  EXPECT_TRUE(environment.find("WAYLAND_DISPLAY") == environment.end());
  EXPECT_EQ(environment["DISPLAY"].to_string(), ":0");
  EXPECT_EQ(environment["XDG_SESSION_TYPE"].to_string(), "x11");
  EXPECT_EQ(environment["XDG_CURRENT_DESKTOP"].to_string(), "gamescope");
}

/**
 * @brief Verify rejected evidence cannot alter a physical Desktop environment.
 */
TEST(ProcessDisplayEnvironmentTest, RejectedEndpointPreservesPhysicalDesktopEnvironment) {
  boost::process::v1::environment environment;
  environment["DISPLAY"] = ":0";
  steamos_virtual_session::session_display_endpoint_t endpoint {
    .gamescope_wayland_display = "gamescope-0",
    .x11_display = ":27",
    .verification = steamos_virtual_session::display_verification_e::rejected,
  };

  proc::apply_session_display_environment(environment, endpoint, true);

  EXPECT_EQ(environment["DISPLAY"].to_string(), ":0");
  EXPECT_TRUE(environment.find("XAUTHORITY") == environment.end());
  EXPECT_TRUE(environment.find("ENABLE_GAMESCOPE_WSI") == environment.end());
  EXPECT_TRUE(environment.find("DXVK_HDR") == environment.end());
}

/**
 * @brief Verify a virtual endpoint cannot retain an unverified host D-Bus address.
 */
TEST(ProcessDisplayEnvironmentTest, RemovesUnverifiedDbusAddressFromVirtualLaunch) {
  boost::process::v1::environment environment;
  environment["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/run/user/1000/unverified-bus";
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .origin = steamos_virtual_session::session_origin_e::owned_private,
    .xdg_runtime_directory = "/run/user/1000/steamshine/session-7",
    .wayland_display = "gamescope-0",
    .gamescope_wayland_display = "gamescope-0",
    .x11_display = ":7",
    .xauthority = "/run/user/1000/steamshine/session-7/xauthority",
    .pipewire_runtime_directory = "/run/user/1000",
    .pipewire_remote = "pipewire-0",
    .pulse_runtime_path = "/run/user/1000/pulse",
    .verification = steamos_virtual_session::display_verification_e::verified,
  };

  proc::apply_session_display_environment(environment, endpoint, true);

  EXPECT_TRUE(environment.find("DBUS_SESSION_BUS_ADDRESS") == environment.end());
}

TEST(ProcessCommandSelectionTest, SelectsOwnedDesktopOnlyForCaptureOnlyApplication) {
  EXPECT_TRUE(proc::should_launch_owned_virtual_desktop("", 0, true));
  EXPECT_FALSE(proc::should_launch_owned_virtual_desktop("", 1, true));
  EXPECT_FALSE(proc::should_launch_owned_virtual_desktop("game --launch", 0, true));
  EXPECT_FALSE(proc::should_launch_owned_virtual_desktop("", 0, false));
}

/**
 * @brief Verify Big Picture launch commands request an owned canvas without name matching.
 */
TEST(ProcessCommandSelectionTest, PrefersOwnedCanvasForEveryMoonlightApplication) {
  proc::ctx_t application {};
  application.name = "Localized Steam label";
  application.cmd = "steam steam://open/bigpicture";
  EXPECT_TRUE(proc::should_prefer_owned_virtual_display(application));

  application.cmd.clear();
  application.detached = {"setsid steam steam://open/bigpicture"};
  EXPECT_TRUE(proc::should_prefer_owned_virtual_display(application));

  application.detached = {"game --launch"};
  EXPECT_TRUE(proc::should_prefer_owned_virtual_display(application));

  application.prep_cmds.emplace_back("xdg-open steam://open/bigpicture", "", false);
  EXPECT_TRUE(proc::should_prefer_owned_virtual_display(application));

  application.id = "42";
  boost::process::v1::environment environment;
  proc::proc_t process_manager {std::move(environment), std::vector<proc::ctx_t> {application}};
  EXPECT_TRUE(process_manager.prefers_owned_virtual_display(42));
  EXPECT_FALSE(process_manager.prefers_owned_virtual_display(43));
}

/**
 * @brief Verify only commandless applications request the physical Desktop preference.
 */
TEST(ProcessCommandSelectionTest, PrefersPhysicalDesktopForCommandlessApplication) {
  proc::ctx_t application {};
  application.id = "42";
  EXPECT_TRUE(proc::should_prefer_physical_desktop(application));

  application.detached = {"game --launch"};
  EXPECT_FALSE(proc::should_prefer_physical_desktop(application));
  application.detached.clear();
  application.cmd = "game --launch";
  EXPECT_FALSE(proc::should_prefer_physical_desktop(application));
  application.cmd.clear();

  boost::process::v1::environment environment;
  proc::proc_t process_manager {std::move(environment), std::vector<proc::ctx_t> {application}};
  EXPECT_TRUE(process_manager.prefers_physical_desktop(42));
  EXPECT_FALSE(process_manager.prefers_physical_desktop(43));
}

/**
 * @brief Verify virtual display and HDR settings cannot leak into later physical launches.
 */
TEST(ProcessEnvironmentTest, RestoresBaselineBetweenLaunches) {
  boost::process::v1::environment baseline;
  baseline["STEAMSHINE_PROCESS_TEST_BASELINE"] = "configured";

  auto launch_environment {baseline};
  launch_environment["XDG_RUNTIME_DIR"] = "/owned/runtime";
  launch_environment["WAYLAND_DISPLAY"] = "gamescope-0";
  launch_environment["PIPEWIRE_REMOTE"] = "owned-pipewire";
  const steamos_virtual_session::session_display_endpoint_t endpoint {
    .origin = steamos_virtual_session::session_origin_e::owned_private,
    .xdg_runtime_directory = "/owned/runtime",
    .wayland_display = "gamescope-0",
    .gamescope_wayland_display = "gamescope-0",
    .pipewire_remote = "owned-pipewire",
    .verification = steamos_virtual_session::display_verification_e::verified,
  };
  proc::apply_session_display_environment(launch_environment, endpoint, true);
  ASSERT_EQ(launch_environment["DXVK_HDR"].to_string(), "1");

  proc::reset_launch_environment(launch_environment, baseline);

  EXPECT_EQ(launch_environment["STEAMSHINE_PROCESS_TEST_BASELINE"].to_string(), "configured");
  EXPECT_TRUE(launch_environment["XDG_RUNTIME_DIR"].empty());
  EXPECT_TRUE(launch_environment["WAYLAND_DISPLAY"].empty());
  EXPECT_TRUE(launch_environment["PIPEWIRE_REMOTE"].empty());
  EXPECT_TRUE(launch_environment.find("ENABLE_GAMESCOPE_WSI") == launch_environment.end());
  EXPECT_TRUE(launch_environment.find("DXVK_HDR") == launch_environment.end());
}

#if defined(__linux__)
/**
 * @brief Recover virtual-session state when the process deinitializer runs.
 */
TEST(ProcessLifecycleTest, DeinitializerRecoversVirtualSessionState) {
  const auto saved_virtual_display {config::steamos_virtual_display};
  auto restore_configuration {util::fail_guard([&saved_virtual_display]() {
    config::steamos_virtual_display = saved_virtual_display;
    steamos_virtual_session::stop();
  })};

  config::steamos_virtual_display.enabled = true;
  steamos_virtual_session::stop();
  rtsp_stream::launch_session_t launch {};
  launch.width = 1920;
  std::string error;
  ASSERT_FALSE(steamos_virtual_session::prepare(launch, error));
  ASSERT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Failed);

  auto deinitializer {proc::init()};
  ASSERT_TRUE(deinitializer);
  deinitializer.reset();

  EXPECT_EQ(steamos_virtual_session::state(), steamos_virtual_session::state_e::Idle);
}
#endif

class ProcessPNGTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}
