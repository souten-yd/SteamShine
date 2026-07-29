/**
 * @file tests/unit/test_web_services.cpp
 * @brief Tests for pure shared Web service validation.
 */

// test imports
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/file_handler.h>
#include <src/utility.h>
#include <src/video.h>
#include <src/web_services.h>

namespace {
  /**
   * @brief In-memory paired-client backend for shared-service tests.
   */
  class FakePairingClientBackend final: public web::PairingClientBackend {
  public:
    /** @copydoc web::PairingClientBackend::submit_pin */
    bool submit_pin(const std::string_view pin, const std::string_view client_name) override {
      if (pin != "1234") {
        return false;
      }
      clients_.emplace("test-client", nlohmann::json {{"uuid", "test-client"}, {"name", client_name}});
      return true;
    }

    /** @copydoc web::PairingClientBackend::list_clients */
    nlohmann::json list_clients() const override {
      nlohmann::json clients = nlohmann::json::array();
      for (const auto &entry : clients_) {
        clients.push_back(entry.second);
      }
      return clients;
    }

    /** @copydoc web::PairingClientBackend::certificate_for_uuid */
    std::string certificate_for_uuid(const std::string_view uuid) const override {
      return clients_.contains(std::string {uuid}) ? "test-certificate" : "";
    }

    /** @copydoc web::PairingClientBackend::revoke_client */
    bool revoke_client(const std::string_view uuid) override {
      return clients_.erase(std::string {uuid}) > 0;
    }

  private:
    std::map<std::string, nlohmann::json, std::less<>> clients_;  ///< Paired clients indexed by stable test UUID.
  };
}  // namespace

/**
 * @brief Validate the exact Moonlight PIN boundary accepted by Web services.
 */
TEST(WebServicesTest, ValidatesFourDigitPins) {
  EXPECT_TRUE(web::is_valid_pin("0000"));
  EXPECT_TRUE(web::is_valid_pin("1234"));
  EXPECT_FALSE(web::is_valid_pin("123"));
  EXPECT_FALSE(web::is_valid_pin("12345"));
  EXPECT_FALSE(web::is_valid_pin("12a4"));
  EXPECT_FALSE(web::is_valid_pin("１２３４"));
}

/**
 * @brief Verify that logout and global invalidation immediately revoke sessions.
 */
TEST(WebServicesTest, InvalidatesSessions) {
  const auto original_username = config::sunshine.username;
  const auto original_password = config::sunshine.password;
  const auto original_salt = config::sunshine.salt;
  config::sunshine.username = "web-services-test";
  config::sunshine.salt = "web-services-test-salt";
  config::sunshine.password = util::hex(crypto::hash("web-services-test-password" + config::sunshine.salt)).to_string();

  web::CredentialService credentials;
  web::SessionService sessions;
  const auto first_session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(first_session.has_value());
  EXPECT_TRUE(sessions.validate(first_session->id).has_value());
  EXPECT_TRUE(sessions.validate_csrf(first_session->id, first_session->csrf_token));
  EXPECT_FALSE(sessions.validate_csrf(first_session->id, "invalid-token"));

  sessions.logout(first_session->id);
  EXPECT_FALSE(sessions.validate(first_session->id).has_value());

  const auto second_session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(second_session.has_value());
  sessions.invalidate_all();
  EXPECT_FALSE(sessions.validate(second_session->id).has_value());

  config::sunshine.username = original_username;
  config::sunshine.password = original_password;
  config::sunshine.salt = original_salt;
}

/**
 * @brief Verify that expired sessions are purged before they can authorize a request.
 */
TEST(WebServicesTest, RejectsExpiredSessions) {
  const auto original_username = config::sunshine.username;
  const auto original_password = config::sunshine.password;
  const auto original_salt = config::sunshine.salt;
  config::sunshine.username = "web-services-test";
  config::sunshine.salt = "web-services-test-salt";
  config::sunshine.password = util::hex(crypto::hash("web-services-test-password" + config::sunshine.salt)).to_string();

  web::CredentialService credentials;
  web::SessionService sessions {std::chrono::seconds::zero()};
  const auto session = sessions.login(credentials, "web-services-test", "web-services-test-password");
  ASSERT_TRUE(session.has_value());
  EXPECT_FALSE(sessions.validate(session->id).has_value());

  config::sunshine.username = original_username;
  config::sunshine.password = original_password;
  config::sunshine.salt = original_salt;
}

/**
 * @brief Verify pairing and revocation share one client state backend.
 */
TEST(WebServicesTest, SharesPairingAndClientState) {
  const auto backend = std::make_shared<FakePairingClientBackend>();
  web::PairingService pairing {backend};
  web::ClientService clients {backend};

  EXPECT_FALSE(pairing.submit_pin("0000", "Moonlight test").success);
  EXPECT_TRUE(clients.list().empty());

  EXPECT_TRUE(pairing.submit_pin("1234", "Moonlight test").success);
  const auto paired_clients = clients.list();
  ASSERT_EQ(paired_clients.size(), 1);
  EXPECT_EQ(paired_clients.at(0).at("name"), "Moonlight test");

  EXPECT_TRUE(clients.revoke("test-client").success);
  EXPECT_TRUE(clients.list().empty());
  EXPECT_FALSE(clients.revoke("test-client").success);
}

/**
 * @brief Verify the SteamShine-only policy writer validates modes and preserves unrelated configuration.
 */
TEST(WebServicesTest, PersistsVirtualDisplayPolicyForRestart) {
  namespace fs = std::filesystem;
  const auto original_config_file = config::sunshine.config_file;
  const auto temporary_config = fs::temp_directory_path() / "steamshine-web-services-virtual-display.conf";
  ASSERT_EQ(file_handler::write_file(temporary_config.string().c_str(), "port = 47989\ncustom_option = preserved\n"), 0);
  config::sunshine.config_file = temporary_config.string();

  web::ConfigurationService configuration;
  const auto invalid = configuration.save_virtual_display(true, "FORCE", "auto", "auto", true, 0);
  EXPECT_FALSE(invalid.success);
  EXPECT_EQ(invalid.code, "invalid_virtual_display_mode");

  const auto invalid_source = configuration.save_virtual_display(true, "force", "resident", "auto", true, 0);
  EXPECT_FALSE(invalid_source.success);
  EXPECT_EQ(invalid_source.code, "invalid_steamos_session_source");

  const auto invalid_presentation = configuration.save_virtual_display(true, "force", "owned_private", "desktop", false, 0);
  EXPECT_FALSE(invalid_presentation.success);
  EXPECT_EQ(invalid_presentation.code, "invalid_steamos_local_presentation");

  const auto invalid_pid = configuration.save_virtual_display(true, "force", "owned_private", "mirror", false, -1);
  EXPECT_FALSE(invalid_pid.success);
  EXPECT_EQ(invalid_pid.code, "invalid_steamos_existing_gamescope_pid");

  const auto saved = configuration.save_virtual_display(true, "force", "owned_private", "mirror", false, 4321);
  EXPECT_TRUE(saved.success);
  EXPECT_EQ(saved.code, "restart_required");
  const auto persisted = file_handler::read_file(temporary_config.string().c_str());
  EXPECT_NE(persisted.find("custom_option = preserved"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_virtual_display_enabled = enabled"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_virtual_display_mode = force"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_session_source = owned_private"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_local_presentation = mirror"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_keep_session_alive = disabled"), std::string::npos);
  EXPECT_NE(persisted.find("steamos_existing_gamescope_pid = 4321"), std::string::npos);
  const auto snapshot = configuration.snapshot();
  EXPECT_EQ(snapshot.at("steamos_virtual_display_enabled"), "enabled");
  EXPECT_EQ(snapshot.at("steamos_virtual_display_mode"), "force");
  EXPECT_EQ(snapshot.at("steamos_session_source"), "owned_private");
  EXPECT_EQ(snapshot.at("steamos_local_presentation"), "mirror");
  EXPECT_EQ(snapshot.at("steamos_keep_session_alive"), "disabled");
  EXPECT_EQ(snapshot.at("steamos_existing_gamescope_pid"), "4321");

  config::sunshine.config_file = original_config_file;
  fs::remove(temporary_config);
}

/**
 * @brief Verify the authenticated status payload includes PipeWire and ownership diagnostics.
 */
TEST(WebServicesTest, StatusSnapshotIncludesPipeWireDiagnostics) {
  web::StatusSnapshotService status;
  const auto snapshot = status.snapshot();

  EXPECT_EQ(snapshot.at("service_binary_commit"), PROJECT_VERSION_COMMIT);
  EXPECT_EQ(snapshot.at("service_config_path"), config::sunshine.config_file);
  EXPECT_EQ(snapshot.at("service_launch_mode"), "manual");
  ASSERT_TRUE(snapshot.contains("stream_negotiation"));
  const auto &negotiation = snapshot.at("stream_negotiation");
  EXPECT_EQ(negotiation.at("schema_version"), 1);
  EXPECT_EQ(negotiation.at("poll_interval_ms"), 2000);
  EXPECT_FALSE(negotiation.at("available"));
  EXPECT_TRUE(negotiation.contains("requested"));
  EXPECT_TRUE(negotiation.contains("selected"));
  EXPECT_TRUE(negotiation.contains("active"));
  EXPECT_TRUE(negotiation.contains("observed"));
  EXPECT_TRUE(negotiation.contains("fallback_reasons"));
  EXPECT_TRUE(negotiation.at("requested").contains("client_codec_mask"));
  EXPECT_TRUE(negotiation.at("requested").contains("capability_signature"));
  EXPECT_TRUE(negotiation.at("selected").contains("encode_geometry"));
  EXPECT_TRUE(negotiation.at("selected").contains("reason"));
  EXPECT_TRUE(negotiation.at("selected").contains("profile_selection_reason"));
  EXPECT_TRUE(negotiation.at("active").contains("runtime_rate_update_supported"));
  EXPECT_TRUE(negotiation.at("observed").contains("network_age_p99_ms"));
  EXPECT_EQ(snapshot.at("codec_policy"), "auto");
  EXPECT_EQ(snapshot.at("codec_fallback"), "strict");
  EXPECT_FALSE(snapshot.at("codec_allow_software"));
  ASSERT_TRUE(snapshot.contains("codec_state"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("requested"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("selected"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("active"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("profile"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("bit_depth"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("backend"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("hardware"));
  EXPECT_TRUE(snapshot.at("codec_state").contains("reason"));
  EXPECT_TRUE(snapshot.contains("pipewire_runtime"));
  EXPECT_TRUE(snapshot.contains("virtual_display_origin"));
  EXPECT_TRUE(snapshot.contains("virtual_display_process_owned"));
  EXPECT_TRUE(snapshot.contains("virtual_display_runtime_owned"));
  EXPECT_TRUE(snapshot.contains("virtual_display_source_description"));
  EXPECT_TRUE(snapshot.contains("virtual_display_source_executable"));
  EXPECT_TRUE(snapshot.contains("steam_location"));
  EXPECT_TRUE(snapshot.contains("migration_required"));
  EXPECT_TRUE(snapshot.contains("app_launch_rejected_reason"));
  EXPECT_TRUE(snapshot.contains("app_launch_rejected_message"));
  EXPECT_TRUE(snapshot.contains("capture_selection_reason"));
  EXPECT_TRUE(snapshot.contains("steamshine_hdr_policy"));
  ASSERT_TRUE(snapshot.contains("hdr_state"));
  const auto &hdr_state = snapshot.at("hdr_state");
  EXPECT_TRUE(hdr_state.contains("requested"));
  EXPECT_TRUE(hdr_state.contains("client_capable"));
  EXPECT_TRUE(hdr_state.contains("selected"));
  EXPECT_TRUE(hdr_state.contains("active"));
  EXPECT_TRUE(hdr_state.contains("bit_depth"));
  EXPECT_TRUE(hdr_state.contains("primaries"));
  EXPECT_TRUE(hdr_state.contains("transfer"));
  EXPECT_TRUE(hdr_state.contains("matrix"));
  EXPECT_TRUE(hdr_state.contains("range"));
  EXPECT_TRUE(hdr_state.contains("reason"));
  EXPECT_TRUE(snapshot.contains("presentation"));
  EXPECT_TRUE(snapshot.contains("local_presenter_active"));
  EXPECT_TRUE(snapshot.contains("requested_stream_width"));
  EXPECT_TRUE(snapshot.contains("requested_stream_height"));
  EXPECT_TRUE(snapshot.at("requested_stream_refresh").contains("numerator"));
  EXPECT_TRUE(snapshot.contains("selected_stream_width"));
  EXPECT_TRUE(snapshot.contains("selected_stream_height"));
  EXPECT_TRUE(snapshot.at("selected_stream_refresh").contains("denominator"));
  EXPECT_TRUE(snapshot.contains("stream_geometry_reason"));
  EXPECT_TRUE(snapshot.contains("capture_width"));
  EXPECT_TRUE(snapshot.contains("capture_height"));
  EXPECT_TRUE(snapshot.at("content_rectangle").contains("width"));
  EXPECT_TRUE(snapshot.contains("input_events_received"));
  EXPECT_TRUE(snapshot.contains("input_events_injected"));
  EXPECT_TRUE(snapshot.contains("input_motion_coalesced"));
  EXPECT_TRUE(snapshot.contains("input_motion_dropped"));
  EXPECT_TRUE(snapshot.contains("input_route_target"));
  EXPECT_TRUE(snapshot.contains("input_route_error"));
  EXPECT_TRUE(snapshot.contains("input_queue_current"));
  EXPECT_TRUE(snapshot.contains("input_queue_max"));
  ASSERT_TRUE(snapshot.contains("input_queue_age_ms"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("count"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("average"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("p50"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("p95"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("p99"));
  EXPECT_TRUE(snapshot.at("input_queue_age_ms").contains("max"));
  EXPECT_TRUE(snapshot.contains("capture_queue_current"));
  EXPECT_TRUE(snapshot.contains("capture_queue_max"));
  EXPECT_TRUE(snapshot.contains("pipewire_buffers_received"));
  EXPECT_TRUE(snapshot.contains("pipewire_buffers_replaced"));
  EXPECT_TRUE(snapshot.contains("encoder_queue_current"));
  EXPECT_TRUE(snapshot.contains("encoder_queue_max"));
  EXPECT_TRUE(snapshot.contains("capture_deadline_misses"));
  EXPECT_TRUE(snapshot.contains("encoded_unique_frames"));
  EXPECT_TRUE(snapshot.contains("encoded_duplicate_frames"));
  EXPECT_TRUE(snapshot.contains("duplicate_run_max"));
  EXPECT_TRUE(snapshot.contains("pipewire_buffers_received"));
  EXPECT_TRUE(snapshot.contains("pipewire_unique_frames"));
  EXPECT_TRUE(snapshot.contains("pipewire_redundant_pts"));
  EXPECT_TRUE(snapshot.contains("pipewire_no_damage_frames"));
  EXPECT_TRUE(snapshot.contains("pipewire_queue_overflows"));
  EXPECT_TRUE(snapshot.at("requested_fps").contains("numerator"));
  EXPECT_TRUE(snapshot.at("negotiated_fps").contains("denominator"));
  EXPECT_TRUE(snapshot.at("negotiated_max_fps").contains("numerator"));
  EXPECT_TRUE(snapshot.contains("observed_source_fps"));
  EXPECT_TRUE(snapshot.contains("observed_encode_fps"));
  EXPECT_TRUE(snapshot.contains("output_status_reason"));
  EXPECT_TRUE(snapshot.at("source_interarrival_ms").contains("p99"));
  EXPECT_TRUE(snapshot.at("encode_interarrival_ms").contains("p99"));
  EXPECT_TRUE(snapshot.contains("network_queue_bytes"));
  EXPECT_TRUE(snapshot.contains("network_queue_frames"));
  EXPECT_TRUE(snapshot.contains("socket_outq_bytes"));
  ASSERT_TRUE(snapshot.contains("adaptive_bitrate"));
  const auto &adaptive {snapshot.at("adaptive_bitrate")};
  EXPECT_TRUE(adaptive.contains("minimum_kbps"));
  EXPECT_TRUE(adaptive.contains("initial_kbps"));
  EXPECT_TRUE(adaptive.contains("target_kbps"));
  EXPECT_TRUE(adaptive.contains("active_kbps"));
  EXPECT_TRUE(adaptive.contains("maximum_kbps"));
  EXPECT_TRUE(adaptive.contains("peak_kbps"));
  EXPECT_TRUE(adaptive.contains("vbv_kbits"));
  EXPECT_TRUE(adaptive.contains("actual_video_kbps"));
  EXPECT_TRUE(adaptive.contains("learned_next_kbps"));
  EXPECT_TRUE(adaptive.contains("state"));
  EXPECT_TRUE(adaptive.contains("reason"));
  EXPECT_TRUE(adaptive.contains("runtime_update_supported"));
  EXPECT_TRUE(adaptive.contains("feedback_samples"));
  EXPECT_TRUE(adaptive.contains("lost_packets"));
  EXPECT_TRUE(adaptive.contains("updates_applied"));
  EXPECT_TRUE(adaptive.contains("updates_unsupported"));
  EXPECT_TRUE(adaptive.contains("updates_failed"));
  EXPECT_TRUE(snapshot.contains("idr_requests"));
  EXPECT_TRUE(snapshot.contains("idr_emitted"));
  EXPECT_TRUE(snapshot.contains("idr_reason_client_request"));
  EXPECT_TRUE(snapshot.contains("idr_reason_recovery"));
  EXPECT_TRUE(snapshot.contains("idr_reason_periodic"));
  EXPECT_TRUE(snapshot.contains("idr_reason_reconnect"));
  EXPECT_TRUE(snapshot.at("frame_age_at_capture_ms").contains("p95"));
  EXPECT_TRUE(snapshot.at("frame_age_at_encode_ms").contains("p95"));
  EXPECT_TRUE(snapshot.at("frame_age_at_network_ms").contains("p95"));
  EXPECT_TRUE(snapshot.contains("local_presented_frames"));
  EXPECT_TRUE(snapshot.contains("local_dropped_frames"));
  EXPECT_TRUE(snapshot.contains("pipewire_remote"));
  EXPECT_TRUE(snapshot.contains("pipewire_node_id"));
  EXPECT_TRUE(snapshot.contains("pipewire_object_serial"));
  EXPECT_TRUE(snapshot.contains("pipewire_producer_pid"));
  EXPECT_EQ(snapshot.at("pipewire_node_id"), 0);
  EXPECT_EQ(snapshot.at("pipewire_object_serial"), 0);
  EXPECT_EQ(snapshot.at("pipewire_producer_pid"), -1);
  EXPECT_TRUE(snapshot.contains("requested_display_endpoint"));
  EXPECT_EQ(snapshot.at("active_display_endpoint_origin"), "none");
  EXPECT_FALSE(snapshot.at("active_display_endpoint_verified"));
  EXPECT_TRUE(snapshot.contains("active_display_endpoint_generation"));
  EXPECT_FALSE(snapshot.contains("active_display_endpoint_xauthority"));
  EXPECT_FALSE(snapshot.contains("active_display_endpoint_dbus_session_bus_address"));
}

/**
 * @brief Verify resident Gamescope discovery exposes only a bounded public schema.
 */
TEST(WebServicesTest, GamescopeSourceDiscoveryHasBoundedSchema) {
  web::ConfigurationService configuration;
  const auto snapshot = configuration.gamescope_sources();

  EXPECT_TRUE(snapshot.contains("available"));
  EXPECT_TRUE(snapshot.contains("error"));
  ASSERT_TRUE(snapshot.contains("sources"));
  EXPECT_TRUE(snapshot.at("sources").is_array());
}

/**
 * @brief Verify profile selection requires exact client, network, and capabilities.
 */
TEST(WebServicesTest, SelectsOnlyExactStreamCapabilityProfile) {
  namespace fs = std::filesystem;
  const auto path {fs::temp_directory_path() / "steamshine-stream-profile-selection.json"};
  fs::remove(path);
  web::StreamProfileService profiles {path};
  web::stream_profile_t profile;
  profile.client_id = "client-a";
  profile.network_class = "lan";
  profile.capability_signature = "h264-hevc-8bit";
  profile.codec_policy = "hevc";
  profile.active = true;
  ASSERT_TRUE(profiles.save(profile).success);

  const auto selected {profiles.select("client-a", "lan", "h264-hevc-8bit")};
  ASSERT_TRUE(selected.profile.has_value());
  EXPECT_EQ(selected.profile->codec_policy, "hevc");
  EXPECT_EQ(selected.reason, "exact_client_network_capability_match");
  EXPECT_FALSE(profiles.select("client-a", "wifi-5ghz", "h264-hevc-8bit").profile.has_value());
  const auto changed {profiles.select("client-a", "lan", "h264-only")};
  EXPECT_FALSE(changed.profile.has_value());
  EXPECT_EQ(changed.reason, "capability_signature_changed");
  EXPECT_TRUE(profiles.select_active("client-a", "h264-hevc-8bit").profile.has_value());
  EXPECT_EQ(profiles.select_active("client-a", "h264-only").reason, "capability_signature_changed");
  EXPECT_EQ(profiles.select_active("missing-client", "h264-hevc-8bit").reason, "active_network_profile_not_found");

  fs::remove(path);
}

/**
 * @brief Verify one active network wins and safe defaults never rewrite a requested codec.
 */
TEST(WebServicesTest, AppliesOnlyActiveSafeStreamProfileDefaults) {
  namespace fs = std::filesystem;
  const auto path {fs::temp_directory_path() / "steamshine-stream-profile-active.json"};
  fs::remove(path);
  web::StreamProfileService profiles {path};
  web::stream_profile_t lan;
  lan.client_id = "client-active";
  lan.network_class = "lan";
  lan.capability_signature = "v1-c1-d1-x0-h1";
  lan.active = true;
  ASSERT_TRUE(profiles.save(lan).success);
  auto wifi {lan};
  wifi.network_class = "wifi-5ghz";
  wifi.active = true;
  wifi.fps_policy = "custom";
  wifi.fps_ceiling = 60;
  wifi.codec_policy = "av1";
  wifi.hdr_policy = "off";
  wifi.bitrate_ceiling_kbps = 15000;
  wifi.learned_start_kbps = 12000;
  ASSERT_TRUE(profiles.save(wifi).success);
  EXPECT_FALSE(profiles.select("client-active", "lan", lan.capability_signature).profile->active);
  const auto selected {profiles.select_active("client-active", wifi.capability_signature)};
  ASSERT_TRUE(selected.profile.has_value());
  EXPECT_EQ(selected.profile->network_class, "wifi-5ghz");

  video::config_t config {};
  config.framerate = 120;
  config.framerateX100 = 12000;
  config.bitrate = 30000;
  config.videoFormat = 1;
  config.dynamicRange = 1;
  config.requestedDynamicRange = 1;
  const auto applied {web::apply_stream_profile(*selected.profile, config)};
  EXPECT_TRUE(applied.applied);
  EXPECT_EQ(config.framerate, 60);
  EXPECT_EQ(config.framerateX100, 0);
  EXPECT_EQ(config.bitrate, 12000);
  EXPECT_EQ(config.videoFormat, 1);
  EXPECT_EQ(config.dynamicRange, 0);
  EXPECT_NE(std::find(applied.fallback_reasons.begin(), applied.fallback_reasons.end(), "profile_codec_yielded_to_client_request"), applied.fallback_reasons.end());
  EXPECT_EQ(web::stream_capability_signature(config, true), "v1-c1-d1-x0-h1");
  EXPECT_TRUE(profiles.update_learned_start("client-active", "wifi-5ghz", wifi.capability_signature, 9000).success);
  EXPECT_EQ(profiles.select_active("client-active", wifi.capability_signature).profile->learned_start_kbps, 9000);

  auto capability_limited {wifi};
  capability_limited.hdr_policy = "require";
  capability_limited.codec_policy = "hevc";
  video::config_t sdr_request {};
  sdr_request.videoFormat = 1;
  sdr_request.dynamicRange = 0;
  sdr_request.requestedDynamicRange = 0;
  const auto preserved {web::apply_stream_profile(capability_limited, sdr_request)};
  EXPECT_FALSE(preserved.applied);
  EXPECT_NE(
    std::find(preserved.fallback_reasons.begin(), preserved.fallback_reasons.end(), "profile_hdr_requirement_yielded_to_client_capability"),
    preserved.fallback_reasons.end()
  );
  EXPECT_EQ(profiles.update_learned_start("client-active", "wifi-5ghz", "changed-capabilities", 8000).code, "profile_selection_changed");
  fs::remove(path);
}

/**
 * @brief Verify orientation, safe area, and policy values are bounded.
 */
TEST(WebServicesTest, ValidatesStreamProfileLayoutAndPolicies) {
  namespace fs = std::filesystem;
  const auto path {fs::temp_directory_path() / "steamshine-stream-profile-validation.json"};
  fs::remove(path);
  web::StreamProfileService profiles {path};
  web::stream_profile_t profile;
  profile.client_id = "portrait-client";
  profile.network_class = "wifi-6ghz";
  profile.capability_signature = "av1-main10-hdr";
  profile.geometry_policy = "virtual_fallback";
  profile.fps_policy = "custom";
  profile.fps_ceiling = 120;
  profile.codec_policy = "av1";
  profile.hdr_policy = "require";
  profile.bitrate_ceiling_kbps = 80000;
  profile.quality_preset = "quality";
  profile.orientation = "portrait";
  profile.safe_area_percent = 10;
  EXPECT_TRUE(profiles.save(profile).success);

  profile.safe_area_percent = 26;
  EXPECT_EQ(profiles.save(profile).code, "invalid_layout_policy");
  profile.safe_area_percent = 10;
  profile.codec_policy = "marketing-super-codec";
  EXPECT_EQ(profiles.save(profile).code, "invalid_codec_policy");
  profile.codec_policy = "av1";
  profile.fps_ceiling = 241;
  EXPECT_EQ(profiles.save(profile).code, "invalid_fps_policy");

  fs::remove(path);
}

/**
 * @brief Verify the persisted profile schema is bounded, private, reloadable, and resettable.
 */
TEST(WebServicesTest, PersistsAndResetsBoundedStreamProfiles) {
  namespace fs = std::filesystem;
  const auto path {fs::temp_directory_path() / "steamshine-stream-profile-persistence.json"};
  fs::remove(path);
  {
    web::StreamProfileService profiles {path};
    web::stream_profile_t profile;
    profile.client_id = "client-b";
    profile.network_class = "tailscale";
    profile.capability_signature = "h264-8bit";
    profile.learned_start_kbps = 12000;
    profile.active = true;
    ASSERT_TRUE(profiles.save(profile).success);
    const auto snapshot = profiles.snapshot();
    ASSERT_TRUE(snapshot.is_object()) << snapshot.dump();
    EXPECT_EQ(snapshot.at("schema_version"), 1);
    EXPECT_EQ(snapshot.at("maximum_profiles"), 64);
    ASSERT_EQ(snapshot.at("profiles").size(), 1);
  }
  const auto permissions {fs::status(path).permissions()};
  EXPECT_EQ(permissions & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);

  web::StreamProfileService reloaded {path};
  EXPECT_TRUE(reloaded.select_active("client-b", "h264-8bit").profile.has_value());
  EXPECT_EQ(reloaded.update_learned_start("client-b", "tailscale", "h264-8bit", 200001).code, "invalid_learned_start");
  EXPECT_TRUE(reloaded.reset("client-b", "tailscale").success);
  EXPECT_TRUE(reloaded.snapshot().at("profiles").empty());
  EXPECT_EQ(reloaded.reset("client-b", "tailscale").code, "profile_not_found");

  fs::remove(path);
}
