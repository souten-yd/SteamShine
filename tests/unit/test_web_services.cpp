/**
 * @file tests/unit/test_web_services.cpp
 * @brief Tests for pure shared Web service validation.
 */

// test imports
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/file_handler.h>
#include <src/utility.h>
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
  EXPECT_TRUE(snapshot.contains("presentation"));
  EXPECT_TRUE(snapshot.contains("local_presenter_active"));
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
  EXPECT_TRUE(snapshot.contains("encoder_queue_current"));
  EXPECT_TRUE(snapshot.contains("encoder_queue_max"));
  EXPECT_TRUE(snapshot.contains("network_queue_bytes"));
  EXPECT_TRUE(snapshot.contains("network_queue_frames"));
  EXPECT_TRUE(snapshot.contains("socket_outq_bytes"));
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
