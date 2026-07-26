/**
 * @file src/web_services.h
 * @brief Shared application services used by both Sunshine and SteamShine Web UIs.
 */
#pragma once

// standard includes
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// lib includes
#include <nlohmann/json.hpp>

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
     * @brief Persist the virtual-display policy for use after the next restart.
     *
     * @param enabled Whether SteamShine virtual-display management is enabled.
     * @param mode Requested canonical mode: off, auto, or force.
     * @param session_source Requested Gamescope source: auto, existing_gamescope, or owned_private.
     * @param keep_session_alive Whether an owned session remains available after disconnect.
     * @return Non-secret result describing validation or persistence outcome.
     */
    service_result_t save_virtual_display(bool enabled, std::string_view mode, std::string_view session_source, bool keep_session_alive) const;
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
