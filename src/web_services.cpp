/**
 * @file src/web_services.cpp
 * @brief Shared application service implementations for the Web UI frontends.
 */

// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <vector>

// lib includes
#include <boost/algorithm/string/predicate.hpp>

// local includes
#include "build_info.h"
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
#include "stream.h"
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
     * @brief Serialize one bounded stream profile.
     *
     * @param profile Profile to serialize.
     * @return Public JSON representation.
     */
    nlohmann::json stream_profile_json(const stream_profile_t &profile) {
      return {
        {"client_id", profile.client_id},
        {"network_class", profile.network_class},
        {"capability_signature", profile.capability_signature},
        {"geometry_policy", profile.geometry_policy},
        {"fps_policy", profile.fps_policy},
        {"fps_ceiling", profile.fps_ceiling},
        {"codec_policy", profile.codec_policy},
        {"hdr_policy", profile.hdr_policy},
        {"bitrate_ceiling_kbps", profile.bitrate_ceiling_kbps},
        {"quality_preset", profile.quality_preset},
        {"orientation", profile.orientation},
        {"safe_area_percent", profile.safe_area_percent},
        {"learned_start_kbps", profile.learned_start_kbps},
        {"active", profile.active},
      };
    }

    /**
     * @brief Parse one stream profile without accepting implicit JSON coercion.
     *
     * @param value JSON object to parse.
     * @return Parsed profile, or an empty optional for malformed input.
     */
    std::optional<stream_profile_t> stream_profile_from_json(const nlohmann::json &value) {
      try {
        return stream_profile_t {
          .client_id = value.at("client_id").get<std::string>(),
          .network_class = value.at("network_class").get<std::string>(),
          .capability_signature = value.at("capability_signature").get<std::string>(),
          .geometry_policy = value.value("geometry_policy", "fit"),
          .fps_policy = value.value("fps_policy", "auto"),
          .fps_ceiling = value.value("fps_ceiling", 0),
          .codec_policy = value.value("codec_policy", "auto"),
          .hdr_policy = value.value("hdr_policy", "auto"),
          .bitrate_ceiling_kbps = value.value("bitrate_ceiling_kbps", 0),
          .quality_preset = value.value("quality_preset", "balanced"),
          .orientation = value.value("orientation", "auto"),
          .safe_area_percent = value.value("safe_area_percent", 0),
          .learned_start_kbps = value.value("learned_start_kbps", 0),
          .active = value.value("active", false),
        };
      } catch (const nlohmann::json::exception &) {
        return std::nullopt;
      }
    }

    /**
     * @brief Resolve the owner-private default stream profile path.
     *
     * @return Absolute profile path, or an empty path when no user state root exists.
     */
    std::filesystem::path default_stream_profile_path() {
      if (const auto *const state_home {std::getenv("XDG_STATE_HOME")}; state_home && *state_home) {
        const std::filesystem::path path {state_home};
        if (path.is_absolute()) {
          return path / "steamshine" / "stream-profiles.json";
        }
      }
      if (const auto *const user_home {std::getenv("HOME")}; user_home && *user_home) {
        const std::filesystem::path path {user_home};
        if (path.is_absolute()) {
          return path / ".local" / "state" / "steamshine" / "stream-profiles.json";
        }
      }
      return {};
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

  StreamProfileService::StreamProfileService(std::filesystem::path path):
      path_ {path.empty() ? default_stream_profile_path() : std::move(path)} {
    if (path_.empty()) {
      return;
    }
    try {
      const auto document = nlohmann::json::parse(file_handler::read_file(path_.string().c_str()));
      if (document.value("schema_version", 0) != 1 || !document.contains("profiles") || !document.at("profiles").is_array()) {
        return;
      }
      for (const auto &value : document.at("profiles")) {
        const auto profile {stream_profile_from_json(value)};
        if (!profile || !validate(*profile).empty() || profiles_.size() >= 64) {
          continue;
        }
        if (profile->active) {
          for (auto &[existing_key, existing_profile] : profiles_) {
            if (existing_key.first == profile->client_id) {
              existing_profile.active = false;
            }
          }
        }
        profiles_[{profile->client_id, profile->network_class}] = *profile;
      }
    } catch (const std::exception &) {
    }
  }

  nlohmann::json StreamProfileService::snapshot() const {
    std::lock_guard lock {mutex_};
    nlohmann::json profiles = nlohmann::json::array();
    for (const auto &[key, profile] : profiles_) {
      static_cast<void>(key);
      profiles.push_back(stream_profile_json(profile));
    }
    return nlohmann::json::object({
      {"schema_version", 1},
      {"maximum_profiles", 64},
      {"profiles", std::move(profiles)},
    });
  }

  service_result_t StreamProfileService::save(const stream_profile_t &profile) {
    if (const auto error {validate(profile)}; !error.empty()) {
      return {false, error, "Stream profile validation failed."};
    }
    std::lock_guard lock {mutex_};
    const auto key {std::make_pair(profile.client_id, profile.network_class)};
    if (!profiles_.contains(key) && profiles_.size() >= 64) {
      return {false, "profile_limit_reached", "The bounded stream profile limit was reached."};
    }
    const auto old_profiles {profiles_};
    if (profile.active) {
      for (auto &[existing_key, existing_profile] : profiles_) {
        if (existing_key.first == profile.client_id) {
          existing_profile.active = false;
        }
      }
    }
    profiles_[key] = profile;
    if (!persist_locked()) {
      profiles_ = old_profiles;
      return {false, "profile_save_failed", "Unable to save the stream profile."};
    }
    return {true, "profile_saved", "Stream profile saved."};
  }

  service_result_t StreamProfileService::ensure_registered(const std::string_view client_id, const std::string_view capability_signature) {
    stream_profile_t candidate;
    candidate.client_id = std::string {client_id};
    candidate.network_class = default_network_class;
    candidate.capability_signature = std::string {capability_signature};
    candidate.active = true;
    if (const auto error {validate(candidate)}; !error.empty()) {
      return {false, error, "Stream profile registration key is invalid."};
    }
    std::lock_guard lock {mutex_};
    const auto existing {std::find_if(profiles_.begin(), profiles_.end(), [&candidate](const auto &entry) {
      return entry.first.first == candidate.client_id;
    })};
    if (existing != profiles_.end()) {
      return {true, "profile_present", "Stream profile already exists for this client."};
    }
    if (profiles_.size() >= 64) {
      return {false, "profile_limit_reached", "The bounded stream profile limit was reached."};
    }
    const auto key {std::make_pair(candidate.client_id, candidate.network_class)};
    profiles_[key] = candidate;
    if (!persist_locked()) {
      profiles_.erase(key);
      return {false, "profile_save_failed", "Unable to record the connecting client."};
    }
    return {true, "profile_registered", "Connecting client recorded with automatic defaults."};
  }

  service_result_t StreamProfileService::reset(const std::string_view client_id, const std::string_view network_class) {
    stream_profile_t reset_key;
    reset_key.client_id = std::string {client_id};
    reset_key.network_class = std::string {network_class};
    reset_key.capability_signature = "reset";
    if (const auto error {validate(reset_key)}; error == "invalid_client_id" || error == "invalid_network_class") {
      return {false, error, "Stream profile reset key is invalid."};
    }
    std::lock_guard lock {mutex_};
    const auto key {std::make_pair(std::string {client_id}, std::string {network_class})};
    const auto position {profiles_.find(key)};
    if (position == profiles_.end()) {
      return {false, "profile_not_found", "Stream profile was not found."};
    }
    const auto previous {position->second};
    profiles_.erase(position);
    if (!persist_locked()) {
      profiles_[key] = previous;
      return {false, "profile_save_failed", "Unable to reset the stream profile."};
    }
    return {true, "profile_reset", "Stream profile reset."};
  }

  stream_profile_selection_t StreamProfileService::select(
    const std::string_view client_id,
    const std::string_view network_class,
    const std::string_view capability_signature
  ) const {
    std::lock_guard lock {mutex_};
    const auto position {profiles_.find({std::string {client_id}, std::string {network_class}})};
    if (position == profiles_.end()) {
      return {std::nullopt, "profile_not_found"};
    }
    if (position->second.capability_signature != capability_signature) {
      return {std::nullopt, "capability_signature_changed"};
    }
    return {position->second, "exact_client_network_capability_match"};
  }

  stream_profile_selection_t StreamProfileService::select_active(
    const std::string_view client_id,
    const std::string_view capability_signature
  ) const {
    std::lock_guard lock {mutex_};
    const auto position = std::find_if(profiles_.begin(), profiles_.end(), [client_id](const auto &entry) {
      return entry.first.first == client_id && entry.second.active;
    });
    if (position == profiles_.end()) {
      return {std::nullopt, "active_network_profile_not_found"};
    }
    if (position->second.capability_signature != capability_signature) {
      return {std::nullopt, "capability_signature_changed"};
    }
    return {position->second, "exact_active_client_network_capability_match"};
  }

  service_result_t StreamProfileService::update_learned_start(
    const std::string_view client_id,
    const std::string_view network_class,
    const std::string_view capability_signature,
    const int learned_start_kbps
  ) {
    if (learned_start_kbps < 0 || learned_start_kbps > 200000) {
      return {false, "invalid_learned_start", "Learned stream bitrate is outside the bounded range."};
    }
    std::lock_guard lock {mutex_};
    const auto key {std::make_pair(std::string {client_id}, std::string {network_class})};
    const auto position {profiles_.find(key)};
    if (position == profiles_.end()) {
      return {false, "profile_not_found", "Stream profile was not found."};
    }
    if (!position->second.active || position->second.capability_signature != capability_signature) {
      return {false, "profile_selection_changed", "Stream profile selection changed before learning completed."};
    }
    const auto previous {position->second.learned_start_kbps};
    position->second.learned_start_kbps = learned_start_kbps;
    if (!persist_locked()) {
      position->second.learned_start_kbps = previous;
      return {false, "profile_save_failed", "Unable to save the learned stream bitrate."};
    }
    return {true, "learned_start_saved", "Learned stream bitrate saved."};
  }

  StreamProfileService &stream_profile_service() {
    static StreamProfileService service;
    return service;
  }

  std::string stream_capability_signature(const video::config_t &request, const bool hdr_requested) {
    return std::format(
      "v1-c{}-d{}-x{}-h{}",
      request.videoFormat,
      request.requestedDynamicRange,
      request.chromaSamplingType,
      hdr_requested ? 1 : 0
    );
  }

  stream_profile_application_t apply_stream_profile(const stream_profile_t &profile, video::config_t &config) {
    stream_profile_application_t result;
    if (profile.fps_policy == "custom" && config.framerate > profile.fps_ceiling) {
      config.framerate = profile.fps_ceiling;
      config.framerateX100 = 0;
      result.applied = true;
      result.fallback_reasons.emplace_back("profile_fps_ceiling_applied");
    }
    if (profile.bitrate_ceiling_kbps > 0 && config.bitrate > profile.bitrate_ceiling_kbps) {
      config.bitrate = profile.bitrate_ceiling_kbps;
      result.applied = true;
      result.fallback_reasons.emplace_back("profile_bitrate_ceiling_applied");
    }
    if (profile.learned_start_kbps > 0 && config.bitrate > profile.learned_start_kbps) {
      config.bitrate = profile.learned_start_kbps;
      result.applied = true;
      result.fallback_reasons.emplace_back("profile_learned_start_applied");
    }
    const auto requested_codec = config.videoFormat == 0 ? "h264" : config.videoFormat == 1 ? "hevc" :
                                                                  config.videoFormat == 2   ? "av1" :
                                                                                              "unknown";
    if (profile.codec_policy != "auto" && profile.codec_policy != requested_codec) {
      result.fallback_reasons.emplace_back("profile_codec_yielded_to_client_request");
    }
    if (profile.hdr_policy == "off" && config.dynamicRange != 0) {
      config.dynamicRange = 0;
      result.applied = true;
      result.fallback_reasons.emplace_back("profile_hdr_off_applied");
    } else if (profile.hdr_policy == "require" && config.requestedDynamicRange != 1) {
      result.fallback_reasons.emplace_back("profile_hdr_requirement_yielded_to_client_capability");
    }
    return result;
  }

  std::string StreamProfileService::validate(const stream_profile_t &profile) {
    const auto bounded_token = [](const std::string_view value, const std::size_t maximum) {
      return !value.empty() && value.size() <= maximum && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == ':';
      });
    };
    if (!bounded_token(profile.client_id, 128)) {
      return "invalid_client_id";
    }
    if (!bounded_token(profile.network_class, 32)) {
      return "invalid_network_class";
    }
    if (!bounded_token(profile.capability_signature, 256)) {
      return "invalid_capability_signature";
    }
    const auto one_of = [](const std::string &value, const std::initializer_list<std::string_view> accepted) {
      return std::find(accepted.begin(), accepted.end(), value) != accepted.end();
    };
    if (!one_of(profile.geometry_policy, {"exact", "fit", "virtual_fallback"})) {
      return "invalid_geometry_policy";
    }
    if (!one_of(profile.fps_policy, {"auto", "custom"}) || (profile.fps_policy == "auto" ? profile.fps_ceiling != 0 : profile.fps_ceiling < 30 || profile.fps_ceiling > 240)) {
      return "invalid_fps_policy";
    }
    if (!one_of(profile.codec_policy, {"auto", "h264", "hevc", "av1"})) {
      return "invalid_codec_policy";
    }
    if (!one_of(profile.hdr_policy, {"off", "auto", "require"})) {
      return "invalid_hdr_policy";
    }
    if (profile.bitrate_ceiling_kbps < 0 || profile.bitrate_ceiling_kbps > 200000) {
      return "invalid_bitrate_ceiling";
    }
    if (!one_of(profile.quality_preset, {"low_latency", "balanced", "quality"})) {
      return "invalid_quality_preset";
    }
    if (!one_of(profile.orientation, {"auto", "landscape", "portrait"}) || profile.safe_area_percent < 0 || profile.safe_area_percent > 25) {
      return "invalid_layout_policy";
    }
    if (profile.learned_start_kbps < 0 || profile.learned_start_kbps > 200000) {
      return "invalid_learned_start";
    }
    return {};
  }

  bool StreamProfileService::persist_locked() const {
    if (path_.empty()) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
      return false;
    }
    nlohmann::json profiles = nlohmann::json::array();
    for (const auto &[key, profile] : profiles_) {
      static_cast<void>(key);
      profiles.push_back(stream_profile_json(profile));
    }
    const auto document = nlohmann::json::object({
      {"schema_version", 1},
      {"profiles", std::move(profiles)},
    });
    auto temporary {path_};
    temporary += ".tmp";
    {
      std::ofstream output {temporary, std::ios::binary | std::ios::trunc};
      if (!output || !(output << document.dump(2) << '\n')) {
        return false;
      }
    }
    std::filesystem::permissions(
      temporary,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      error
    );
    if (error) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    std::filesystem::rename(temporary, path_, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    return true;
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
    const auto steam_migration = persisted.contains("steamos_steam_migration") ? persisted.at("steamos_steam_migration") : std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.steam_migration)};
    const auto stock_session_handoff = persisted.contains("steamos_stock_session_handoff") ? persisted.at("steamos_stock_session_handoff") : std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.stock_session_handoff)};
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
      {"steamos_steam_migration", steam_migration},
      {"steamos_stock_session_handoff", stock_session_handoff},
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

  service_result_t ConfigurationService::save_virtual_display(const bool enabled, const std::string_view mode, const std::string_view session_source, const std::string_view local_presentation, const bool keep_session_alive, const int existing_gamescope_pid, const std::string_view steam_migration, const std::string_view stock_session_handoff) const {
    if (!steamos_virtual_session::parse_virtual_display_mode(mode).has_value()) {
      return {false, "invalid_virtual_display_mode", "Virtual display mode must be off, auto, or force."};
    }
    if (!steamos_virtual_session::parse_session_source_policy(session_source).has_value()) {
      return {false, "invalid_steamos_session_source", "Gamescope source must be auto, existing_gamescope, or owned_private."};
    }
    if (!steamos_virtual_session::parse_local_presentation_policy(local_presentation).has_value()) {
      return {false, "invalid_steamos_local_presentation", "Local presentation must be auto, off, or mirror."};
    }
    if (!steamos_virtual_session::parse_steam_migration_policy(steam_migration).has_value()) {
      return {false, "invalid_steamos_steam_migration", "Steam migration must be reject or auto_idle."};
    }
    if (!steamos_virtual_session::parse_stock_handoff_policy(stock_session_handoff).has_value()) {
      return {false, "invalid_steamos_stock_session_handoff", "Stock session handoff must be attach or auto_idle."};
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
      persisted["steamos_steam_migration"] = std::string {steam_migration};
      persisted["steamos_stock_session_handoff"] = std::string {stock_session_handoff};
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
    const auto codec {video::codec_status_snapshot()};
    const auto [input_diagnostics, video_diagnostics] {aggregated_diagnostics()};
    const auto negotiation {rtsp_stream::active_negotiation_snapshot()};
    const stream::stream_negotiation_snapshot_t empty_negotiation;
    const auto hdr_status {video::hdr_status_snapshot()};
    const auto *const launch_mode {std::getenv("STEAMSHINE_LAUNCH_MODE")};
    return {
      {"service_binary_commit", build_info::commit()},
      {"service_config_path", config::sunshine.config_file},
      {"service_launch_mode", launch_mode && *launch_mode ? launch_mode : "manual"},
      {"active_streams", rtsp_stream::session_count()},
      {"stream_negotiation", stream::negotiation_snapshot_json(negotiation ? *negotiation : empty_negotiation, negotiation.has_value())},
      {"application_running", proc::proc.running() > 0},
      {"gamescope_active", steamos_virtual_session::active()},
      {"virtual_display_enabled", config::steamos_virtual_display.enabled},
      {"virtual_display_mode", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.mode)}},
      {"steamos_session_source", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)}},
      {"steamos_local_presentation", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.local_presentation)}},
      {"steamos_steam_migration", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.steam_migration)}},
      {"steamos_stock_session_handoff", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.stock_session_handoff)}},
      {"steamos_keep_session_alive", config::steamos_virtual_display.keep_session_alive},
      {"game_gpu", config::steamos_virtual_display.game_gpu},
      {"capture_gpu", config::steamos_virtual_display.capture_gpu},
      {"encoder_gpu", config::steamos_virtual_display.encoder_gpu},
      {"render_node", render_node_ready ? render_node : ""},
      {"encoder", config::video.encoder},
      {"codec_policy", std::string {codec_policy::to_string(config::video.codec_policy)}},
      {"codec_fallback", std::string {codec_policy::to_string(config::video.codec_fallback)}},
      {"codec_allow_software", config::video.codec_allow_software},
      {"codec_state", {
                        {"requested", codec.requested},
                        {"selected", codec.selected},
                        {"active", codec.active},
                        {"profile", codec.profile},
                        {"bit_depth", codec.bit_depth},
                        {"backend", codec.backend},
                        {"hardware", codec.hardware},
                        {"reason", codec.reason},
                      }},
      {"steamshine_hdr_policy", hdr_policy::to_string(config::video.steamshine_hdr_policy)},
      {"hdr_state", {
                      {"requested", hdr_status.requested},
                      {"client_capable", hdr_status.client_capable},
                      {"selected", hdr_status.selected},
                      {"active", hdr_status.active},
                      {"bit_depth", hdr_status.bit_depth},
                      {"primaries", hdr_status.primaries},
                      {"transfer", hdr_status.transfer},
                      {"matrix", hdr_status.matrix},
                      {"range", hdr_status.range},
                      {"reason", hdr_status.reason},
                    }},
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
      {"steam_migration_state", std::string {steamos_virtual_session::to_string(virtual_session.migration_state)}},
      {"stock_handoff_state", std::string {steamos_virtual_session::to_string(virtual_session.stock_handoff_state)}},
      {"stock_handoff_reason", virtual_session.stock_handoff_reason},
      {"stock_handoff_generation", virtual_session.stock_handoff_generation},
      {"app_launch_rejected_reason", virtual_session.app_launch_rejected_reason},
      {"app_launch_rejected_message", virtual_session.app_launch_rejected_message},
      {"requested_display_endpoint", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)}},
      {"active_display_endpoint_origin", std::string {steamos_virtual_session::to_string(virtual_session.display_endpoint.origin)}},
      {"active_display_endpoint_verified", virtual_session.display_endpoint.verification == steamos_virtual_session::display_verification_e::verified},
      {"active_display_endpoint_error", virtual_session.display_endpoint.error},
      {"active_display_endpoint_wayland", virtual_session.display_endpoint.wayland_display},
      {"active_display_endpoint_x11", virtual_session.display_endpoint.x11_display},
      {"active_display_endpoint_producer_pid", virtual_session.display_endpoint.producer_pid},
      {"active_display_endpoint_producer_start_time", virtual_session.display_endpoint.producer_start_time},
      {"active_display_endpoint_environment_pid", virtual_session.display_endpoint.environment_source_pid},
      {"active_display_endpoint_environment_start_time", virtual_session.display_endpoint.environment_source_start_time},
      {"active_display_endpoint_generation", virtual_session.display_endpoint.generation},
      {"capture_selection_reason", virtual_session.selection_reason},
      {"owned_gamescope_backend", std::string {steamos_virtual_session::to_string(virtual_session.owned_backend)}},
      {"presentation_reason", virtual_session.presentation_reason},
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
      {"requested_stream_width", virtual_session.requested_width},
      {"requested_stream_height", virtual_session.requested_height},
      {"requested_stream_refresh", {
                                     {"numerator", virtual_session.requested_refresh.numerator},
                                     {"denominator", virtual_session.requested_refresh.denominator},
                                   }},
      {"selected_stream_width", virtual_session.width},
      {"selected_stream_height", virtual_session.height},
      {"selected_stream_refresh", {
                                    {"numerator", virtual_session.refresh.numerator},
                                    {"denominator", virtual_session.refresh.denominator},
                                  }},
      {"stream_geometry_reason", virtual_session.geometry_reason},
      {"capture_width", virtual_session.capture_width},
      {"capture_height", virtual_session.capture_height},
      {"content_rectangle", {
                              {"x", virtual_session.content_rectangle.x},
                              {"y", virtual_session.content_rectangle.y},
                              {"width", virtual_session.content_rectangle.width},
                              {"height", virtual_session.content_rectangle.height},
                            }},
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
      {"pipewire_buffers_received", video_diagnostics.pipewire_buffers_received},
      {"pipewire_buffers_replaced", video_diagnostics.pipewire_buffers_replaced},
      {"encoder_queue_current", video_diagnostics.encoder_queue_current},
      {"encoder_queue_max", video_diagnostics.encoder_queue_max},
      {"capture_deadline_misses", video_diagnostics.capture_deadline_misses},
      {"encoded_unique_frames", video_diagnostics.encoded_unique_frames},
      {"encoded_duplicate_frames", video_diagnostics.encoded_duplicate_frames},
      {"encoded_frames", video_diagnostics.encoded_frames},
      {"duplicate_frames", video_diagnostics.duplicate_frames},
      {"duplicate_run_max", video_diagnostics.duplicate_run_max},
      {"pipewire_buffers_received", video_diagnostics.pipewire_buffers_received},
      {"pipewire_unique_frames", video_diagnostics.pipewire_unique_frames},
      {"pipewire_redundant_pts", video_diagnostics.pipewire_redundant_pts},
      {"pipewire_no_damage_frames", video_diagnostics.pipewire_no_damage_frames},
      {"pipewire_queue_overflows", video_diagnostics.pipewire_queue_overflows},
      {"requested_fps", {
                          {"numerator", video_diagnostics.requested_fps_numerator},
                          {"denominator", video_diagnostics.requested_fps_denominator},
                        }},
      {"negotiated_fps", {
                           {"numerator", video_diagnostics.negotiated_fps_numerator},
                           {"denominator", video_diagnostics.negotiated_fps_denominator},
                         }},
      {"negotiated_max_fps", {
                               {"numerator", video_diagnostics.negotiated_max_fps_numerator},
                               {"denominator", video_diagnostics.negotiated_max_fps_denominator},
                             }},
      {"observed_source_fps", video_diagnostics.observed_source_fps},
      {"observed_encode_fps", video_diagnostics.observed_encode_fps},
      {"output_status_reason", video_diagnostics.output_status_reason},
      {"source_interarrival_ms", latency_statistics_json(video_diagnostics.source_interarrival_ms)},
      {"encode_interarrival_ms", latency_statistics_json(video_diagnostics.encode_interarrival_ms)},
      {"network_queue_bytes", video_diagnostics.network_queue_bytes},
      {"network_queue_frames", video_diagnostics.network_queue_frames},
      {"network_queue_frames_max", video_diagnostics.network_queue_frames_max},
      {"socket_outq_bytes", video_diagnostics.socket_outq_bytes},
      {"socket_outq_bytes_max", video_diagnostics.socket_outq_bytes_max},
      {"adaptive_bitrate", {
                             {"enabled", video_diagnostics.adaptive_bitrate_enabled},
                             {"minimum_kbps", video_diagnostics.bitrate.minimum_kbps},
                             {"initial_kbps", video_diagnostics.bitrate.initial_kbps},
                             {"target_kbps", video_diagnostics.bitrate.target_kbps},
                             {"active_kbps", video_diagnostics.bitrate_active_kbps},
                             {"maximum_kbps", video_diagnostics.bitrate.maximum_kbps},
                             {"peak_kbps", video_diagnostics.bitrate.peak_kbps},
                             {"vbv_kbits", video_diagnostics.bitrate.vbv_kbits},
                             {"actual_video_kbps", video_diagnostics.actual_video_bitrate_kbps},
                             {"learned_next_kbps", video_diagnostics.bitrate_learned_next_kbps},
                             {"state", adaptive_bitrate::to_string(video_diagnostics.congestion_state)},
                             {"reason", video_diagnostics.bitrate_reason},
                             {"runtime_update_supported", video_diagnostics.runtime_bitrate_update_supported},
                             {"feedback_samples", video_diagnostics.bitrate_feedback_samples},
                             {"lost_packets", video_diagnostics.bitrate_lost_packets},
                             {"updates_applied", video_diagnostics.bitrate_updates_applied},
                             {"updates_unsupported", video_diagnostics.bitrate_updates_unsupported},
                             {"updates_failed", video_diagnostics.bitrate_updates_failed},
                           }},
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
