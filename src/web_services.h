/**
 * @file src/web_services.h
 * @brief Shared application services used by both Sunshine and SteamShine Web UIs.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace video {
  struct config_t;
}

namespace web {
  /**
   * @brief Stable, non-secret result returned by a Web application service.
   */
  struct service_result_t {
    bool success;  ///< Whether the requested operation completed successfully.
    std::string code;  ///< Stable machine-readable result code.
    std::string message;  ///< Safe message suitable for a Web response.
  };

  /**
   * @brief Validate a Moonlight PIN before it reaches the pairing backend.
   *
   * @param pin Candidate PIN supplied by a browser.
   * @return True only for exactly four ASCII decimal digits.
   */
  bool is_valid_pin(std::string_view pin);

  /**
   * @brief Reuses Sunshine credential persistence and verification.
   */
  class CredentialService {
  public:
    /**
     * @brief Report whether the initial Web credential has been configured.
     *
     * @return True when a username is currently configured.
     */
    bool is_configured() const;

    /**
     * @brief Verify a supplied Web UI credential without exposing credential material.
     *
     * @param username Candidate username.
     * @param password Candidate password.
     * @return True when the credential matches the configured Web UI credential.
     */
    bool verify(std::string_view username, std::string_view password) const;

    /**
     * @brief Create the initial credential or replace it after verification.
     *
     * @param current_username Current username when replacing an existing credential.
     * @param current_password Current password when replacing an existing credential.
     * @param new_username Requested username.
     * @param new_password Requested password.
     * @param confirm_password Confirmation of the requested password.
     * @return Non-secret operation result.
     */
    service_result_t save(std::string_view current_username, std::string_view current_password, std::string_view new_username, std::string_view new_password, std::string_view confirm_password) const;
  };

  /**
   * @brief Public session details returned after successful SteamShine authentication.
   */
  struct session_t {
    std::string id;  ///< Opaque server-generated session identifier.
    std::string csrf_token;  ///< Opaque CSRF token bound to this session.
    std::string username;  ///< Authenticated username.
  };

  /**
   * @brief Manages short-lived, server-side SteamShine Web sessions.
   */
  class SessionService {
  public:
    /**
     * @brief Construct a server-side SteamShine session store.
     *
     * @param lifetime Lifetime assigned to newly created sessions.
     */
    explicit SessionService(std::chrono::steady_clock::duration lifetime = std::chrono::hours(8));

    /**
     * @brief Authenticate a user and create a server-side session.
     *
     * @param credential_service Credential verifier shared with the upstream UI.
     * @param username Candidate username.
     * @param password Candidate password.
     * @return Session details on success, otherwise an empty optional.
     */
    std::optional<session_t> login(const CredentialService &credential_service, std::string_view username, std::string_view password);

    /**
     * @brief Validate an existing session identifier.
     *
     * @param session_id Opaque session identifier from the HTTP-only cookie.
     * @return Session details on success, otherwise an empty optional.
     */
    std::optional<session_t> validate(std::string_view session_id);

    /**
     * @brief Validate a CSRF token bound to an existing session.
     *
     * @param session_id Opaque session identifier from the HTTP-only cookie.
     * @param csrf_token Token supplied in the request header.
     * @return True when both values identify the same unexpired session.
     */
    bool validate_csrf(std::string_view session_id, std::string_view csrf_token);

    /**
     * @brief Remove one session, making its cookie unusable immediately.
     *
     * @param session_id Opaque session identifier to revoke.
     */
    void logout(std::string_view session_id);

    /**
     * @brief Remove every SteamShine session after credentials change.
     */
    void invalidate_all();

  private:
    /**
     * @brief Server-only session state.
     */
    struct session_record_t {
      std::string csrf_token;  ///< CSRF token bound to this session.
      std::string username;  ///< Authenticated username.
      std::chrono::steady_clock::time_point expiration;  ///< Monotonic expiration deadline.
    };

    /**
     * @brief Remove expired records while holding the session mutex.
     *
     * @param now Monotonic time used to evaluate expiration.
     */
    void purge_expired(std::chrono::steady_clock::time_point now);

    std::mutex mutex_;  ///< Mutex protecting session records.
    std::unordered_map<std::string, session_record_t> sessions_;  ///< Server-side sessions indexed by opaque identifier.
    std::chrono::steady_clock::duration lifetime_;  ///< Lifetime assigned to newly created sessions.
  };

  /**
   * @brief Reuses Sunshine's Moonlight pairing PIN backend.
   */
  class PairingService {
  public:
    /**
     * @brief Construct a pairing service using the shared paired-client backend.
     *
     * @param backend Shared backend, or the production NVHTTP backend when omitted.
     */
    explicit PairingService(std::shared_ptr<class PairingClientBackend> backend = {});

    /**
     * @brief Submit a validated PIN for the active Moonlight pairing request.
     *
     * @param pin Four digit PIN supplied by the browser.
     * @param client_name Friendly client name supplied by the browser.
     * @return Non-secret pairing result.
     */
    service_result_t submit_pin(std::string_view pin, std::string_view client_name) const;

  private:
    std::shared_ptr<class PairingClientBackend> backend_;  ///< Shared paired-client backend.
  };

  /**
   * @brief Backend boundary shared by pairing and client-management services.
   *
   * The production implementation delegates to NVHTTP. Tests can provide an
   * in-memory backend without exposing a test control surface in the server.
   */
  class PairingClientBackend {
  public:
    /**
     * @brief Destroy a backend through its virtual interface.
     */
    virtual ~PairingClientBackend() = default;

    /**
     * @brief Submit a PIN for the pending pairing operation.
     *
     * @param pin Four-digit Moonlight PIN.
     * @param client_name Friendly paired-client name.
     * @return True when pairing completed.
     */
    virtual bool submit_pin(std::string_view pin, std::string_view client_name) = 0;

    /**
     * @brief Return paired clients in Sunshine's existing JSON representation.
     *
     * @return Paired-client JSON array.
     */
    virtual nlohmann::json list_clients() const = 0;

    /**
     * @brief Get the certificate associated with one paired client.
     *
     * @param uuid Paired-client identifier.
     * @return Client certificate, or an empty string when absent.
     */
    virtual std::string certificate_for_uuid(std::string_view uuid) const = 0;

    /**
     * @brief Remove one paired client from the backing store.
     *
     * @param uuid Paired-client identifier.
     * @return True when a client was removed.
     */
    virtual bool revoke_client(std::string_view uuid) = 0;
  };

  /**
   * @brief Reuses Sunshine paired-client persistence and revocation logic.
   */
  class ClientService {
  public:
    /**
     * @brief Construct a client-management service using the shared paired-client backend.
     *
     * @param backend Shared backend, or the production NVHTTP backend when omitted.
     */
    explicit ClientService(std::shared_ptr<PairingClientBackend> backend = {});

    /**
     * @brief Return the current paired-client list.
     *
     * @return JSON array in the existing Sunshine client representation.
     */
    nlohmann::json list() const;

    /**
     * @brief Revoke one paired client and terminate its active sessions.
     *
     * @param uuid Paired-client identifier.
     * @return Non-secret revocation result.
     */
    service_result_t revoke(std::string_view uuid) const;

  private:
    std::shared_ptr<PairingClientBackend> backend_;  ///< Shared paired-client backend.
  };

  /**
   * @brief Reuses Sunshine application persistence without exposing file access to handlers.
   */
  class ApplicationService {
  public:
    /**
     * @brief Read the configured application document.
     *
     * @return Application JSON or an empty application list on a read failure.
     */
    nlohmann::json list() const;
  };

  /**
   * @brief Supplies a deliberately bounded configuration snapshot for Web clients.
   */
  class ConfigurationService {
  public:
    /**
     * @brief Return safe Web UI configuration values.
     *
     * @return JSON snapshot without paths or secrets.
     */
    nlohmann::json snapshot() const;

    /**
     * @brief Return verified resident Game Mode Gamescope candidates for explicit selection.
     *
     * @return Bounded candidate metadata and a non-secret discovery result.
     */
    nlohmann::json gamescope_sources() const;

    /**
     * @brief Persist the virtual-display policy for use after the next restart.
     *
     * @param enabled Whether SteamShine virtual-display management is enabled.
     * @param mode Requested canonical mode: off, auto, or force.
     * @param session_source Requested Gamescope source: auto, existing_gamescope, or owned_private.
     * @param local_presentation Requested local presentation: auto, off, or mirror.
     * @param keep_session_alive Whether an owned session remains available after disconnect.
     * @param existing_gamescope_pid Optional current-user resident Gamescope PID; zero selects automatically.
     * @return Non-secret result describing validation or persistence outcome.
     */
    service_result_t save_virtual_display(bool enabled, std::string_view mode, std::string_view session_source, std::string_view local_presentation, bool keep_session_alive, int existing_gamescope_pid) const;
  };

  /**
   * @brief Supplies stream and service state without reading streaming hot paths synchronously.
   */
  class StatusSnapshotService {
  public:
    /**
     * @brief Return a bounded status snapshot.
     *
     * @return JSON snapshot of service and stream state.
     */
    nlohmann::json snapshot() const;
  };

  /**
   * @brief Network class assigned to automatically recorded clients.
   *
   * SteamShine never infers LAN, Wi-Fi, or overlay state from an address, so a
   * client recorded on its first connection is filed under one neutral class
   * until the user names a different one.
   */
  inline constexpr std::string_view default_network_class {"default"};

  /**
   * @brief Bounded per-client and network stream negotiation defaults.
   */
  struct stream_profile_t {
    std::string client_id;  ///< Stable paired-client identifier, never a marketing name.
    std::string network_class;  ///< User-selected network class such as lan or wifi-5ghz.
    std::string capability_signature;  ///< Stable signature of current protocol capabilities.
    std::string geometry_policy {"fit"};  ///< Geometry policy: exact, fit, or virtual_fallback.
    std::string fps_policy {"auto"};  ///< FPS policy: auto or custom.
    int fps_ceiling {0};  ///< Custom FPS ceiling, or zero for automatic.
    std::string codec_policy {"auto"};  ///< Codec policy: auto, h264, hevc, or av1.
    std::string hdr_policy {"auto"};  ///< HDR policy: off, auto, or require.
    int bitrate_ceiling_kbps {0};  ///< Custom video bitrate ceiling, or zero for automatic.
    std::string quality_preset {"balanced"};  ///< Existing-setting preset: low_latency, balanced, or quality.
    std::string orientation {"auto"};  ///< Content orientation: auto, landscape, or portrait.
    int safe_area_percent {0};  ///< Symmetric safe-area inset from zero through twenty-five percent.
    int learned_start_kbps {0};  ///< Learned next-session start target, or zero when unknown.
    bool active {false};  ///< Whether this network class is selected for the client's next connection.
  };

  /**
   * @brief Result of resolving a profile against current client capabilities.
   */
  struct stream_profile_selection_t {
    std::optional<stream_profile_t> profile;  ///< Matching profile, if its capability signature is current.
    std::string reason;  ///< Stable selection or rejection reason.
  };

  /**
   * @brief Result of applying safe profile defaults to one RTSP video request.
   */
  struct stream_profile_application_t {
    bool applied {false};  ///< Whether at least one safe per-session value changed.
    std::vector<std::string> fallback_reasons;  ///< Stable reasons for policies that yielded to the client request.
  };

  /**
   * @brief Atomically persists a bounded client/network stream profile set.
   */
  class StreamProfileService {
  public:
    /**
     * @brief Construct a profile store and load any valid persisted document.
     *
     * @param path Explicit test/production path, or an empty path for the default user state path.
     */
    explicit StreamProfileService(std::filesystem::path path = {});

    /**
     * @brief Return the bounded profile document exposed to authenticated clients.
     *
     * @return Schema-versioned profile array without secrets.
     */
    nlohmann::json snapshot() const;

    /**
     * @brief Validate and atomically save one client/network profile.
     *
     * @param profile Candidate profile.
     * @return Stable validation or persistence result.
     */
    service_result_t save(const stream_profile_t &profile);

    /**
     * @brief Create a neutral profile the first time a paired client connects.
     *
     * Registration only ever adds an entry whose policies are all automatic, so
     * a client that has never been configured behaves exactly as it did before
     * it was recorded. An existing entry is never modified, which preserves the
     * rule that a changed capability signature disables a saved profile instead
     * of silently refreshing it.
     *
     * @param client_id Stable paired-client identifier supplied by the client.
     * @param capability_signature Current protocol capability signature.
     * @return Stable creation, already-present, or persistence result.
     */
    service_result_t ensure_registered(std::string_view client_id, std::string_view capability_signature);

    /**
     * @brief Remove one exact client/network profile.
     *
     * @param client_id Stable paired-client identifier.
     * @param network_class Exact network class.
     * @return Stable reset result.
     */
    service_result_t reset(std::string_view client_id, std::string_view network_class);

    /**
     * @brief Resolve an exact profile without overriding changed capabilities.
     *
     * @param client_id Stable paired-client identifier.
     * @param network_class Current network class.
     * @param capability_signature Current protocol capability signature.
     * @return Matching profile or a stable reason it was ignored.
     */
    stream_profile_selection_t select(std::string_view client_id, std::string_view network_class, std::string_view capability_signature) const;

    /**
     * @brief Resolve the one explicitly active network profile for a client.
     *
     * @param client_id Stable paired-client identifier.
     * @param capability_signature Current protocol capability signature.
     * @return Active matching profile or a stable reason it was ignored.
     */
    stream_profile_selection_t select_active(std::string_view client_id, std::string_view capability_signature) const;

    /**
     * @brief Persist a bounded learned start rate into an exact active profile.
     *
     * @param client_id Stable paired-client identifier.
     * @param network_class Exact selected network class.
     * @param capability_signature Capability signature used by the completed stream.
     * @param learned_start_kbps Bounded aggregate next-session target.
     * @return Stable update, mismatch, or persistence result.
     */
    service_result_t update_learned_start(std::string_view client_id, std::string_view network_class, std::string_view capability_signature, int learned_start_kbps);

  private:
    /**
     * @brief Validate every bounded field in a profile.
     *
     * @param profile Candidate profile.
     * @return Empty string when valid, otherwise a stable error code.
     */
    static std::string validate(const stream_profile_t &profile);

    /**
     * @brief Write the complete bounded document through atomic replacement.
     *
     * @return True when persistence completed.
     */
    bool persist_locked() const;

    std::filesystem::path path_;  ///< Owner-private JSON profile path.
    mutable std::mutex mutex_;  ///< Protects profile reads and atomic replacement.
    std::map<std::pair<std::string, std::string>, stream_profile_t> profiles_;  ///< Profiles keyed by client and network class.
  };

  /**
   * @brief Return the process-wide profile service shared by media policy and Web handlers.
   *
   * @return Owner-private bounded profile service.
   */
  StreamProfileService &stream_profile_service();

  /**
   * @brief Build a stable signature only from current protocol request facts.
   *
   * @param request Parsed RTSP video request before host policy changes.
   * @param hdr_requested Original NVHTTP HDR intent.
   * @return Bounded signature suitable for exact profile matching.
   */
  std::string stream_capability_signature(const video::config_t &request, bool hdr_requested);

  /**
   * @brief Apply only safe, bounded profile defaults without inventing client capabilities.
   *
   * @param profile Exact active profile selected for this request.
   * @param config Mutable per-session video configuration.
   * @return Applied state and stable reasons for policies that yielded to the request.
   */
  stream_profile_application_t apply_stream_profile(const stream_profile_t &profile, video::config_t &config);

  /**
   * @brief Reads a bounded diagnostic log snapshot for Web clients.
   */
  class DiagnosticService {
  public:
    /**
     * @brief Return recent log content without exposing arbitrary file reads.
     *
     * @param maximum_bytes Maximum number of trailing bytes to return.
     * @return Recent configured Sunshine log content.
     */
    std::string recent_logs(std::size_t maximum_bytes = 65536U) const;
  };
}  // namespace web
