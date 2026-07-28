/**
 * @file src/web_services.cpp
 * @brief Shared application service implementations for the Web UI frontends.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

// lib includes
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "input.h"
#include "nvhttp.h"
#include "platform/linux/gamescope_source.h"
#include "process.h"
#include "rtsp.h"
#include "steamos_virtual_session.h"
#include "video.h"
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

    /**
     * @brief Serialize fixed-memory latency statistics for the status API.
     *
     * @param statistics Aggregate and percentile values to expose.
     * @return Stable JSON object with millisecond units.
     */
    nlohmann::json latency_statistics_json(const latency_diagnostics::statistics_t &statistics) {
      return {
        {"count", statistics.count},
        {"window_count", statistics.window_count},
        {"average", statistics.average_ms},
        {"p50", statistics.p50_ms},
        {"p95", statistics.p95_ms},
        {"p99", statistics.p99_ms},
        {"max", statistics.max_ms},
      };
    }

    /**
     * @brief Cached 500 ms diagnostics aggregation used only by status readers.
     */
    struct diagnostics_aggregation_t {
      std::mutex mutex;  ///< Serializes infrequent Web snapshot refreshes.
      std::chrono::steady_clock::time_point refreshed_at {};  ///< Last atomic/ring aggregation time.
      input::diagnostics_snapshot_t input;  ///< Cached input counters and percentiles.
      video::pipeline_diagnostics_t video;  ///< Cached video counters and percentiles.
    };

    /**
     * @brief Refresh diagnostics at most once per 500 ms status interval.
     *
     * @return Copies of the current input and video aggregate snapshots.
     */
    std::pair<input::diagnostics_snapshot_t, video::pipeline_diagnostics_t> aggregated_diagnostics() {
      static diagnostics_aggregation_t aggregation;
      constexpr auto aggregation_interval = std::chrono::milliseconds {500};
      std::scoped_lock lock {aggregation.mutex};
      const auto now {std::chrono::steady_clock::now()};
      if (aggregation.refreshed_at.time_since_epoch().count() == 0 || now - aggregation.refreshed_at >= aggregation_interval) {
        aggregation.input = input::diagnostics_snapshot();
        aggregation.video = video::pipeline_diagnostics_snapshot();
        aggregation.refreshed_at = now;
      }
      return {aggregation.input, aggregation.video};
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
    const auto local_presentation = persisted.contains("steamos_local_presentation") ? persisted.at("steamos_local_presentation") : std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.local_presentation)};
    const auto keep_session_alive = persisted.contains("steamos_keep_session_alive") ? persisted.at("steamos_keep_session_alive") : (config::steamos_virtual_display.keep_session_alive ? "enabled" : "disabled");
    const auto existing_gamescope_pid = persisted.contains("steamos_existing_gamescope_pid") ? persisted.at("steamos_existing_gamescope_pid") : std::to_string(config::steamos_virtual_display.existing_gamescope_pid);
    return {
      {"locale", config::sunshine.locale},
      {"port", config::sunshine.port},
      {"address_family", config::sunshine.address_family},
      {"system_tray", config::sunshine.system_tray},
      {"steamos_virtual_display_enabled", enabled},
      {"steamos_virtual_display_mode", mode},
      {"steamos_session_source", session_source},
      {"steamos_local_presentation", local_presentation},
      {"steamos_keep_session_alive", keep_session_alive},
      {"steamos_existing_gamescope_pid", existing_gamescope_pid},
    };
  }

  nlohmann::json ConfigurationService::gamescope_sources() const {
    const char *const inherited_runtime {std::getenv("XDG_RUNTIME_DIR")};
    const char *const inherited_remote {std::getenv("PIPEWIRE_REMOTE")};
    const std::string runtime {config::steamos_virtual_display.pipewire_runtime.empty() ? (inherited_runtime ? inherited_runtime : "") : config::steamos_virtual_display.pipewire_runtime};
    const std::string remote {config::steamos_virtual_display.pipewire_remote.empty() ? (inherited_remote && *inherited_remote ? inherited_remote : "pipewire-0") : config::steamos_virtual_display.pipewire_remote};
    std::string error;
    const auto sources {gamescope_source::discover_gamescope_sources(runtime, remote, std::chrono::milliseconds {250}, error)};
    nlohmann::json candidates {nlohmann::json::array()};
    for (const auto &source : sources) {
      if (!source.game_mode_verified || source.origin != steamos_virtual_session::session_origin_e::attached_existing) {
        continue;
      }
      candidates.push_back({
        {"pid", source.producer_pid},
        {"start_time", source.producer_start_time},
        {"description", source.node_description.empty() ? source.application_name : source.node_description},
        {"render_node", source.render_node},
      });
    }
    return {
      {"available", error.empty()},
      {"error", error},
      {"sources", candidates},
    };
  }

  service_result_t ConfigurationService::save_virtual_display(const bool enabled, const std::string_view mode, const std::string_view session_source, const std::string_view local_presentation, const bool keep_session_alive, const int existing_gamescope_pid) const {
    if (!steamos_virtual_session::parse_virtual_display_mode(mode).has_value()) {
      return {false, "invalid_virtual_display_mode", "Virtual display mode must be off, auto, or force."};
    }
    if (!steamos_virtual_session::parse_session_source_policy(session_source).has_value()) {
      return {false, "invalid_steamos_session_source", "Gamescope source must be auto, existing_gamescope, or owned_private."};
    }
    if (!steamos_virtual_session::parse_local_presentation_policy(local_presentation).has_value()) {
      return {false, "invalid_steamos_local_presentation", "Local presentation must be auto, off, or mirror."};
    }
    if (existing_gamescope_pid < 0) {
      return {false, "invalid_steamos_existing_gamescope_pid", "Gamescope PID must be zero or a positive process ID."};
    }

    try {
      auto persisted = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      persisted["steamos_virtual_display_enabled"] = enabled ? "enabled" : "disabled";
      persisted["steamos_virtual_display_mode"] = std::string {mode};
      persisted["steamos_session_source"] = std::string {session_source};
      persisted["steamos_local_presentation"] = std::string {local_presentation};
      persisted["steamos_keep_session_alive"] = keep_session_alive ? "enabled" : "disabled";
      persisted["steamos_existing_gamescope_pid"] = std::to_string(existing_gamescope_pid);

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
    const auto [input_diagnostics, video_diagnostics] {aggregated_diagnostics()};
    const auto *const launch_mode {std::getenv("STEAMSHINE_LAUNCH_MODE")};
    return {
      {"service_binary_commit", PROJECT_VERSION_COMMIT},
      {"service_config_path", config::sunshine.config_file},
      {"service_launch_mode", launch_mode && *launch_mode ? launch_mode : "manual"},
      {"active_streams", rtsp_stream::session_count()},
      {"application_running", proc::proc.running() > 0},
      {"gamescope_active", steamos_virtual_session::active()},
      {"virtual_display_enabled", config::steamos_virtual_display.enabled},
      {"virtual_display_mode", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.mode)}},
      {"steamos_session_source", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)}},
      {"steamos_local_presentation", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.local_presentation)}},
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
      {"migration_required", virtual_session.migration_required},
      {"app_launch_rejected_reason", virtual_session.app_launch_rejected_reason},
      {"app_launch_rejected_message", virtual_session.app_launch_rejected_message},
      {"capture_selection_reason", virtual_session.selection_reason},
      {"presentation", std::string {steamos_virtual_session::to_string(virtual_session.presentation)}},
      {"local_presenter_active", virtual_session.local_presenter_active},
      {"local_presented_frames", virtual_session.local_presented_frames},
      {"local_dropped_frames", virtual_session.local_dropped_frames},
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
      {"input_events_received", input_diagnostics.events_received},
      {"input_events_injected", input_diagnostics.events_injected},
      {"input_motion_coalesced", input_diagnostics.motion_coalesced},
      {"input_motion_dropped", input_diagnostics.motion_dropped},
      {"input_queue_current", input_diagnostics.queue_current},
      {"input_queue_max", input_diagnostics.queue_max},
      {"input_queue_age_ms", latency_statistics_json(input_diagnostics.queue_age_ms)},
      {"input_route_target", input_diagnostics.route_target},
      {"input_route_error", input_diagnostics.route_error},
      {"capture_queue_current", video_diagnostics.capture_queue_current},
      {"capture_queue_max", video_diagnostics.capture_queue_max},
      {"capture_frames_replaced", video_diagnostics.capture_frames_replaced},
      {"encoder_queue_current", video_diagnostics.encoder_queue_current},
      {"encoder_queue_max", video_diagnostics.encoder_queue_max},
      {"capture_deadline_misses", video_diagnostics.capture_deadline_misses},
      {"encoded_unique_frames", video_diagnostics.encoded_unique_frames},
      {"encoded_duplicate_frames", video_diagnostics.encoded_duplicate_frames},
      {"duplicate_run_max", video_diagnostics.duplicate_run_max},
      {"pipewire_buffers_received", video_diagnostics.pipewire_buffers_received},
      {"pipewire_unique_frames", video_diagnostics.pipewire_unique_frames},
      {"pipewire_redundant_pts", video_diagnostics.pipewire_redundant_pts},
      {"pipewire_no_damage_frames", video_diagnostics.pipewire_no_damage_frames},
      {"source_interarrival_ms", latency_statistics_json(video_diagnostics.source_interarrival_ms)},
      {"encode_interarrival_ms", latency_statistics_json(video_diagnostics.encode_interarrival_ms)},
      {"network_queue_bytes", video_diagnostics.network_queue_bytes},
      {"network_queue_frames", video_diagnostics.network_queue_frames},
      {"network_queue_frames_max", video_diagnostics.network_queue_frames_max},
      {"socket_outq_bytes", video_diagnostics.socket_outq_bytes},
      {"socket_outq_bytes_max", video_diagnostics.socket_outq_bytes_max},
      {"idr_requests", video_diagnostics.idr_requests},
      {"idr_emitted", video_diagnostics.idr_emitted},
      {"idr_reason_client_request", video_diagnostics.idr_reason_client_request},
      {"idr_reason_recovery", video_diagnostics.idr_reason_recovery},
      {"idr_reason_periodic", video_diagnostics.idr_reason_periodic},
      {"idr_reason_reconnect", video_diagnostics.idr_reason_reconnect},
      {"frame_age_at_capture_ms", latency_statistics_json(video_diagnostics.frame_age_at_capture_ms)},
      {"frame_age_at_encode_ms", latency_statistics_json(video_diagnostics.frame_age_at_encode_ms)},
      {"frame_age_at_network_ms", latency_statistics_json(video_diagnostics.frame_age_at_network_ms)},
    };
  }

  std::string DiagnosticService::recent_logs(const std::size_t maximum_bytes) const {
    const auto content = file_handler::read_file(config::sunshine.log_file.c_str());
    return content.size() <= maximum_bytes ? content : content.substr(content.size() - maximum_bytes);
  }
}  // namespace web
