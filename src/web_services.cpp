/**
 * @file src/web_services.cpp
 * @brief Shared application service implementations for the Web UI frontends.
 */

// standard includes
#include <algorithm>
#include <vector>

// lib includes
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "nvhttp.h"
#include "process.h"
#include "rtsp.h"
#include "steamos_virtual_session.h"
#include "web_services.h"

namespace web {
  namespace {
    /**
     * @brief Number of random URL-safe characters in session and CSRF identifiers.
     */
    constexpr auto SESSION_TOKEN_SIZE = 48U;
    /**
     * @brief URL-safe alphabet used for opaque browser tokens.
     */
    constexpr std::string_view SESSION_TOKEN_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    /**
     * @brief Production paired-client backend backed by existing NVHTTP operations.
     */
    class NvHttpPairingClientBackend final: public PairingClientBackend {
    public:
      /** @copydoc PairingClientBackend::submit_pin */
      bool submit_pin(const std::string_view pin, const std::string_view client_name) override {
        return nvhttp::pin(std::string {pin}, std::string {client_name});
      }

      /** @copydoc PairingClientBackend::list_clients */
      nlohmann::json list_clients() const override {
        return nvhttp::get_all_clients();
      }

      /** @copydoc PairingClientBackend::certificate_for_uuid */
      std::string certificate_for_uuid(const std::string_view uuid) const override {
        return nvhttp::get_cert_by_uuid(uuid);
      }

      /** @copydoc PairingClientBackend::revoke_client */
      bool revoke_client(const std::string_view uuid) override {
        return nvhttp::unpair_client(uuid);
      }
    };

    /**
     * @brief Return the production backend used when callers do not inject one.
     *
     * @return Shared production paired-client backend.
     */
    std::shared_ptr<PairingClientBackend> default_pairing_client_backend() {
      static const auto backend = std::make_shared<NvHttpPairingClientBackend>();
      return backend;
    }
  }  // namespace

  bool is_valid_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char character) {
             return character >= '0' && character <= '9';
           });
  }

  bool CredentialService::is_configured() const {
    return !config::sunshine.username.empty();
  }

  bool CredentialService::verify(const std::string_view username, const std::string_view password) const {
    if (!is_configured() || username.empty() || password.empty()) {
      return false;
    }
    const auto hash = util::hex(crypto::hash(std::string {password} + config::sunshine.salt)).to_string();
    return boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password;
  }

  service_result_t CredentialService::save(const std::string_view current_username, const std::string_view current_password, const std::string_view new_username, const std::string_view new_password, const std::string_view confirm_password) const {
    if (new_username.empty() || new_username.size() > 64) {
      return {false, "invalid_username", "Username must contain between 1 and 64 characters."};
    }
    if (new_password.empty() || new_password != confirm_password) {
      return {false, "password_mismatch", "The passwords do not match."};
    }
    if (is_configured()) {
      if (!verify(current_username, current_password)) {
        return {false, "invalid_credentials", "The current credentials are invalid."};
      }
    }
    if (http::save_user_creds(config::sunshine.credentials_file, std::string {new_username}, std::string {new_password}) != 0 || http::reload_user_creds(config::sunshine.credentials_file) != 0) {
      return {false, "credential_persistence_failed", "The credential could not be saved."};
    }
    return {true, "credentials_saved", "Credentials saved."};
  }

  SessionService::SessionService(const std::chrono::steady_clock::duration lifetime):
      lifetime_ {lifetime} {}

  std::optional<session_t> SessionService::login(const CredentialService &credential_service, const std::string_view username, const std::string_view password) {
    if (!credential_service.verify(username, password)) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    session_t session {
      crypto::rand_alphabet(SESSION_TOKEN_SIZE, SESSION_TOKEN_ALPHABET),
      crypto::rand_alphabet(SESSION_TOKEN_SIZE, SESSION_TOKEN_ALPHABET),
      std::string {username},
    };
    std::scoped_lock lock(mutex_);
    purge_expired(now);
    sessions_.emplace(session.id, session_record_t {session.csrf_token, session.username, now + lifetime_});
    return session;
  }

  std::optional<session_t> SessionService::validate(const std::string_view session_id) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    purge_expired(now);
    const auto session = sessions_.find(std::string {session_id});
    if (session == sessions_.end()) {
      return std::nullopt;
    }
    return session_t {session->first, session->second.csrf_token, session->second.username};
  }

  bool SessionService::validate_csrf(const std::string_view session_id, const std::string_view csrf_token) {
    const auto session = validate(session_id);
    return session.has_value() && session->csrf_token == csrf_token;
  }

  void SessionService::logout(const std::string_view session_id) {
    std::scoped_lock lock(mutex_);
    sessions_.erase(std::string {session_id});
  }

  void SessionService::invalidate_all() {
    std::scoped_lock lock(mutex_);
    sessions_.clear();
  }

  void SessionService::purge_expired(const std::chrono::steady_clock::time_point now) {
    std::erase_if(sessions_, [&now](const auto &entry) {
      return entry.second.expiration <= now;
    });
  }

  PairingService::PairingService(std::shared_ptr<PairingClientBackend> backend):
      backend_ {backend ? std::move(backend) : default_pairing_client_backend()} {}

  service_result_t PairingService::submit_pin(const std::string_view pin, const std::string_view client_name) const {
    if (!is_valid_pin(pin)) {
      return {false, "invalid_pin", "PIN must contain exactly four digits."};
    }
    if (client_name.empty() || client_name.size() > 128) {
      return {false, "invalid_client_name", "Client name must contain between 1 and 128 characters."};
    }
    if (!backend_->submit_pin(pin, client_name)) {
      return {false, "pairing_rejected", "Pairing could not be completed."};
    }
    return {true, "paired", "Pairing completed."};
  }

  ClientService::ClientService(std::shared_ptr<PairingClientBackend> backend):
      backend_ {backend ? std::move(backend) : default_pairing_client_backend()} {}

  nlohmann::json ClientService::list() const {
    return backend_->list_clients();
  }

  service_result_t ClientService::revoke(const std::string_view uuid) const {
    if (uuid.empty() || uuid.size() > 128) {
      return {false, "invalid_client_id", "Client identifier is invalid."};
    }
    const auto certificate = backend_->certificate_for_uuid(uuid);
    if (!backend_->revoke_client(uuid)) {
      return {false, "client_not_found", "Paired client was not found."};
    }
    if (!certificate.empty()) {
      rtsp_stream::terminate_sessions_by_cert(certificate);
    }
    if (rtsp_stream::session_count() == 0 && proc::proc.running() > 0) {
      proc::proc.terminate();
    }
    return {true, "client_revoked", "Paired client revoked."};
  }

  nlohmann::json ApplicationService::list() const {
    try {
      return nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
    } catch (const std::exception &) {
      return {{"apps", nlohmann::json::array()}};
    }
  }

  nlohmann::json ConfigurationService::snapshot() const {
    auto persisted = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    const auto enabled = persisted.contains("steamos_virtual_display_enabled") ? persisted.at("steamos_virtual_display_enabled") : (config::steamos_virtual_display.enabled ? "enabled" : "disabled");
    const auto mode = persisted.contains("steamos_virtual_display_mode") ? persisted.at("steamos_virtual_display_mode") : std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.mode)};
    const auto session_source = persisted.contains("steamos_session_source") ? persisted.at("steamos_session_source") : std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)};
    const auto keep_session_alive = persisted.contains("steamos_keep_session_alive") ? persisted.at("steamos_keep_session_alive") : (config::steamos_virtual_display.keep_session_alive ? "enabled" : "disabled");
    return {
      {"locale", config::sunshine.locale},
      {"port", config::sunshine.port},
      {"address_family", config::sunshine.address_family},
      {"system_tray", config::sunshine.system_tray},
      {"steamos_virtual_display_enabled", enabled},
      {"steamos_virtual_display_mode", mode},
      {"steamos_session_source", session_source},
      {"steamos_keep_session_alive", keep_session_alive},
    };
  }

  service_result_t ConfigurationService::save_virtual_display(const bool enabled, const std::string_view mode, const std::string_view session_source, const bool keep_session_alive) const {
    if (!steamos_virtual_session::parse_virtual_display_mode(mode).has_value()) {
      return {false, "invalid_virtual_display_mode", "Virtual display mode must be off, auto, or force."};
    }
    if (!steamos_virtual_session::parse_session_source_policy(session_source).has_value()) {
      return {false, "invalid_steamos_session_source", "Gamescope source must be auto, existing_gamescope, or owned_private."};
    }

    try {
      auto persisted = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      persisted["steamos_virtual_display_enabled"] = enabled ? "enabled" : "disabled";
      persisted["steamos_virtual_display_mode"] = std::string {mode};
      persisted["steamos_session_source"] = std::string {session_source};
      persisted["steamos_keep_session_alive"] = keep_session_alive ? "enabled" : "disabled";

      std::vector<std::pair<std::string, std::string>> ordered {persisted.begin(), persisted.end()};
      std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
      });

      std::string content;
      for (const auto &[key, value] : ordered) {
        content += key + " = " + value + '\n';
      }
      if (file_handler::write_file(config::sunshine.config_file.c_str(), content) != 0) {
        return {false, "configuration_save_failed", "Unable to save the virtual display policy."};
      }
      return {true, "restart_required", "Virtual display policy saved. Restart SteamShine before launching Moonlight."};
    } catch (const std::exception &error) {
      BOOST_LOG(warning) << "SteamShine virtual display configuration save failed: " << error.what();
      return {false, "configuration_save_failed", "Unable to save the virtual display policy."};
    }
  }

  nlohmann::json StatusSnapshotService::snapshot() const {
    std::string render_node;
    const bool render_node_ready = steamos_virtual_session::encoder_render_node(render_node);
    const auto virtual_session {steamos_virtual_session::status_snapshot()};
    return {
      {"active_streams", rtsp_stream::session_count()},
      {"application_running", proc::proc.running() > 0},
      {"gamescope_active", steamos_virtual_session::active()},
      {"virtual_display_enabled", config::steamos_virtual_display.enabled},
      {"virtual_display_mode", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.mode)}},
      {"steamos_session_source", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)}},
      {"steamos_keep_session_alive", config::steamos_virtual_display.keep_session_alive},
      {"game_gpu", config::steamos_virtual_display.game_gpu},
      {"capture_gpu", config::steamos_virtual_display.capture_gpu},
      {"encoder_gpu", config::steamos_virtual_display.encoder_gpu},
      {"render_node", render_node_ready ? render_node : ""},
      {"encoder", config::video.encoder},
      {"virtual_display_state", std::string {steamos_virtual_session::to_string(virtual_session.state)}},
      {"virtual_display_origin", std::string {steamos_virtual_session::to_string(virtual_session.origin)}},
      {"virtual_display_process_owned", virtual_session.process_owned},
      {"virtual_display_runtime_owned", virtual_session.runtime_owned},
      {"virtual_display_socket", virtual_session.socket_path},
      {"virtual_display_runtime", virtual_session.runtime_directory},
      {"virtual_display_source_description", virtual_session.source_description},
      {"virtual_display_source_executable", virtual_session.source_executable},
      {"virtual_display_source_process_start_time", virtual_session.source_process_start_time},
      {"steam_location", virtual_session.steam_location},
      {"pipewire_runtime", virtual_session.pipewire_runtime},
      {"pipewire_remote", virtual_session.pipewire_remote},
      {"pipewire_node_id", virtual_session.pipewire_node_id.value_or(0)},
      {"pipewire_object_serial", virtual_session.pipewire_object_serial.value_or(0)},
      {"pipewire_producer_pid", virtual_session.pipewire_producer_pid},
      {"gamescope_pid", virtual_session.gamescope_pid},
      {"pci_bdf", virtual_session.pci_bdf},
      {"captured_frames", virtual_session.captured_frames},
      {"encoded_packets", virtual_session.encoded_packets},
      {"encoded_bytes", virtual_session.encoded_bytes},
      {"idr_packets", virtual_session.idr_packets},
    };
  }

  std::string DiagnosticService::recent_logs(const std::size_t maximum_bytes) const {
    const auto content = file_handler::read_file(config::sunshine.log_file.c_str());
    return content.size() <= maximum_bytes ? content : content.substr(content.size() - maximum_bytes);
  }
}  // namespace web
