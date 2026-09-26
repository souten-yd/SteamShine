/**
 * @file src/steamshine_addons.h
 * @brief Fixed-scope addon status and lifecycle operations for SteamShine.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steamshine_addons {

  /**
   * @brief Supported Decky Loader lifecycle operations.
   */
  enum class decky_action_e {
    install,  ///< Install the latest stable release, preserving plugin data.
    update,  ///< Re-run the stable installer to update or repair Decky Loader.
    uninstall,  ///< Remove Decky Loader while preserving its plugin data.
    start,  ///< Start an installed inactive loader for an owned Gamescope session.
  };

  /**
   * @brief Observable Decky Loader state for the Addon tab.
   */
  struct decky_status_t {
    bool installed {false};  ///< Whether the official PluginLoader executable exists.
    std::string version;  ///< Installed version marker, when available.
    bool service_enabled {false};  ///< Whether plugin_loader.service is enabled.
    bool service_active {false};  ///< Whether plugin_loader.service is active.
    bool management_available {false};  ///< Whether the root-owned fixed-operation helper is installed.
  };

  /**
   * @brief Result of one fixed Decky Loader lifecycle operation.
   */
  struct decky_action_result_t {
    bool success {false};  ///< Whether the official installer operation exited successfully.
    int exit_code {-1};  ///< Normalized helper exit code.
    std::string message;  ///< Bounded helper output suitable for an operator message.
    decky_status_t status;  ///< State sampled after the operation.
  };

  /**
   * @brief Parse an API action without accepting arbitrary command text.
   *
   * @param value API action spelling.
   * @return Supported action, or no value for all other input.
   */
  std::optional<decky_action_e> parse_decky_action(std::string_view value);

  /**
   * @brief Return the fixed privileged-helper arguments for an action.
   *
   * @param action Valid Decky operation.
   * @param helper_path Root-owned helper path.
   * @return Exact argv vector passed without a shell.
   */
  std::vector<std::string> decky_helper_arguments(decky_action_e action, std::string_view helper_path);

  /**
   * @brief Inspect the current Decky Loader installation and system service.
   *
   * @return Current read-only Decky state.
   */
  decky_status_t decky_status();

  /**
   * @brief Decide whether an installed Decky service must be started for owned Gamescope.
   *
   * @param status Current Decky installation and service state.
   * @return True only when Decky is installed but inactive.
   */
  bool decky_start_required(const decky_status_t &status);

  /**
   * @brief Execute one fixed Decky operation through the provisioned helper.
   *
   * @param action Operation selected by the authenticated Addon tab.
   * @return Operation result and freshly sampled state.
   */
  decky_action_result_t perform_decky_action(decky_action_e action);

  /**
   * @brief Start Decky when an owned Gamescope session needs it.
   *
   * Active or uninstalled instances are successful no-ops. A failed helper
   * invocation is logged for Diagnostics but does not reject video streaming.
   *
   * @return True when Decky is absent, already active, or started successfully.
   */
  bool ensure_decky_active_for_owned_session();

  /**
   * @brief Inspect whether the fixed Decky or GPU helper is current and authorized.
   * @param name Fixed helper name.
   * @return True when ready.
   */
  bool management_ready(std::string_view name);

  /**
   * @brief Inspect Web management authorization without prompting.
   * @return Per-feature readiness and local authorization availability.
   */
  nlohmann::json management_status();

  /**
   * @brief Validate a password transport value without logging it.
   * @param password Transient password.
   * @return Whether it fits one bounded stdin line.
   */
  bool management_password_valid(std::string_view password);

  /**
   * @brief Authenticate with sudo over stdin and provision only fixed management operations.
   * @param password Transient administrator password.
   * @return Operation result without credentials.
   */
  nlohmann::json authorize_management(std::string_view password);

  /**
   * @brief Build shell-free arguments for the unprivileged Steam cache helper.
   * @param helper_path Packaged helper selected by the server.
   * @param library_id Empty for status, or a 24-character lowercase hexadecimal library identifier.
   * @return Fixed arguments, or no value when the identifier is invalid.
   */
  std::optional<std::vector<std::string>> steam_cache_helper_arguments(std::string_view helper_path, std::string_view library_id = {});

  /**
   * @brief Inspect Steam library cache locations without changing Steam or stored data.
   * @return JSON status including disconnected libraries and existing cache links.
   */
  nlohmann::json steam_cache_status();

  /**
   * @brief Configure internal shader storage for one registered library while preserving its original data.
   * @param library_id Opaque identifier returned by the status endpoint.
   * @return JSON operation result; active Steam processes and unsafe paths reject the change.
   */
  nlohmann::json configure_steam_cache(std::string_view library_id);

  /**
   * @brief Build fixed storage-helper arguments.
   * @param action Status, remember, or restore.
   * @param uuid UUID required only for restore.
   * @return Arguments or no value for invalid input.
   */
  std::optional<std::vector<std::string>> storage_helper_arguments(std::string_view action, std::string_view uuid = {});

  /**
   * @brief Inspect saved data-volume mounts without mounting disks.
   * @return Recovery status and helper readiness.
   */
  nlohmann::json storage_status();

  /**
   * @brief Save known mount mappings or restore one registered data volume.
   * @param action Remember or restore.
   * @param uuid UUID for restore.
   * @return Safe recovery result.
   */
  nlohmann::json storage_action(std::string_view action, std::string_view uuid = {});

  /**
   * @brief Serialize Decky status to JSON.
   *
   * @param json Destination JSON value.
   * @param value Status to serialize.
   */
  void to_json(nlohmann::json &json, const decky_status_t &value);

  /**
   * @brief Serialize a Decky action result to JSON.
   *
   * @param json Destination JSON value.
   * @param value Result to serialize.
   */
  void to_json(nlohmann::json &json, const decky_action_result_t &value);

}  // namespace steamshine_addons
