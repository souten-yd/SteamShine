/**
 * @file tests/unit/test_steamos_virtual_session_core.cpp
 * @brief Tests for standalone SteamOS virtual-session request helpers.
 */
#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <src/platform/linux/gamescope_presenter.h>
#include <src/platform/linux/gamescope_source.h>
#include <src/platform/linux/host_desktop_endpoint.h>
#include <src/platform/linux/pipewire_capture.h>
#include <src/platform/linux/steam_session.h>
#include <src/steamos_virtual_session_core.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  /**
   * @brief Verify missing values use defaults while malformed values fail closed.
   */
  TEST(SteamOSVirtualSessionCore, NormalizesDisplayRequest) {
    const auto defaults {steamos_virtual_session::normalize_display_request(0, -1, 0, 1920, 1080, 60)};
    EXPECT_FALSE(defaults.valid);
    EXPECT_EQ(defaults.reason, "invalid_nonpositive_geometry");

    const auto missing {steamos_virtual_session::normalize_display_request(0, 0, 0, 1920, 1080, 60)};
    EXPECT_TRUE(missing.valid);
    EXPECT_EQ(missing.width, 1920);
    EXPECT_EQ(missing.height, 1080);
    EXPECT_EQ(missing.fps, 60);
    EXPECT_EQ(missing.reason, "default_geometry_selected");

    const auto bounds {steamos_virtual_session::normalize_display_request(1, 9000, 999, 1920, 1080, 60)};
    EXPECT_FALSE(bounds.valid);
    EXPECT_EQ(bounds.reason, "geometry_out_of_bounds");
  }

  /**
   * @brief Verify alignment policy records minimal adjustment or rejects exact mode.
   */
  TEST(SteamOSVirtualSessionCore, AppliesExplicitGeometryAlignmentPolicy) {
    using steamos_virtual_session::geometry_alignment_policy_e;
    using steamos_virtual_session::margin_input_policy_e;
    using steamos_virtual_session::select_display_request;

    EXPECT_EQ(steamos_virtual_session::parse_geometry_alignment_policy("auto"), geometry_alignment_policy_e::auto_align);
    EXPECT_EQ(steamos_virtual_session::parse_geometry_alignment_policy("require_exact"), geometry_alignment_policy_e::require_exact);
    EXPECT_FALSE(steamos_virtual_session::parse_geometry_alignment_policy("exact").has_value());
    EXPECT_EQ(steamos_virtual_session::parse_margin_input_policy("clamp"), margin_input_policy_e::clamp);
    EXPECT_EQ(steamos_virtual_session::parse_margin_input_policy("reject"), margin_input_policy_e::reject);
    EXPECT_FALSE(steamos_virtual_session::parse_margin_input_policy("drop").has_value());

    const auto automatic {select_display_request(1921, 1081, 60, 0, 1920, 1080, 60, geometry_alignment_policy_e::auto_align)};
    ASSERT_TRUE(automatic.valid);
    EXPECT_EQ(automatic.requested_width, 1921);
    EXPECT_EQ(automatic.requested_height, 1081);
    EXPECT_EQ(automatic.width, 1922);
    EXPECT_EQ(automatic.height, 1082);
    EXPECT_TRUE(automatic.adjusted);
    EXPECT_EQ(automatic.reason, "geometry_minimally_aligned");

    const auto exact {select_display_request(1921, 1081, 60, 0, 1920, 1080, 60, geometry_alignment_policy_e::require_exact)};
    EXPECT_FALSE(exact.valid);
    EXPECT_EQ(exact.reason, "exact_geometry_unaligned");
  }

  /**
   * @brief Verify exact rational refresh values across the required 30-240 FPS matrix.
   */
  TEST(SteamOSVirtualSessionCore, PreservesSafeRationalRefreshMatrix) {
    using steamos_virtual_session::geometry_alignment_policy_e;
    using steamos_virtual_session::select_display_request;

    for (const int fps : {30, 50, 60, 75, 90, 100, 120, 144, 165, 240}) {
      const auto selected {select_display_request(1920, 1080, fps, 0, 1920, 1080, 60, geometry_alignment_policy_e::auto_align)};
      ASSERT_TRUE(selected.valid) << fps;
      EXPECT_EQ(selected.refresh.numerator, static_cast<std::uint32_t>(fps));
      EXPECT_EQ(selected.refresh.denominator, 1U);
    }
    const auto ntsc {select_display_request(1920, 1080, 60, 5994, 1920, 1080, 60, geometry_alignment_policy_e::auto_align)};
    ASSERT_TRUE(ntsc.valid);
    EXPECT_EQ(ntsc.refresh.numerator, 2997U);
    EXPECT_EQ(ntsc.refresh.denominator, 50U);
    EXPECT_EQ(ntsc.fps, 60);
  }

  /**
   * @brief Verify extent, pixel-rate, and buffer overflow checks fail with stable reasons.
   */
  TEST(SteamOSVirtualSessionCore, RejectsUnsafeGeometryBudgets) {
    using steamos_virtual_session::display_constraints_t;
    using steamos_virtual_session::geometry_alignment_policy_e;
    using steamos_virtual_session::select_display_request;

    display_constraints_t pixel_rate_limits;
    pixel_rate_limits.maximum_pixel_rate = 1920ULL * 1080ULL * 60ULL;
    const auto too_fast {select_display_request(3840, 2160, 60, 0, 1920, 1080, 60, geometry_alignment_policy_e::auto_align, pixel_rate_limits)};
    EXPECT_FALSE(too_fast.valid);
    EXPECT_EQ(too_fast.reason, "pixel_rate_exceeded");

    display_constraints_t coded_limits;
    coded_limits.maximum_frame_pixels = 1920ULL * 1080ULL;
    const auto too_large {select_display_request(2560, 1440, 60, 0, 1920, 1080, 60, geometry_alignment_policy_e::auto_align, coded_limits)};
    EXPECT_FALSE(too_large.valid);
    EXPECT_EQ(too_large.reason, "coded_extent_exceeded");

    display_constraints_t overflow_limits {
      .minimum_width = 1,
      .maximum_width = std::numeric_limits<int>::max(),
      .minimum_height = 1,
      .maximum_height = std::numeric_limits<int>::max(),
      .width_alignment = 1,
      .height_alignment = 1,
      .minimum_fps = 1,
      .maximum_fps = 1,
      .maximum_frame_pixels = std::numeric_limits<std::uint64_t>::max(),
      .maximum_pixel_rate = std::numeric_limits<std::uint64_t>::max(),
      .maximum_buffer_bytes = std::numeric_limits<std::uint64_t>::max(),
      .buffer_count = 4,
      .bytes_per_pixel = 4,
    };
    const auto overflow {select_display_request(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 1, 0, 1, 1, 1, geometry_alignment_policy_e::auto_align, overflow_limits)};
    EXPECT_FALSE(overflow.valid);
    EXPECT_EQ(overflow.reason, "buffer_size_overflow");
  }

  /**
   * @brief Verify retained reuse includes geometry, rational refresh, color, GPU, and source format.
   */
  TEST(SteamOSVirtualSessionCore, ComparesCompleteRetainedSessionKey) {
    using steamos_virtual_session::retained_session_compatible;
    using steamos_virtual_session::retained_session_key_t;

    const retained_session_key_t retained {
      .width = 2560,
      .height = 1600,
      .refresh = {2997, 25},
      .hdr = false,
      .render_node = "/dev/dri/renderD128",
      .source_identity = "/usr/bin/gamescope",
      .capture_pixel_format = "NV12",
    };
    EXPECT_TRUE(retained_session_compatible(retained, retained));
    for (auto changed : {
           retained_session_key_t {retained.width + 2, retained.height, retained.refresh, retained.hdr, retained.render_node, retained.source_identity, retained.capture_pixel_format},
           retained_session_key_t {retained.width, retained.height, {120, 1}, retained.hdr, retained.render_node, retained.source_identity, retained.capture_pixel_format},
           retained_session_key_t {retained.width, retained.height, retained.refresh, true, retained.render_node, retained.source_identity, "P010"},
           retained_session_key_t {retained.width, retained.height, retained.refresh, retained.hdr, "/dev/dri/renderD129", retained.source_identity, retained.capture_pixel_format},
           retained_session_key_t {retained.width, retained.height, retained.refresh, retained.hdr, retained.render_node, "/opt/gamescope", retained.capture_pixel_format},
         }) {
      EXPECT_FALSE(retained_session_compatible(retained, changed));
    }
    auto changed_backend {retained};
    changed_backend.backend = steamos_virtual_session::owned_backend_e::wayland_nested;
    EXPECT_FALSE(retained_session_compatible(retained, changed_backend));
    auto changed_generation {retained};
    changed_generation.host_endpoint_generation = 2;
    EXPECT_FALSE(retained_session_compatible(retained, changed_generation));
    auto changed_presentation {retained};
    changed_presentation.local_presentation_required = true;
    EXPECT_FALSE(retained_session_compatible(retained, changed_presentation));
  }

  /**
   * @brief Verify fitted rectangles and margin input policy preserve source aspect ratio.
   */
  TEST(SteamOSVirtualSessionCore, FitsContentAndMapsMargins) {
    using steamos_virtual_session::fit_content_rectangle;
    using steamos_virtual_session::map_content_coordinate;
    using steamos_virtual_session::margin_input_policy_e;

    const auto letterbox {fit_content_rectangle(3440, 1440, 1920, 1080)};
    EXPECT_EQ(letterbox.x, 0);
    EXPECT_EQ(letterbox.y, 138);
    EXPECT_EQ(letterbox.width, 1920);
    EXPECT_EQ(letterbox.height, 802);

    const auto pillarbox {fit_content_rectangle(1920, 1080, 3440, 1440)};
    EXPECT_EQ(pillarbox.x, 440);
    EXPECT_EQ(pillarbox.y, 0);
    EXPECT_EQ(pillarbox.width, 2560);
    EXPECT_EQ(pillarbox.height, 1440);

    const auto center {map_content_coordinate(960.0, 539.0, letterbox, margin_input_policy_e::reject)};
    ASSERT_TRUE(center.accepted);
    EXPECT_DOUBLE_EQ(center.x, 0.5);
    EXPECT_DOUBLE_EQ(center.y, 0.5);

    EXPECT_FALSE(map_content_coordinate(960.0, 20.0, letterbox, margin_input_policy_e::reject).accepted);
    const auto clamped {map_content_coordinate(960.0, 20.0, letterbox, margin_input_policy_e::clamp)};
    ASSERT_TRUE(clamped.accepted);
    EXPECT_DOUBLE_EQ(clamped.x, 0.5);
    EXPECT_DOUBLE_EQ(clamped.y, 0.0);
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
    const auto dma_buf {std::make_shared<gamescope_presenter::dma_buf_frame_t>()};
    dma_buf->width = 1280;
    dma_buf->height = 720;

    EXPECT_FALSE(queue.publish({.sequence = 1, .release = [&released_sequence] {
                                  released_sequence = 1;
                                }})
                   .replaced_pending_frame);
    EXPECT_TRUE(queue.publish({.sequence = 2, .dma_buf = dma_buf, .release = [&released_sequence] {
                                 released_sequence = 2;
                               }})
                  .replaced_pending_frame);
    EXPECT_EQ(released_sequence, 1U);
    EXPECT_EQ(queue.pending_count(), 1U);

    const auto frame {queue.take_latest()};
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->sequence, 2U);
    ASSERT_EQ(frame->dma_buf, dma_buf);
    EXPECT_EQ(frame->dma_buf->width, 1280U);
    EXPECT_EQ(frame->dma_buf->height, 720U);
    EXPECT_EQ(queue.pending_count(), 0U);
    frame->release();
    EXPECT_EQ(released_sequence, 2U);
  }

  /**
   * @brief Verify explicit policies and automatic priority select one route.
   */
  TEST(SteamOSVirtualSessionCore, SelectsSessionRoutePolicy) {
    using steamos_virtual_session::select_session_route;
    using steamos_virtual_session::session_route_e;
    using steamos_virtual_session::session_route_input_t;
    using steamos_virtual_session::session_source_policy_e;
    using steamos_virtual_session::virtual_display_mode_e;

    const auto input = [](const virtual_display_mode_e mode, const session_source_policy_e source_policy) {
      return session_route_input_t {
        .feature_enabled = true,
        .mode = mode,
        .source_policy = source_policy,
        .prefer_owned_session = false,
        .prefer_physical_desktop = false,
        .startup_preflight_owned_session = false,
        .capturable_output_present = false,
        .retained_owned_session = false,
        .host_supported = true,
        .verified_existing_gamescope_present = false,
      };
    };

    auto request {input(virtual_display_mode_e::auto_detect, session_source_policy_e::auto_select)};
    EXPECT_EQ(select_session_route(request).route, session_route_e::new_owned_private);
    request.capturable_output_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::physical_desktop);
    request.verified_existing_gamescope_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    request.retained_owned_session = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);

    request.prefer_physical_desktop = true;
    request.live_kwin_available = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::physical_desktop);
    EXPECT_EQ(select_session_route(request).reason, "desktop_application_capturable_output");
    request.capturable_output_present = false;
    request.live_kwin_available = false;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    request.prefer_physical_desktop = false;

    request.startup_preflight_owned_session = true;
    request.prefer_owned_session = true;
    request.retained_owned_session = false;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    EXPECT_EQ(select_session_route(request).reason, "verified_existing_gamescope_without_kwin");
    request.retained_owned_session = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    EXPECT_EQ(select_session_route(request).reason, "verified_existing_gamescope_without_kwin");
    request.startup_preflight_owned_session = false;

    request.verified_existing_gamescope_present = false;
    request.retained_owned_session = false;
    request.prefer_owned_session = true;
    request.capturable_output_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::new_owned_private);
    EXPECT_EQ(select_session_route(request).reason, "application_owned_private");
    request.retained_owned_session = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::retained_owned_private);
    EXPECT_EQ(select_session_route(request).reason, "application_retained_owned_private");
    request.retained_owned_session = false;
    request.host_supported = false;
    EXPECT_EQ(select_session_route(request).route, session_route_e::reject);
    EXPECT_EQ(select_session_route(request).reason, "application_owned_private_host_unsupported");

    request = input(virtual_display_mode_e::auto_detect, session_source_policy_e::owned_private);
    request.capturable_output_present = true;
    request.verified_existing_gamescope_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    request.live_kwin_available = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::new_owned_private);
    request.retained_owned_session = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::retained_owned_private);

    request = input(virtual_display_mode_e::auto_detect, session_source_policy_e::existing_gamescope);
    EXPECT_EQ(select_session_route(request).route, session_route_e::reject);
    request.verified_existing_gamescope_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);

    request = input(virtual_display_mode_e::force, session_source_policy_e::auto_select);
    request.capturable_output_present = true;
    request.verified_existing_gamescope_present = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::attached_existing);
    EXPECT_EQ(select_session_route(request).reason, "verified_existing_gamescope_without_kwin");
    request.live_kwin_available = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::new_owned_private);
    request.retained_owned_session = true;
    EXPECT_EQ(select_session_route(request).route, session_route_e::retained_owned_private);

    request.feature_enabled = false;
    EXPECT_EQ(select_session_route(request).route, session_route_e::physical_desktop);
    request.feature_enabled = true;
    request.mode = virtual_display_mode_e::off;
    EXPECT_EQ(select_session_route(request).route, session_route_e::physical_desktop);

    request = input(virtual_display_mode_e::force, session_source_policy_e::auto_select);
    request.host_supported = false;
    EXPECT_EQ(select_session_route(request).route, session_route_e::reject);
    request.mode = virtual_display_mode_e::auto_detect;
    request.source_policy = session_source_policy_e::owned_private;
    EXPECT_EQ(select_session_route(request).route, session_route_e::reject);
    request.source_policy = session_source_policy_e::auto_select;
    EXPECT_EQ(select_session_route(request).route, session_route_e::reject);

    EXPECT_TRUE(steamos_virtual_session::route_uses_gamescope_capture(session_route_e::reject));
    EXPECT_FALSE(steamos_virtual_session::route_uses_gamescope_capture(session_route_e::physical_desktop));
    EXPECT_EQ(steamos_virtual_session::to_string(session_route_e::physical_desktop), "physical_desktop");
    EXPECT_EQ(steamos_virtual_session::to_string(session_route_e::attached_existing), "attached_existing");
    EXPECT_EQ(steamos_virtual_session::to_string(session_route_e::retained_owned_private), "retained_owned_private");
    EXPECT_EQ(steamos_virtual_session::to_string(session_route_e::new_owned_private), "new_owned_private");
    EXPECT_EQ(steamos_virtual_session::to_string(session_route_e::reject), "reject");
  }

  /**
   * @brief Verify the four physical-output startup and reconnect scenarios.
   */
  TEST(SteamOSVirtualSessionCore, SelectsRoutesAcrossRequiredReconnectScenarios) {
    using steamos_virtual_session::select_session_route;
    using steamos_virtual_session::session_route_e;
    using steamos_virtual_session::session_route_input_t;
    using steamos_virtual_session::session_source_policy_e;
    using steamos_virtual_session::virtual_display_mode_e;

    session_route_input_t observation {
      .feature_enabled = true,
      .mode = virtual_display_mode_e::auto_detect,
      .source_policy = session_source_policy_e::auto_select,
      .prefer_owned_session = false,
      .prefer_physical_desktop = false,
      .startup_preflight_owned_session = false,
      .capturable_output_present = false,
      .retained_owned_session = false,
      .host_supported = true,
      .verified_existing_gamescope_present = false,
    };

    EXPECT_EQ(select_session_route(observation).route, session_route_e::new_owned_private);
    observation.retained_owned_session = true;
    observation.verified_existing_gamescope_present = true;
    EXPECT_EQ(select_session_route(observation).route, session_route_e::attached_existing);

    observation = {
      .feature_enabled = true,
      .mode = virtual_display_mode_e::auto_detect,
      .source_policy = session_source_policy_e::auto_select,
      .prefer_owned_session = false,
      .prefer_physical_desktop = false,
      .startup_preflight_owned_session = false,
      .capturable_output_present = true,
      .retained_owned_session = false,
      .host_supported = true,
      .verified_existing_gamescope_present = false,
    };
    EXPECT_EQ(select_session_route(observation).route, session_route_e::physical_desktop);
    EXPECT_EQ(select_session_route(observation).route, session_route_e::physical_desktop);

    EXPECT_EQ(select_session_route(observation).route, session_route_e::physical_desktop);

    observation.capturable_output_present = false;
    EXPECT_EQ(select_session_route(observation).route, session_route_e::new_owned_private);
    observation.retained_owned_session = true;
    observation.capturable_output_present = true;
    EXPECT_EQ(select_session_route(observation).route, session_route_e::physical_desktop);
  }

  /**
   * @brief Verify a working physical Desktop portal outranks automatic virtual capture.
   */
  TEST(SteamOSVirtualSessionCore, PrefersPhysicalDesktopCompositorCapture) {
    using steamos_virtual_session::physical_desktop_capturable;
    using steamos_virtual_session::should_probe_physical_portal;
    using steamos_virtual_session::use_virtual_capture_backend;

    EXPECT_TRUE(physical_desktop_capturable(true, true, false));
    EXPECT_TRUE(physical_desktop_capturable(true, false, true));
    EXPECT_FALSE(physical_desktop_capturable(true, false, false));
    EXPECT_FALSE(physical_desktop_capturable(false, true, true));
    EXPECT_FALSE(use_virtual_capture_backend(true, true, true, false));
    EXPECT_TRUE(use_virtual_capture_backend(true, true, false, false));
    EXPECT_TRUE(use_virtual_capture_backend(true, false, true, false));
    EXPECT_TRUE(use_virtual_capture_backend(true, true, true, true));
    EXPECT_FALSE(use_virtual_capture_backend(false, false, false, false));
    EXPECT_TRUE(should_probe_physical_portal(true, false, true, false));
    EXPECT_FALSE(should_probe_physical_portal(true, false, true, true));
    EXPECT_FALSE(should_probe_physical_portal(true, false, false, false));
    EXPECT_FALSE(should_probe_physical_portal(false, false, true, false));
    EXPECT_TRUE(should_probe_physical_portal(false, true, false, true));
  }

  /**
   * @brief Verify an EIS path is derived only from a contained display name.
   */
  TEST(SteamOSVirtualSessionCore, DerivesContainedGamescopeEisSocketPath) {
    EXPECT_EQ(
      steamos_virtual_session::gamescope_eis_socket_path("/run/user/1000/session", "gamescope-0"),
      std::filesystem::path {"/run/user/1000/session/gamescope-0-ei"}
    );
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path({}, "gamescope-0").has_value());
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path("relative", "gamescope-0").has_value());
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path("/run/user/1000/session", {}).has_value());
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path("/run/user/1000/session", ".").has_value());
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path("/run/user/1000/session", "..").has_value());
    EXPECT_FALSE(steamos_virtual_session::gamescope_eis_socket_path("/run/user/1000/session", "../bus").has_value());
  }

  /**
   * @brief Verify command generation only uses Gamescope-advertised options.
   */
  TEST(SteamOSVirtualSessionCore, BuildsGeneralApplicationHeadlessGamescopeCommand) {
    std::string error;
    const auto arguments {steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --output-width --output-height --nested-refresh --expose-wayland --steam --scaler --hdr-enabled --prefer-vk-device", 2560, 1440, 120, true, "1002:744c", error)};
    EXPECT_TRUE(error.empty());
    const std::vector<std::string> expected {
      "--backend",
      "headless",
      "--nested-width",
      "2560",
      "--nested-height",
      "1440",
      "--output-width",
      "2560",
      "--output-height",
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
   * @brief Verify nested Gamescope uses only advertised Wayland fullscreen options.
   */
  TEST(SteamOSVirtualSessionCore, BuildsNestedGamescopeCommand) {
    std::string error;
    const auto arguments {steamos_virtual_session::gamescope_arguments(
      "--backend headless wayland --fullscreen --nested-width --nested-height --nested-refresh --expose-wayland --scaler --prefer-vk-device",
      1920,
      1080,
      60,
      false,
      "1002:744c",
      error,
      steamos_virtual_session::owned_backend_e::wayland_nested
    )};
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(arguments, (std::vector<std::string> {"--backend", "wayland", "--fullscreen", "--nested-width", "1920", "--nested-height", "1080", "--nested-refresh", "60", "--expose-wayland", "--scaler", "fit", "--prefer-vk-device", "1002:744c"}));

    EXPECT_TRUE(steamos_virtual_session::gamescope_arguments("--backend wayland --nested-width --nested-height --nested-refresh --expose-wayland", 1920, 1080, 60, false, {}, error, steamos_virtual_session::owned_backend_e::wayland_nested).empty());
  }

  /**
   * @brief Verify local presentation selects nested, fallback, and refusal routes.
   */
  TEST(SteamOSVirtualSessionCore, SelectsOwnedBackendSafely) {
    using steamos_virtual_session::local_presentation_policy_e;
    using steamos_virtual_session::owned_backend_e;
    using steamos_virtual_session::select_owned_backend;

    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::off, true, true, false, false), owned_backend_e::headless);
    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::auto_select, true, true, false, false), owned_backend_e::wayland_nested);
    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::auto_select, true, true, true, true), owned_backend_e::wayland_nested);
    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::auto_select, true, true, true, false), owned_backend_e::headless);
    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::auto_select, false, true, false, false), owned_backend_e::headless);
    EXPECT_FALSE(select_owned_backend(local_presentation_policy_e::mirror, false, true, false, false));
    EXPECT_FALSE(select_owned_backend(local_presentation_policy_e::mirror, true, true, true, false));
    EXPECT_EQ(select_owned_backend(local_presentation_policy_e::mirror, true, true, true, true), owned_backend_e::wayland_nested);
    EXPECT_EQ(steamos_virtual_session::to_string(owned_backend_e::headless), "headless");
    EXPECT_EQ(steamos_virtual_session::to_string(owned_backend_e::wayland_nested), "wayland_nested");
  }

  /**
   * @brief Verify Steam migration policies and observable states use stable spellings.
   */
  TEST(SteamOSVirtualSessionCore, ParsesSteamMigrationPolicyAndStates) {
    using steamos_virtual_session::parse_steam_migration_policy;
    using steamos_virtual_session::steam_migration_policy_e;
    using steamos_virtual_session::steam_migration_state_e;

    EXPECT_EQ(parse_steam_migration_policy("reject"), steam_migration_policy_e::reject);
    EXPECT_EQ(parse_steam_migration_policy("auto_idle"), steam_migration_policy_e::auto_idle);
    EXPECT_FALSE(parse_steam_migration_policy("auto"));
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_policy_e::reject), "reject");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_policy_e::auto_idle), "auto_idle");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::not_needed), "not_needed");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::checking_idle), "checking_idle");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::shutting_down), "shutting_down");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::migrated), "migrated");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::blocked_active_game), "blocked_active_game");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::blocked_unknown), "blocked_unknown");
    EXPECT_EQ(steamos_virtual_session::to_string(steam_migration_state_e::shutdown_timeout), "shutdown_timeout");
  }

  /**
   * @brief Verify stock handoff is limited to an explicit idle application launch.
   */
  TEST(SteamOSVirtualSessionCore, SelectsStockSessionHandoffSafely) {
    using steamos_virtual_session::parse_stock_handoff_policy;
    using steamos_virtual_session::select_stock_handoff_action;
    using steamos_virtual_session::stock_activity_e;
    using steamos_virtual_session::stock_handoff_action_e;
    using steamos_virtual_session::stock_handoff_policy_e;
    using steamos_virtual_session::stock_handoff_state_e;

    EXPECT_EQ(parse_stock_handoff_policy("attach"), stock_handoff_policy_e::attach);
    EXPECT_EQ(parse_stock_handoff_policy("auto_idle"), stock_handoff_policy_e::auto_idle);
    EXPECT_FALSE(parse_stock_handoff_policy("always"));
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_policy_e::attach), "attach");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_policy_e::auto_idle), "auto_idle");
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::auto_idle, true, false, stock_activity_e::idle), stock_handoff_action_e::handoff_owned);
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::attach, true, false, stock_activity_e::idle), stock_handoff_action_e::attach);
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::auto_idle, false, false, stock_activity_e::idle), stock_handoff_action_e::attach);
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::auto_idle, true, true, stock_activity_e::idle), stock_handoff_action_e::attach);
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::auto_idle, true, false, stock_activity_e::active_game), stock_handoff_action_e::attach);
    EXPECT_EQ(select_stock_handoff_action(stock_handoff_policy_e::auto_idle, true, false, stock_activity_e::unknown), stock_handoff_action_e::attach);
    EXPECT_FALSE(steamos_virtual_session::systemctl_job_mode_argument(true));
    ASSERT_TRUE(steamos_virtual_session::systemctl_job_mode_argument(false));
    EXPECT_EQ(*steamos_virtual_session::systemctl_job_mode_argument(false), "--no-block");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_state_e::inactive), "inactive");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_state_e::attached_active_game), "attached_active_game");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_state_e::owned_active), "owned_active");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_state_e::restored), "restored");
    EXPECT_EQ(steamos_virtual_session::to_string(stock_handoff_state_e::failed), "failed");
  }

  /**
   * @brief Verify Desktop Steam may migrate into either owned compositor backend.
   */
  TEST(SteamOSVirtualSessionCore, AllowsDesktopSteamMigrationIntoOwnedBackends) {
    using steamos_virtual_session::owned_backend_e;
    using steamos_virtual_session::session_origin_e;
    using steamos_virtual_session::steam_migration_allowed;
    using steamos_virtual_session::steam_migration_policy_e;

    EXPECT_TRUE(steam_migration_allowed(steam_migration_policy_e::auto_idle, true, session_origin_e::owned_private, owned_backend_e::headless));
    EXPECT_TRUE(steam_migration_allowed(steam_migration_policy_e::auto_idle, true, session_origin_e::owned_private, owned_backend_e::wayland_nested));
    EXPECT_FALSE(steam_migration_allowed(steam_migration_policy_e::reject, true, session_origin_e::owned_private, owned_backend_e::headless));
    EXPECT_FALSE(steam_migration_allowed(steam_migration_policy_e::auto_idle, false, session_origin_e::owned_private, owned_backend_e::headless));
    EXPECT_FALSE(steam_migration_allowed(steam_migration_policy_e::auto_idle, true, session_origin_e::attached_existing, owned_backend_e::headless));
  }

  /**
   * @brief Verify Steam-controlled focus is omitted even when Gamescope advertises it.
   */
  TEST(SteamOSVirtualSessionCore, OmitsSteamControlledFocusForGeneralApplications) {
    std::string error;
    const auto arguments {steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --output-width --output-height --nested-refresh --expose-wayland --steam", 1920, 1080, 60, false, {}, error)};

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(std::ranges::find(arguments, "--steam"), arguments.end());
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
   * @brief Verify modern headless Gamescope cannot silently use its 1280x720 output default.
   */
  TEST(SteamOSVirtualSessionCore, RejectsModernHeadlessWithoutOutputSizeOptions) {
    std::string error;
    EXPECT_TRUE(steamos_virtual_session::gamescope_arguments("--backend headless --nested-width --nested-height --nested-refresh --expose-wayland", 1920, 1080, 60, false, {}, error).empty());
    EXPECT_EQ(error, "Installed Gamescope does not advertise headless output size options");
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
   * @brief Verify Gamescope's current output-stream class and the legacy source class are accepted.
   */
  TEST(SteamOSVirtualSessionCore, AcceptsGamescopeCaptureMediaClasses) {
    EXPECT_TRUE(gamescope_source::is_gamescope_capture_media_class("Stream/Output/Video"));
    EXPECT_TRUE(gamescope_source::is_gamescope_capture_media_class("Video/Source"));
    EXPECT_FALSE(gamescope_source::is_gamescope_capture_media_class("Stream/Input/Video"));
    EXPECT_FALSE(gamescope_source::is_gamescope_capture_media_class("Audio/Source"));

    auto current_gamescope {verified_source(steamos_virtual_session::session_origin_e::attached_existing, 220)};
    current_gamescope.media_class = "Stream/Output/Video";
    const auto selected {gamescope_source::select_gamescope_source({current_gamescope}, {})};
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->producer_pid, 220);
  }

  /**
   * @brief Verify absent GPU metadata is allowed while contradictory metadata remains rejected.
   */
  TEST(SteamOSVirtualSessionCore, AllowsAbsentGamescopeRenderNodeMetadata) {
    auto source {verified_source(steamos_virtual_session::session_origin_e::attached_existing, 230, "")};
    const gamescope_source::source_selection_request_t request {
      .required_render_node = "/dev/dri/renderD128",
    };
    const auto selected {gamescope_source::select_gamescope_source({source}, request)};
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected->render_node.empty());

    source.render_node = "/dev/dri/renderD129";
    const auto mismatched {gamescope_source::select_gamescope_source({source}, request)};
    EXPECT_FALSE(mismatched.has_value());
    EXPECT_EQ(mismatched.error(), gamescope_source::source_error_e::unavailable);
  }

  /**
   * @brief Verify a Game Mode source published after an earlier request is selected on retry.
   */
  TEST(SteamOSVirtualSessionCore, SelectsGameModeSourceThatAppearsOnLaterRetry) {
    const auto unavailable {gamescope_source::select_gamescope_source({}, {})};
    ASSERT_FALSE(unavailable.has_value());
    EXPECT_EQ(unavailable.error(), gamescope_source::source_error_e::unavailable);

    const std::vector<gamescope_source::gamescope_source_t> later_sources {
      verified_source(steamos_virtual_session::session_origin_e::attached_existing, 210),
    };
    const auto selected {gamescope_source::select_gamescope_source(later_sources, {})};
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->origin, steamos_virtual_session::session_origin_e::attached_existing);
    EXPECT_EQ(selected->producer_pid, 210);
    EXPECT_EQ(selected->producer_start_time, 3210U);
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
   * @brief Verify capability-restricted Gamescope identification requires both procfs fields.
   */
  TEST(SteamOSVirtualSessionCore, ValidatesCapabilityRestrictedGamescopeCommandIdentity) {
    EXPECT_TRUE(gamescope_source::has_gamescope_command_identity("gamescope\0--backend\0headless", "gamescope-wl"));
    EXPECT_TRUE(gamescope_source::has_gamescope_command_identity("/usr/bin/gamescope-wl\0--backend\0headless", "gamescope"));
    EXPECT_FALSE(gamescope_source::has_gamescope_command_identity("gamescope-helper\0", "gamescope"));
    EXPECT_FALSE(gamescope_source::has_gamescope_command_identity("gamescope\0", "unrelated"));
  }

  /**
   * @brief Verify stock SteamOS Game Mode is accepted without a `--steam` argument.
   */
  TEST(SteamOSVirtualSessionCore, ValidatesSteamOSGameModeSessionIdentity) {
    constexpr char stock_command_data[] {"gamescope\0-e\0-R\0/run/user/1000/gamescope/startup.socket"};
    const std::string stock_command {stock_command_data, sizeof(stock_command_data) - 1};
    const std::string stock_cgroup {"0::/user.slice/user-1000.slice/user@1000.service/session.slice/gamescope-session.service\n"};
    EXPECT_TRUE(gamescope_source::has_game_mode_session_identity(stock_command, stock_cgroup));
    EXPECT_TRUE(gamescope_source::has_game_mode_session_identity("gamescope --steam", "0::/user.slice/steam-session.scope\n"));
    EXPECT_FALSE(gamescope_source::has_game_mode_session_identity(stock_command, "0::/user.slice/not-gamescope-session.service\n"));
    EXPECT_FALSE(gamescope_source::has_game_mode_session_identity("gamescope", "0::/user.slice/steam-session.scope\n"));
    EXPECT_FALSE(gamescope_source::has_game_mode_session_identity("gamescope --steam", "0::/user.slice/user@1000.service/app.slice/steamshine.service\n"));
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

    const steam_session::target_session_t vendor_target {
      .gamescope_pid = 100,
      .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/gamescope-session.service\n",
    };
    const std::vector<process_record_t> vendor_steam {
      {.pid = 451, .parent_pid = 1, .executable_name = "steam", .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/steam-launcher.service\n"},
      {.pid = 452, .parent_pid = 451, .executable_name = "steamwebhelper", .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/steam-launcher.service\n"},
    };
    EXPECT_EQ(classify_instance_location(vendor_steam, vendor_target), instance_location_e::inside_target_gamescope);

    auto similar_vendor_steam {vendor_steam};
    similar_vendor_steam[0].cgroup = "0::/user.slice/session.slice/not-steam-launcher.service\n";
    EXPECT_EQ(classify_instance_location(similar_vendor_steam, vendor_target), instance_location_e::outside_target_gamescope);

    const target_session_t shared_cgroup_target {
      .gamescope_pid = 100,
      .cgroup = "user.slice/user-1000.slice/user@1000.service/app.slice",
    };
    const std::vector<process_record_t> shared_cgroup {
      {.pid = 501, .parent_pid = 1, .executable_name = "steam", .cgroup = shared_cgroup_target.cgroup},
    };
    EXPECT_EQ(classify_instance_location(shared_cgroup, shared_cgroup_target), instance_location_e::outside_target_gamescope);

    const std::vector<process_record_t> mixed {
      {.pid = 601, .parent_pid = 100, .executable_name = "steam"},
      {.pid = 602, .parent_pid = 1, .executable_name = "steamwebhelper", .xdg_runtime_directory = "/run/user/1000"},
    };
    EXPECT_EQ(classify_instance_location(mixed, target), instance_location_e::outside_target_gamescope);
  }

  /**
   * @brief Verify resident Steam endpoint selection checks UID, start time, and uniqueness.
   */
  TEST(SteamOSVirtualSessionCore, SelectsOnlyVerifiedResidentSteamEnvironment) {
    const steam_session::target_session_t target {
      .gamescope_pid = 100,
      .cgroup = "user.slice/gamescope-session.scope",
    };
    EXPECT_FALSE(steam_session::select_resident_environment({}, {}, 1000));
    const steam_session::process_record_t gamescope {
      .pid = 100,
      .uid = 1000,
      .parent_pid = 1,
      .start_time = 10,
      .executable_name = "gamescope",
      .cgroup = target.cgroup,
    };
    const steam_session::process_record_t steam {
      .pid = 101,
      .uid = 1000,
      .parent_pid = 100,
      .start_time = 20,
      .executable_name = "steam",
      .xdg_runtime_directory = "/run/user/1000",
      .wayland_display = "gamescope-0",
      .x11_display = ":27",
      .xauthority = "/run/user/1000/xauthority",
      .gamescope_wayland_display = "gamescope-0",
      .dbus_session_bus_address = "unix:path=/run/user/1000/bus",
      .xdg_session_type = "x11",
      .xdg_current_desktop = "gamescope",
      .cgroup = target.cgroup,
    };
    const auto selected {steam_session::select_resident_environment({gamescope, steam}, target, 1000)};
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->steam_pid, 101);
    EXPECT_EQ(selected->steam_start_time, 20U);
    EXPECT_EQ(selected->x11_display, ":27");
    EXPECT_EQ(selected->xdg_session_type, "x11");
    EXPECT_EQ(selected->xdg_current_desktop, "gamescope");

    auto vendor_gamescope {gamescope};
    vendor_gamescope.cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/gamescope-session.service\n";
    auto vendor_steam {steam};
    vendor_steam.parent_pid = 1;
    vendor_steam.cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/steam-launcher.service\n";
    const steam_session::target_session_t vendor_target {
      .gamescope_pid = 100,
      .cgroup = vendor_gamescope.cgroup,
    };
    const auto vendor_selected {steam_session::select_resident_environment({vendor_gamescope, vendor_steam}, vendor_target, 1000)};
    ASSERT_TRUE(vendor_selected);
    EXPECT_EQ(vendor_selected->steam_pid, 101);
    EXPECT_TRUE(vendor_selected->allows_authless_xwayland);
    EXPECT_FALSE(selected->allows_authless_xwayland);

    const steam_session::process_record_t stale_game_reaper {
      .pid = 103,
      .uid = 1000,
      .parent_pid = 1,
      .start_time = 30,
      .executable_name = "reaper",
      .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-game.scope\n",
    };
    const auto selected_with_stale_game {steam_session::select_resident_environment({gamescope, steam, stale_game_reaper}, target, 1000)};
    ASSERT_TRUE(selected_with_stale_game);
    EXPECT_EQ(selected_with_stale_game->steam_pid, 101);

    auto outside_steam {steam};
    outside_steam.pid = 104;
    outside_steam.cgroup = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-desktop.scope\n";
    EXPECT_FALSE(steam_session::select_resident_environment({gamescope, steam, outside_steam}, target, 1000));

    auto wrong_uid {steam};
    wrong_uid.uid = 1001;
    EXPECT_FALSE(steam_session::select_resident_environment({gamescope, wrong_uid}, target, 1000));
    auto reused_pid {steam};
    reused_pid.start_time = 0;
    EXPECT_FALSE(steam_session::select_resident_environment({gamescope, reused_pid}, target, 1000));
    auto duplicate {steam};
    duplicate.pid = 102;
    EXPECT_FALSE(steam_session::select_resident_environment({gamescope, steam, duplicate}, target, 1000));
  }

  /**
   * @brief Verify Steam command detection catches executable paths and URIs only.
   */
  TEST(SteamOSVirtualSessionCore, DetectsSteamCommandsWithoutMatchingSteamHelperNames) {
    EXPECT_TRUE(steam_session::command_references_steam("steam -tenfoot"));
    EXPECT_TRUE(steam_session::command_references_steam("/usr/bin/steam steam://open/bigpicture"));
    EXPECT_TRUE(steam_session::command_references_steam("xdg-open steam://open/bigpicture"));
    EXPECT_TRUE(steam_session::command_opens_big_picture("setsid steam steam://open/bigpicture"));
    EXPECT_FALSE(steam_session::command_opens_big_picture("setsid steam steam://close/bigpicture"));
    EXPECT_FALSE(steam_session::command_references_steam("steamwebhelper --type=renderer"));
    EXPECT_FALSE(steam_session::command_references_steam("/usr/bin/gamescope --steamcompmgr"));
  }

  /**
   * @brief Verify Desktop Steam migration fails closed unless one idle instance is proven.
   */
  TEST(SteamOSVirtualSessionCore, AssessesIdleDesktopSteamMigration) {
    const steam_session::process_record_t steam {
      .pid = 40,
      .uid = 1000,
      .parent_pid = 1,
      .start_time = 400,
      .executable_name = "steam",
      .executable_path = "/usr/bin/steam",
      .xdg_runtime_directory = "/run/user/1000",
      .wayland_display = "wayland-0",
      .dbus_session_bus_address = "unix:path=/run/user/1000/bus",
      .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam@autostart.service\n",
    };
    auto result {steam_session::assess_idle_desktop_migration({steam}, "/run/user/1000", "wayland-0", 1000)};
    EXPECT_EQ(result.result, steam_session::migration_idle_result_e::idle);
    EXPECT_EQ(result.steam_pid, 40);
    EXPECT_EQ(result.steam_start_time, 400U);

    auto reaper {steam};
    reaper.pid = 41;
    reaper.executable_name = "reaper";
    reaper.executable_path = "/usr/lib/steam/reaper";
    reaper.cgroup = "0::/user.slice/app-steam-123.scope\n";
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam, reaper}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::idle);

    auto game {reaper};
    game.executable_name = "game";
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam, game}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::active_game);

    auto alternate_game_scope {game};
    alternate_game_scope.cgroup = "0::/user.slice/steam-app-123.scope\n";
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam, alternate_game_scope}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::active_game);

    auto misleading_scope {game};
    misleading_scope.cgroup = "0::/user.slice/not-app-steam-123.scope\n";
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam, misleading_scope}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::idle);

    auto duplicate {steam};
    duplicate.pid = 42;
    duplicate.start_time = 420;
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam, duplicate}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::unknown);

    auto unreadable {steam};
    unreadable.metadata_readable = false;
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({unreadable}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::unknown);
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({steam}, "/run/user/1000", "wayland-1", 1000).result, steam_session::migration_idle_result_e::unknown);
    EXPECT_EQ(steam_session::assess_idle_desktop_migration({}, "/run/user/1000", "wayland-0", 1000).result, steam_session::migration_idle_result_e::unknown);
  }

  /**
   * @brief Verify stock Game Mode handoff requires one idle vendor Steam instance.
   */
  TEST(SteamOSVirtualSessionCore, AssessesIdleStockSteamSession) {
    const steam_session::target_session_t target {
      .gamescope_pid = 50,
      .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/gamescope-session.service\n",
    };
    const steam_session::process_record_t steam {
      .pid = 51,
      .uid = 1000,
      .parent_pid = 1,
      .start_time = 510,
      .executable_name = "steam",
      .executable_path = "/usr/bin/steam",
      .cgroup = "0::/user.slice/user-1000.slice/user@1000.service/session.slice/steam-launcher.service\n",
    };
    auto result {steam_session::assess_idle_stock_session({steam}, target, 1000)};
    EXPECT_EQ(result.result, steam_session::migration_idle_result_e::idle);
    EXPECT_EQ(result.steam_pid, 51);
    EXPECT_EQ(result.steam_start_time, 510U);

    auto reaper {steam};
    reaper.pid = 52;
    reaper.executable_name = "reaper";
    reaper.cgroup = "0::/user.slice/app-steam-123.scope\n";
    EXPECT_EQ(steam_session::assess_idle_stock_session({steam, reaper}, target, 1000).result, steam_session::migration_idle_result_e::idle);

    auto game {reaper};
    game.executable_name = "game";
    EXPECT_EQ(steam_session::assess_idle_stock_session({steam, game}, target, 1000).result, steam_session::migration_idle_result_e::active_game);

    auto unreadable_game {game};
    unreadable_game.metadata_readable = false;
    EXPECT_EQ(steam_session::assess_idle_stock_session({steam, unreadable_game}, target, 1000).result, steam_session::migration_idle_result_e::unknown);

    auto duplicate {steam};
    duplicate.pid = 53;
    EXPECT_EQ(steam_session::assess_idle_stock_session({steam, duplicate}, target, 1000).result, steam_session::migration_idle_result_e::unknown);

    auto desktop_steam {steam};
    desktop_steam.cgroup = "0::/user.slice/app-steam@autostart.service\n";
    EXPECT_EQ(steam_session::assess_idle_stock_session({desktop_steam}, target, 1000).result, steam_session::migration_idle_result_e::unknown);

    auto non_vendor_target {target};
    non_vendor_target.cgroup = "0::/user.slice/app-gamescope.scope\n";
    EXPECT_EQ(steam_session::assess_idle_stock_session({steam}, non_vendor_target, 1000).result, steam_session::migration_idle_result_e::unknown);
  }

  /**
   * @brief Verify graceful shutdown observations reject PID reuse and report timeout.
   */
  TEST(SteamOSVirtualSessionCore, ClassifiesSteamShutdownObservations) {
    using steamos_virtual_session::classify_steam_shutdown_observation;
    using steamos_virtual_session::steam_migration_state_e;

    EXPECT_EQ(classify_steam_shutdown_observation(400, 400, false), steam_migration_state_e::shutting_down);
    EXPECT_EQ(classify_steam_shutdown_observation(400, std::nullopt, false), steam_migration_state_e::migrated);
    EXPECT_EQ(classify_steam_shutdown_observation(400, 401, false), steam_migration_state_e::blocked_unknown);
    EXPECT_EQ(classify_steam_shutdown_observation(400, 400, true), steam_migration_state_e::shutdown_timeout);
    EXPECT_EQ(classify_steam_shutdown_observation(0, 400, false), steam_migration_state_e::blocked_unknown);
  }

  /**
   * @brief Verify only the canonical close URI is treated as a Big Picture teardown request.
   */
  TEST(SteamOSVirtualSessionCore, DetectsSteamBigPictureCloseCommands) {
    EXPECT_TRUE(steam_session::command_closes_big_picture("setsid steam steam://close/bigpicture"));
    EXPECT_TRUE(steam_session::command_closes_big_picture("xdg-open steam://close/bigpicture"));
    EXPECT_FALSE(steam_session::command_closes_big_picture("setsid steam steam://open/bigpicture"));
    EXPECT_FALSE(steam_session::command_closes_big_picture("steam -shutdown"));
  }

  /**
   * @brief Verify each PipeWire consumer retains a complete, independent source identity.
   */
  TEST(SteamOSVirtualSessionCore, ComparesPipeWireConsumerDescriptorsWithoutSharingFds) {
    const pipewire_capture::stream_descriptor_t remote {
      .connected_core_fd = 11,
      .node_id = 23,
      .object_serial = 45,
      .producer_pid = 67,
      .producer_start_time = 89,
      .render_node = "/dev/dri/renderD128",
      .source_label = "Gamescope Video/Source",
    };
    auto local {remote};
    local.connected_core_fd = 12;
    EXPECT_TRUE(pipewire_capture::has_verified_source_identity(remote));
    EXPECT_TRUE(pipewire_capture::has_verified_source_identity(local));
    EXPECT_TRUE(pipewire_capture::refers_to_same_source(remote, local));
    EXPECT_TRUE(pipewire_capture::matches_selected_render_node("", remote.render_node));
    EXPECT_TRUE(pipewire_capture::matches_selected_render_node(remote.render_node, remote.render_node));
    EXPECT_FALSE(pipewire_capture::matches_selected_render_node("/dev/dri/renderD129", remote.render_node));

    local.producer_start_time++;
    EXPECT_FALSE(pipewire_capture::refers_to_same_source(remote, local));
  }

  /**
   * @brief Verify direct Gamescope capture never derives GPU capabilities from Desktop Wayland.
   */
  TEST(SteamOSVirtualSessionCore, SelectsVerifiedRenderNodeForGamescopeDmabufDiscovery) {
    const auto desktop {pipewire_capture::desktop_dmabuf_device()};
    EXPECT_EQ(desktop.origin, pipewire_capture::dmabuf_device_origin_e::desktop_wayland);
    EXPECT_TRUE(desktop.render_node.empty());

    const auto direct {pipewire_capture::verified_render_node_dmabuf_device("/dev/dri/renderD128")};
    ASSERT_TRUE(direct);
    EXPECT_EQ(direct->origin, pipewire_capture::dmabuf_device_origin_e::verified_render_node);
    EXPECT_EQ(direct->render_node, "/dev/dri/renderD128");
  }

  /**
   * @brief Verify untrusted or non-render paths cannot become direct DMA-BUF endpoints.
   */
  TEST(SteamOSVirtualSessionCore, RejectsInvalidGamescopeDmabufRenderNodes) {
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device(""));
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device("renderD128"));
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device("/dev/dri/card0"));
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device("/dev/dri/renderD"));
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device("/dev/dri/renderD128/child"));
    EXPECT_FALSE(pipewire_capture::verified_render_node_dmabuf_device("/tmp/renderD128"));
  }

  /**
   * @brief Verify PipeWire maximum-frame-rate negotiation intersects KWin's positive range.
   */
  TEST(SteamOSVirtualSessionCore, AdvertisesPositivePipeWireMaximumFrameRateRange) {
    const auto range {pipewire_capture::max_framerate_range(60000, 1001)};

    EXPECT_EQ(range.preferred.numerator, 60000U);
    EXPECT_EQ(range.preferred.denominator, 1001U);
    EXPECT_EQ(range.minimum.numerator, 1U);
    EXPECT_EQ(range.minimum.denominator, 1U);
    EXPECT_EQ(range.maximum.numerator, 60000U);
    EXPECT_EQ(range.maximum.denominator, 1001U);
  }

  /**
   * @brief Verify an unspecified frame rate remains an unconstrained zero range.
   */
  TEST(SteamOSVirtualSessionCore, KeepsUnspecifiedPipeWireMaximumFrameRateRangeEmpty) {
    const auto range {pipewire_capture::max_framerate_range(0, 1)};

    EXPECT_EQ(range.preferred.numerator, 0U);
    EXPECT_EQ(range.minimum.numerator, 0U);
    EXPECT_EQ(range.maximum.numerator, 0U);
  }

  /**
   * @brief Verify a terminal PipeWire stream cannot become a dummy-only capture session.
   */
  TEST(SteamOSVirtualSessionCore, RejectsPipeWireNegotiationFailureBeforeFirstFormat) {
    using pipewire_capture::negotiation_state_e;

    EXPECT_EQ(pipewire_capture::negotiation_state(false, 0, 0), negotiation_state_e::pending);
    EXPECT_EQ(pipewire_capture::negotiation_state(false, 3440, 1440), negotiation_state_e::complete);
    EXPECT_EQ(pipewire_capture::negotiation_state(true, 0, 0), negotiation_state_e::failed);
    EXPECT_EQ(pipewire_capture::negotiation_state(true, 3440, 1440), negotiation_state_e::failed);
  }

  /**
   * @brief Verify capability probes preserve an edge-triggered producer's initial frame.
   */
  TEST(SteamOSVirtualSessionCore, DefersEdgeTriggeredPipeWireStreamDuringEncoderProbe) {
    EXPECT_FALSE(pipewire_capture::should_start_stream_during_initialization(true, false));
    EXPECT_TRUE(pipewire_capture::should_start_stream_during_initialization(true, true));
    EXPECT_TRUE(pipewire_capture::should_start_stream_during_initialization(false, false));
    EXPECT_TRUE(pipewire_capture::should_start_stream_during_initialization(false, true));
  }

  /**
   * @brief Verify first-frame starvation fails only a retained productive source.
   */
  TEST(SteamOSVirtualSessionCore, FailsClosedAfterRetainedFirstFrameTimeout) {
    using namespace std::chrono_literals;

    EXPECT_FALSE(pipewire_capture::retained_first_frame_timeout_expired(false, false, 10s, 2s));
    EXPECT_FALSE(pipewire_capture::retained_first_frame_timeout_expired(true, false, 1999ms, 2s));
    EXPECT_TRUE(pipewire_capture::retained_first_frame_timeout_expired(true, false, 2s, 2s));
    EXPECT_TRUE(pipewire_capture::retained_first_frame_timeout_expired(true, false, 3s, 2s));
    EXPECT_FALSE(pipewire_capture::retained_first_frame_timeout_expired(true, true, 10s, 2s));
  }

  /**
   * @brief Verify endpoint refresh accepts a later complete desktop environment only.
   */
  TEST(SteamOSVirtualSessionCore, RefreshesHostDesktopEndpointWhenItBecomesAvailable) {
    const host_desktop_endpoint::endpoint_t empty {};
    const host_desktop_endpoint::endpoint_t complete {
      .xdg_runtime_directory = "/run/user/1000",
      .wayland_display = "wayland-0",
      .x11_display = ":0",
    };
    const host_desktop_endpoint::endpoint_t partial {
      .xdg_runtime_directory = "/run/user/1000",
    };

    EXPECT_TRUE(host_desktop_endpoint::should_refresh(empty, complete));
    EXPECT_FALSE(host_desktop_endpoint::should_refresh(empty, partial));
    EXPECT_FALSE(host_desktop_endpoint::should_refresh(complete, complete));

    auto changed {complete};
    changed.wayland_display = "wayland-1";
    EXPECT_TRUE(host_desktop_endpoint::should_refresh(complete, changed));

    auto next {complete};
    ++next.generation;
    EXPECT_EQ(next.generation, complete.generation + 1);
  }

  /**
   * @brief Verify desktop endpoint discovery accepts one complete endpoint.
   */
  TEST(SteamOSVirtualSessionCore, SelectsUniqueHostDesktopEndpoint) {
    const host_desktop_endpoint::endpoint_t endpoint {
      .xdg_runtime_directory = "/run/user/1000",
      .wayland_display = "wayland-0",
      .x11_display = ":0",
    };

    const auto selected {host_desktop_endpoint::select_unique_endpoint({endpoint, endpoint})};

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->xdg_runtime_directory, endpoint.xdg_runtime_directory);
    EXPECT_EQ(selected->wayland_display, endpoint.wayland_display);
    EXPECT_EQ(selected->x11_display, endpoint.x11_display);
  }

  /**
   * @brief Verify desktop endpoint discovery rejects ambiguity and partial data.
   */
  TEST(SteamOSVirtualSessionCore, RejectsAmbiguousHostDesktopEndpoints) {
    const host_desktop_endpoint::endpoint_t first {
      .xdg_runtime_directory = "/run/user/1000",
      .wayland_display = "wayland-0",
      .x11_display = ":0",
    };
    auto second {first};
    second.wayland_display = "wayland-1";
    const host_desktop_endpoint::endpoint_t partial {
      .wayland_display = "wayland-0",
      .x11_display = ":0",
    };

    EXPECT_FALSE(host_desktop_endpoint::select_unique_endpoint({first, second}).has_value());
    EXPECT_FALSE(host_desktop_endpoint::select_unique_endpoint({partial}).has_value());
  }
}  // namespace
