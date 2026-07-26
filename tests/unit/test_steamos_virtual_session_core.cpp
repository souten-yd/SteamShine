/**
 * @file tests/unit/test_steamos_virtual_session_core.cpp
 * @brief Tests for standalone SteamOS virtual-session request helpers.
 */
#include <gtest/gtest.h>
#include <src/platform/linux/gamescope_source.h>
#include <src/steamos_virtual_session_core.h>
#include <string>
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
    using steamos_virtual_session::session_source_policy_e;

    EXPECT_EQ(parse_session_source_policy("auto"), session_source_policy_e::auto_select);
    EXPECT_EQ(parse_session_source_policy("existing_gamescope"), session_source_policy_e::existing_gamescope);
    EXPECT_EQ(parse_session_source_policy("owned_private"), session_source_policy_e::owned_private);
    EXPECT_FALSE(parse_session_source_policy("existing").has_value());
    EXPECT_FALSE(parse_session_source_policy("AUTO").has_value());
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
}  // namespace
