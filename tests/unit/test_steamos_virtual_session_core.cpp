/**
 * @file tests/unit/test_steamos_virtual_session_core.cpp
 * @brief Tests for standalone SteamOS virtual-session request helpers.
 */
#include <cstdlib>
#include <gtest/gtest.h>
#include <src/platform/linux/gamescope_presenter.h>
#include <src/platform/linux/gamescope_source.h>
#include <src/platform/linux/host_desktop_endpoint.h>
#include <src/platform/linux/steam_session.h>
#include <src/steamos_virtual_session_core.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  /**
   * @brief Verify missing and out-of-range client values use safe display bounds.
   */
  TEST(SteamOSVirtualSessionCore, NormalizesDisplayRequest) {
    const auto defaults {steamos_virtual_session::normalize_display_request(0, -1, 0, 1920, 1080, 60)};
    EXPECT_EQ(defaults.width, 1920);
    EXPECT_EQ(defaults.height, 1080);
    EXPECT_EQ(defaults.fps, 60);

    const auto bounds {steamos_virtual_session::normalize_display_request(1, 9000, 999, 1920, 1080, 60)};
    EXPECT_EQ(bounds.width, 640);
    EXPECT_EQ(bounds.height, 4320);
    EXPECT_EQ(bounds.fps, 240);
  }

  /**
   * @brief Verify only canonical virtual-display policy values are accepted.
   */
  TEST(SteamOSVirtualSessionCore, ParsesVirtualDisplayModes) {
    EXPECT_EQ(steamos_virtual_session::parse_virtual_display_mode("off"), steamos_virtual_session::virtual_display_mode_e::off);
    EXPECT_EQ(steamos_virtual_session::parse_virtual_display_mode("auto"), steamos_virtual_session::virtual_display_mode_e::auto_detect);
    EXPECT_EQ(steamos_virtual_session::parse_virtual_display_mode("force"), steamos_virtual_session::virtual_display_mode_e::force);
    EXPECT_FALSE(steamos_virtual_session::parse_virtual_display_mode("").has_value());
    EXPECT_FALSE(steamos_virtual_session::parse_virtual_display_mode("FORCE").has_value());
    EXPECT_FALSE(steamos_virtual_session::parse_virtual_display_mode("invalid").has_value());
  }

  /**
   * @brief Verify only canonical Gamescope source-policy values are accepted.
   */
  TEST(SteamOSVirtualSessionCore, ParsesSessionSourcePolicies) {
    using steamos_virtual_session::parse_session_source_policy;
    using steamos_virtual_session::session_origin_e;
    using steamos_virtual_session::session_source_policy_e;

    EXPECT_EQ(parse_session_source_policy("auto"), session_source_policy_e::auto_select);
    EXPECT_EQ(parse_session_source_policy("existing_gamescope"), session_source_policy_e::existing_gamescope);
    EXPECT_EQ(parse_session_source_policy("owned_private"), session_source_policy_e::owned_private);
    EXPECT_EQ(steamos_virtual_session::to_string(session_origin_e::attached_existing), "attached_existing");
    EXPECT_FALSE(parse_session_source_policy("existing").has_value());
    EXPECT_FALSE(parse_session_source_policy("AUTO").has_value());
  }

  /**
   * @brief Verify local presentation never mirrors an attached Game Mode output.
   */
  TEST(SteamOSVirtualSessionCore, DecidesLocalPresentationSafely) {
    using steamos_virtual_session::decide_presentation;
    using steamos_virtual_session::local_presentation_policy_e;
    using steamos_virtual_session::parse_local_presentation_policy;
    using steamos_virtual_session::presentation_e;
    using steamos_virtual_session::session_origin_e;

    EXPECT_EQ(parse_local_presentation_policy("mirror"), local_presentation_policy_e::mirror);
    EXPECT_FALSE(parse_local_presentation_policy("local").has_value());
    EXPECT_EQ(decide_presentation(local_presentation_policy_e::mirror, session_origin_e::owned_private, true, true), presentation_e::remote_and_local);
    EXPECT_EQ(decide_presentation(local_presentation_policy_e::auto_select, session_origin_e::attached_existing, true, true), presentation_e::remote_only);
    EXPECT_EQ(decide_presentation(local_presentation_policy_e::mirror, session_origin_e::owned_private, false, true), presentation_e::remote_only);
    EXPECT_EQ(steamos_virtual_session::to_string(presentation_e::remote_and_local), "remote_and_local");
  }

  /**
   * @brief Verify local presentation keeps only the newest acquired frame.
   */
  TEST(SteamOSVirtualSessionCore, UsesLatestFrameWinsPresentationQueue) {
    gamescope_presenter::latest_frame_queue_t queue;
    uint64_t released_sequence {};

    EXPECT_FALSE(queue.publish({.sequence = 1, .release = [&released_sequence] {
                                  released_sequence = 1;
                                }})
                   .replaced_pending_frame);
    EXPECT_TRUE(queue.publish({.sequence = 2, .release = [&released_sequence] {
                                 released_sequence = 2;
                               }})
                  .replaced_pending_frame);
    EXPECT_EQ(released_sequence, 1U);
    EXPECT_EQ(queue.pending_count(), 1U);

    const auto frame {queue.take_latest()};
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->sequence, 2U);
    EXPECT_EQ(queue.pending_count(), 0U);
    frame->release();
    EXPECT_EQ(released_sequence, 2U);
  }

  /**
   * @brief Verify force never falls back because a physical desktop exists.
   */
  TEST(SteamOSVirtualSessionCore, DecidesVirtualDisplayPolicy) {
    using steamos_virtual_session::decide_virtual_display;
    using steamos_virtual_session::virtual_display_decision_input_t;
    using steamos_virtual_session::virtual_display_mode_e;

    EXPECT_FALSE(decide_virtual_display({false, virtual_display_mode_e::force, true, true, true, false, true}).required);
    EXPECT_FALSE(decide_virtual_display({true, virtual_display_mode_e::off, false, false, false, false, true}).required);
    EXPECT_TRUE(decide_virtual_display({true, virtual_display_mode_e::force, true, true, true, false, true}).required);
    EXPECT_TRUE(decide_virtual_display({true, virtual_display_mode_e::force, false, false, true, false, true}).required);
    EXPECT_TRUE(decide_virtual_display({true, virtual_display_mode_e::auto_detect, false, false, false, false, true}).required);
    EXPECT_FALSE(decide_virtual_display({true, virtual_display_mode_e::auto_detect, true, true, true, false, true}).required);
  }

  /**
   * @brief Verify command generation only uses Gamescope-advertised options.
   */
  TEST(SteamOSVirtualSessionCore, BuildsHeadlessGamescopeCommand) {
    std::string error;
    const auto arguments {steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --nested-refresh --expose-wayland --scaler --hdr-enabled --prefer-vk-device", 2560, 1440, 120, true, "1002:744c", error)};
    EXPECT_TRUE(error.empty());
    const std::vector<std::string> expected {
      "--backend",
      "headless",
      "--nested-width",
      "2560",
      "--nested-height",
      "1440",
      "--nested-refresh",
      "120",
      "--expose-wayland",
      "--scaler",
      "fit",
      "--hdr-enabled",
      "--prefer-vk-device",
      "1002:744c"
    };
    EXPECT_EQ(arguments, expected);
  }

  /**
   * @brief Verify missing required Gamescope capability fails closed.
   */
  TEST(SteamOSVirtualSessionCore, RejectsUnsupportedGamescope) {
    std::string error;
    EXPECT_TRUE(steamos_virtual_session::gamescope_arguments("--nested-width --nested-height", 1920, 1080, 60, false, {}, error).empty());
    EXPECT_FALSE(error.empty());
  }

  /**
   * @brief Build a verified source with test-owned identity metadata.
   *
   * @param origin Ownership origin represented by the source.
   * @param pid Gamescope producer PID.
   * @param render_node Source DRM render node.
   * @return Fully verified test source.
   */
  gamescope_source::gamescope_source_t verified_source(const steamos_virtual_session::session_origin_e origin, const int pid, std::string render_node = "/dev/dri/renderD128") {
    return {
      .node_id = static_cast<uint32_t>(pid),
      .object_serial = static_cast<uint64_t>(pid) + 1000U,
      .client_id = static_cast<uint32_t>(pid) + 2000U,
      .producer_pid = pid,
      .producer_uid = 1000,
      .producer_start_time = static_cast<uint64_t>(pid) + 3000U,
      .executable = "/usr/bin/gamescope",
      .node_name = "gamescope",
      .node_description = "Gamescope Video/Source",
      .application_name = "gamescope",
      .media_class = "Video/Source",
      .render_node = std::move(render_node),
      .origin = origin,
      .identity_verified = true,
      .game_mode_verified = origin == steamos_virtual_session::session_origin_e::attached_existing,
    };
  }

  /**
   * @brief Verify automatic source selection prioritizes one resident Game Mode source.
   */
  TEST(SteamOSVirtualSessionCore, SelectsVerifiedExistingGamescopeBeforeOwnedSession) {
    const std::vector<gamescope_source::gamescope_source_t> sources {
      verified_source(steamos_virtual_session::session_origin_e::owned_private, 100),
      verified_source(steamos_virtual_session::session_origin_e::attached_existing, 200),
    };
    const auto selected {gamescope_source::select_gamescope_source(sources, {})};
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->origin, steamos_virtual_session::session_origin_e::attached_existing);
    EXPECT_EQ(selected->producer_pid, 200);
  }

  /**
   * @brief Verify ambiguous resident Gamescope sources fail closed without fallback.
   */
  TEST(SteamOSVirtualSessionCore, RejectsAmbiguousExistingGamescopeSources) {
    const std::vector<gamescope_source::gamescope_source_t> sources {
      verified_source(steamos_virtual_session::session_origin_e::attached_existing, 200),
      verified_source(steamos_virtual_session::session_origin_e::attached_existing, 201),
      verified_source(steamos_virtual_session::session_origin_e::owned_private, 100),
    };
    const auto selected {gamescope_source::select_gamescope_source(sources, {})};
    EXPECT_FALSE(selected.has_value());
    EXPECT_EQ(selected.error(), gamescope_source::source_error_e::ambiguous);
  }

  /**
   * @brief Verify explicit source selection rejects an invalid PID and mismatched GPU.
   */
  TEST(SteamOSVirtualSessionCore, RejectsInvalidExplicitGamescopeSource) {
    const std::vector<gamescope_source::gamescope_source_t> sources {
      verified_source(steamos_virtual_session::session_origin_e::attached_existing, 200),
    };
    const gamescope_source::source_selection_request_t missing_pid {
      .explicit_gamescope_pid = 201,
    };
    const auto missing {gamescope_source::select_gamescope_source(sources, missing_pid)};
    EXPECT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), gamescope_source::source_error_e::explicit_pid_invalid);

    const gamescope_source::source_selection_request_t other_gpu {
      .required_render_node = "/dev/dri/renderD129",
    };
    const auto mismatched {gamescope_source::select_gamescope_source(sources, other_gpu)};
    EXPECT_FALSE(mismatched.has_value());
    EXPECT_EQ(mismatched.error(), gamescope_source::source_error_e::unavailable);
  }

  /**
   * @brief Verify unverified, invalid, and non-Game-Mode candidates are rejected.
   */
  TEST(SteamOSVirtualSessionCore, RejectsUnverifiedExistingGamescopeSource) {
    auto source {verified_source(steamos_virtual_session::session_origin_e::attached_existing, 200)};
    source.identity_verified = false;
    const auto unverified {gamescope_source::select_gamescope_source({source}, {})};
    EXPECT_FALSE(unverified.has_value());
    EXPECT_EQ(unverified.error(), gamescope_source::source_error_e::unavailable);

    source = verified_source(steamos_virtual_session::session_origin_e::attached_existing, 200);
    source.game_mode_verified = false;
    const auto not_game_mode {gamescope_source::select_gamescope_source({source}, {})};
    EXPECT_FALSE(not_game_mode.has_value());
    EXPECT_EQ(not_game_mode.error(), gamescope_source::source_error_e::unavailable);
  }

  /**
   * @brief Verify process identity records a live PID and rejects a non-Gamescope executable.
   */
  TEST(SteamOSVirtualSessionCore, ReadsProcessIdentityWithoutTrustingPidAlone) {
    const auto identity {gamescope_source::read_process_identity(::getpid())};
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->pid, ::getpid());
    EXPECT_GE(identity->uid, 0);
    EXPECT_GT(identity->start_time, 0U);
    EXPECT_FALSE(identity->executable.empty());

    auto source {verified_source(steamos_virtual_session::session_origin_e::attached_existing, ::getpid())};
    source.producer_uid = identity->uid;
    source.producer_start_time = identity->start_time;
    source.executable = identity->executable.string();
    EXPECT_FALSE(gamescope_source::source_identity_is_current(source));
  }

  /**
   * @brief Verify Steam singleton placement uses target membership rather than a name alone.
   */
  TEST(SteamOSVirtualSessionCore, ClassifiesSteamInstanceLocation) {
    using steam_session::classify_instance_location;
    using steam_session::instance_location_e;
    using steam_session::process_record_t;
    using steam_session::target_session_t;

    const target_session_t target {
      .gamescope_pid = 100,
      .runtime_directory = "/run/user/1000/steamshine/session-1",
      .wayland_display = "gamescope-0",
      .cgroup = "user.slice/gamescope-session.scope",
    };
    EXPECT_EQ(classify_instance_location({}, target), instance_location_e::absent);

    const std::vector<process_record_t> inside {
      {.pid = 100, .parent_pid = 1, .executable_name = "gamescope"},
      {.pid = 101, .parent_pid = 100, .executable_name = "steam"},
    };
    EXPECT_EQ(classify_instance_location(inside, target), instance_location_e::inside_target_gamescope);

    const std::vector<process_record_t> outside {
      {.pid = 201, .parent_pid = 1, .executable_name = "steam", .xdg_runtime_directory = "/run/user/1000", .wayland_display = "gamescope-0"},
    };
    EXPECT_EQ(classify_instance_location(outside, target), instance_location_e::outside_target_gamescope);

    const std::vector<process_record_t> unknown {
      {.pid = 301, .parent_pid = 1, .executable_name = "steam", .metadata_readable = false},
    };
    EXPECT_EQ(classify_instance_location(unknown, target), instance_location_e::unknown);

    const std::vector<process_record_t> cgroup_inside {
      {.pid = 401, .parent_pid = 1, .executable_name = "steamwebhelper", .cgroup = target.cgroup},
    };
    EXPECT_EQ(classify_instance_location(cgroup_inside, target), instance_location_e::inside_target_gamescope);

    const target_session_t shared_cgroup_target {
      .gamescope_pid = 100,
      .cgroup = "user.slice/user-1000.slice/user@1000.service/app.slice",
    };
    const std::vector<process_record_t> shared_cgroup {
      {.pid = 501, .parent_pid = 1, .executable_name = "steam", .cgroup = shared_cgroup_target.cgroup},
    };
    EXPECT_EQ(classify_instance_location(shared_cgroup, shared_cgroup_target), instance_location_e::outside_target_gamescope);
  }

  /**
   * @brief Verify Steam command detection catches executable paths and URIs only.
   */
  TEST(SteamOSVirtualSessionCore, DetectsSteamCommandsWithoutMatchingSteamHelperNames) {
    EXPECT_TRUE(steam_session::command_references_steam("steam -tenfoot"));
    EXPECT_TRUE(steam_session::command_references_steam("/usr/bin/steam steam://open/bigpicture"));
    EXPECT_TRUE(steam_session::command_references_steam("xdg-open steam://open/bigpicture"));
    EXPECT_FALSE(steam_session::command_references_steam("steamwebhelper --type=renderer"));
    EXPECT_FALSE(steam_session::command_references_steam("/usr/bin/gamescope --steamcompmgr"));
  }

  /**
   * @brief Verify the local presenter endpoint cannot be replaced by later child setup.
   */
  TEST(SteamOSVirtualSessionCore, CapturesHostDesktopEndpointOnlyOnce) {
    const auto *const original_runtime {std::getenv("XDG_RUNTIME_DIR")};
    const auto *const original_wayland {std::getenv("WAYLAND_DISPLAY")};
    const bool had_runtime {original_runtime != nullptr};
    const bool had_wayland {original_wayland != nullptr};
    const std::string saved_runtime {original_runtime ? original_runtime : ""};
    const std::string saved_wayland {original_wayland ? original_wayland : ""};
    host_desktop_endpoint::capture();
    const auto original {host_desktop_endpoint::current()};

    ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", "/tmp/steamshine-private-runtime", 1), 0);
    ASSERT_EQ(::setenv("WAYLAND_DISPLAY", "gamescope-0", 1), 0);
    host_desktop_endpoint::capture();

    const auto retained {host_desktop_endpoint::current()};
    EXPECT_EQ(retained.xdg_runtime_directory, original.xdg_runtime_directory);
    EXPECT_EQ(retained.wayland_display, original.wayland_display);
    EXPECT_EQ(retained.x11_display, original.x11_display);
    if (had_runtime) {
      ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", saved_runtime.c_str(), 1), 0);
    } else {
      ASSERT_EQ(::unsetenv("XDG_RUNTIME_DIR"), 0);
    }
    if (had_wayland) {
      ASSERT_EQ(::setenv("WAYLAND_DISPLAY", saved_wayland.c_str(), 1), 0);
    } else {
      ASSERT_EQ(::unsetenv("WAYLAND_DISPLAY"), 0);
    }
  }
}  // namespace
