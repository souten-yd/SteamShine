/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <lizardbyte/common/env.h>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// The SteamShine Terminal WebSocket uses Boost.Beast directly: the shared
// Simple-Web-Server fork used for every other route has no WebSocket support,
// and the sibling Simple-WebSocket-Server project is unmaintained against
// modern Boost.Asio (io_service/get_io_service/expires_from_now were all
// removed upstream). Beast ships with the same Boost release Sunshine
// already depends on, so it stays in lockstep automatically.
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#ifdef _WIN32
  #include "platform/virtualhid_input.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/utf_utils.h"

  #include <Windows.h>
#endif

// local includes
#include "build_info.h"
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "steamshine_addons.h"
#include "steamshine_gpuctl.h"
#include "steamshine_hwmonitor.h"
#include "steamshine_terminal.h"
#include "stream.h"
#include "stream_recording.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "web_services.h"

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;
  const web::CredentialService credential_service {};  ///< Shared Web credential operations.
  web::SessionService session_service {std::chrono::hours(8), web::default_web_session_path()};  ///< Restart-stable SteamShine Web session operations.
  const web::PairingService pairing_service {};  ///< Shared Web pairing operations.
  const web::ClientService client_service {};  ///< Shared Web paired-client operations.
  std::atomic_bool steamshine_lifecycle_pending {false};  ///< Prevent duplicate Web lifecycle requests while shutdown begins.

  std::string steamshine_page_content_security_policy(const std::string_view host_header, const std::uint16_t terminal_ws_port) {
    constexpr std::string_view prefix {"default-src 'self'; connect-src 'self'"};
    constexpr std::string_view suffix {"; style-src 'self' 'unsafe-inline'; style-src-attr 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'self';"};
    std::string_view hostname {host_header};

    if (hostname.starts_with('[')) {
      const auto closing_bracket = hostname.find(']');
      if (closing_bracket == std::string_view::npos) {
        return std::format("{}{}", prefix, suffix);
      }
      const auto port = hostname.substr(closing_bracket + 1);
      if (!port.empty() && (!port.starts_with(':') || port.size() == 1 || !std::ranges::all_of(port.substr(1), [](const unsigned char character) {
            return std::isdigit(character);
          }))) {
        return std::format("{}{}", prefix, suffix);
      }
      hostname = hostname.substr(0, closing_bracket + 1);
    } else if (const auto colon = hostname.rfind(':'); colon != std::string_view::npos) {
      if (hostname.find(':') != colon) {
        return std::format("{}{}", prefix, suffix);
      }
      const auto port = hostname.substr(colon + 1);
      if (port.empty() || !std::ranges::all_of(port, [](const unsigned char character) {
            return std::isdigit(character);
          })) {
        return std::format("{}{}", prefix, suffix);
      }
      hostname = hostname.substr(0, colon);
    }

    const auto valid_hostname_character = [](const unsigned char character) {
      return std::isalnum(character) || character == '.' || character == '-' || character == ':' || character == '[' || character == ']' || character == '%' || character == '_';
    };
    if (hostname.empty() || !std::ranges::all_of(hostname, valid_hostname_character)) {
      return std::format("{}{}", prefix, suffix);
    }

    return std::format("{} wss://{}:{}{}", prefix, hostname, terminal_ws_port, suffix);
  }

  const web::StatusSnapshotService status_snapshot_service {};  ///< Shared Web status operations.
  const web::DiagnosticService diagnostic_service {};  ///< Shared Web diagnostic operations.
  const web::ConfigurationService configuration_service {};  ///< Shared Web configuration operations.

  bool terminal_accept_is_retryable(const boost::system::error_code &error) {
    return error == boost::asio::error::would_block || error == boost::asio::error::try_again;
  }

  bool terminal_peer_is_allowed(const std::string_view address) {
    return net::from_address(address) <= http::origin_web_ui_allowed;
  }

  /**
   * @brief HTTPS server type used for Sunshine's configuration UI.
   */
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to configuration handlers.
   */
  using resp_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by configuration handlers.
   */
  using req_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  /**
   * @brief Handler signature for configuration UI HTTPS routes.
   */
  using https_handler_t = std::function<void(resp_https_t, req_https_t)>;

  namespace {
    using license_status_provider_t = std::function<lvh::LicenseResult()>;  ///< Provider for the current libvirtualhid license status.
#if defined(linux) || defined(__FreeBSD__) || defined(SUNSHINE_TESTS)
    using portal_token_path_provider_t = std::function<fs::path()>;  ///< Provider for the XDG Portal token path.
#endif

    /**
     * @brief Return the current libvirtualhid license status provider.
     *
     * Unit-test builds expose a mutable provider so the HTTP fixture can avoid
     * contacting an installed Windows broker. Production builds keep the
     * provider const and always call libvirtualhid directly.
     *
     * @return License status provider for the current build.
     */
    auto &virtual_input_license_status_provider() {
#ifdef SUNSHINE_TESTS
      static license_status_provider_t status_provider = lvh::get_license_status;
#else
      static const license_status_provider_t status_provider = lvh::get_license_status;
#endif
      return status_provider;
    }

#if defined(linux) || defined(__FreeBSD__) || defined(SUNSHINE_TESTS)
    /**
     * @brief Return the path provider for the saved XDG Portal restore token.
     *
     * @return Path provider for the current build.
     */
    auto &portal_token_path_provider() {
  #ifdef SUNSHINE_TESTS
      static portal_token_path_provider_t path_provider = []() {
        return platf::appdata() / "portal_token";
      };
  #else
      static const portal_token_path_provider_t path_provider = []() {
        return platf::appdata() / "portal_token";
      };
  #endif
      return path_provider;
    }
#endif
  }  // namespace

  /**
   * @brief Client certificate operations accepted by the configuration API.
   */
  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  /**
   * @brief Overwrite a request-local sensitive string when leaving scope.
   */
  class scoped_sensitive_string_clear_t {
  public:
    /**
     * @brief Register a sensitive string for best-effort clearing.
     *
     * @param value Mutable sensitive string.
     */
    explicit scoped_sensitive_string_clear_t(std::string &value):
        value_ {value} {}

    scoped_sensitive_string_clear_t(const scoped_sensitive_string_clear_t &) = delete;
    scoped_sensitive_string_clear_t &operator=(const scoped_sensitive_string_clear_t &) = delete;

    /**
     * @brief Overwrite and clear the registered string.
     */
    ~scoped_sensitive_string_clear_t() {
      std::fill(value_.begin(), value_.end(), '\0');
      value_.clear();
    }

  private:
    std::string &value_;  ///< Sensitive request-local string.
  };

#ifdef SUNSHINE_TESTS
  void set_virtual_input_license_status_provider_for_testing(virtual_input_license_status_provider_t status_provider) {
    virtual_input_license_status_provider() = std::move(status_provider);
  }

  void reset_virtual_input_license_status_provider_for_testing() {
    virtual_input_license_status_provider() = lvh::get_license_status;
  }

  void set_portal_token_path_provider_for_testing(confighttp::portal_token_path_provider_t path_provider) {
    portal_token_path_provider() = std::move(path_provider);
  }

  void reset_portal_token_path_provider_for_testing() {
    portal_token_path_provider() = []() {
      return platf::appdata() / "portal_token";
    };
  }

  void clear_sensitive_string_for_testing(std::string &value) {
    const scoped_sensitive_string_clear_t clear_value {value};
  }
#endif

  // CSRF token management
  /**
   * @brief CSRF token value and its expiration deadline.
   */
  struct csrf_token_t {
    std::string token;  ///< Random token value that must be echoed by the client.
    std::chrono::steady_clock::time_point expiration;  ///< Monotonic deadline after which the token is rejected.
  };

  std::map<std::string, csrf_token_t, std::less<>> csrf_tokens;  ///< CSRF tokens by client identifier. NOSONAR(cpp:S5421) - intentionally mutable global
  std::mutex csrf_tokens_mutex;  ///< Mutex protecting CSRF token storage. NOSONAR(cpp:S5421) - intentionally mutable global

  /**
   * @brief Recent protected-operation timestamps for one remote address.
   */
  struct rate_limit_t {
    std::deque<std::chrono::steady_clock::time_point> attempts;  ///< Attempts still within the limit window.
  };

  std::map<std::string, rate_limit_t, std::less<>> steamshine_login_attempts;  ///< Login attempts by remote address.
  std::map<std::string, rate_limit_t, std::less<>> steamshine_pin_attempts;  ///< PIN attempts by remote address.
  std::map<std::string, rate_limit_t, std::less<>> steamshine_admin_attempts;  ///< Administrator authentication attempts by remote address.
  std::mutex steamshine_rate_limit_mutex;  ///< Mutex protecting SteamShine rate-limit state.

  /**
   * @brief Track live Terminal transports so service shutdown can unblock and join them.
   */
  class terminal_connection_registry_t {
  public:
    /**
     * @brief Register a connection-specific close callback.
     *
     * @param close Callback that cancels and closes the transport.
     * @return Stable non-zero registration identifier.
     */
    std::uint64_t add(std::function<void()> close) {
      std::scoped_lock lock {mutex_};
      const auto id {next_id_++};
      connections_.emplace(id, std::move(close));
      return id;
    }

    /**
     * @brief Remove a connection after its worker exits.
     *
     * @param id Registration identifier returned by add().
     */
    void remove(const std::uint64_t id) {
      std::scoped_lock lock {mutex_};
      connections_.erase(id);
    }

    /**
     * @brief Cancel and close every currently registered transport.
     */
    void close_all() {
      std::vector<std::function<void()>> callbacks;
      {
        std::scoped_lock lock {mutex_};
        callbacks.reserve(connections_.size());
        for (const auto &[id, close] : connections_) {
          (void) id;
          callbacks.push_back(close);
        }
      }
      for (const auto &close : callbacks) {
        close();
      }
    }

  private:
    std::mutex mutex_;  ///< Protects registrations during worker cleanup.
    std::uint64_t next_id_ {1};  ///< Next non-zero registration identifier.
    std::unordered_map<std::uint64_t, std::function<void()>> connections_;  ///< Close callbacks by registration identifier.
  };

  // CSRF token configuration
  /**
   * @brief Number of random bytes used when generating a CSRF token.
   */
  constexpr auto CSRF_TOKEN_SIZE = 32;  // 32 bytes = 256 bits
  /**
   * @brief Amount of time a generated CSRF token remains valid.
   */
  constexpr auto CSRF_TOKEN_LIFETIME = std::chrono::hours(1);  // Tokens valid for 1 hour

  constexpr std::string_view libvirtualhid_minimum_version = LIBVIRTUALHID_MINIMUM_VERSION;  ///< Minimum supported libvirtualhid driver version.
  constexpr auto VIGEMBUS_MINIMUM_VERSION = "1.17.0.0"sv;  ///< Minimum supported ViGEmBus fallback driver version.  // NOSONAR(cpp:S1313): not an IP address

  /**
   * @brief Parse one dotted driver-version component.
   *
   * @param part Version component text.
   * @return Parsed component value, or empty when invalid.
   */
  std::optional<unsigned int> parse_driver_version_part(std::string_view part) {
    if (part.empty()) {
      return std::nullopt;
    }

    unsigned int value = 0;
    const auto *begin = part.data();
    const auto *end = part.data() + part.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {} || ptr != end) {
      return std::nullopt;
    }

    return value;
  }

  /**
   * @brief Parse a dotted driver version into numeric components.
   *
   * @param version Driver version text.
   * @return Parsed version parts, or empty when invalid.
   */
  std::optional<std::vector<unsigned int>> parse_driver_version(std::string_view version) {
    if (version.empty()) {
      return std::nullopt;
    }

    std::vector<unsigned int> parts;
    std::size_t start = 0;
    while (start <= version.size()) {
      const auto dot = version.find('.', start);
      const auto length = dot == std::string_view::npos ? std::string_view::npos : dot - start;
      const auto part = parse_driver_version_part(version.substr(start, length));
      if (!part.has_value()) {
        return std::nullopt;
      }

      parts.push_back(*part);
      if (dot == std::string_view::npos) {
        break;
      }
      start = dot + 1;
    }

    return parts;
  }

  bool is_driver_version_development(std::string_view version) {
    const auto version_parts = parse_driver_version(version);
    return version_parts && version_parts->size() >= 3U && (*version_parts)[0] == 0U && (*version_parts)[1] == 0U;
  }

  bool is_driver_version_supported(std::string_view version, std::string_view minimum_version) {
    if (minimum_version.empty() || is_driver_version_development(version)) {
      return true;
    }

    const auto version_parts = parse_driver_version(version);
    const auto minimum_parts = parse_driver_version(minimum_version);
    if (!version_parts || !minimum_parts) {
      return false;
    }

    const auto part_count = std::max(version_parts->size(), minimum_parts->size());
    for (std::size_t i = 0; i < part_count; ++i) {
      const auto version_part = i < version_parts->size() ? (*version_parts)[i] : 0U;
      const auto minimum_part = i < minimum_parts->size() ? (*minimum_parts)[i] : 0U;
      if (version_part != minimum_part) {
        return version_part > minimum_part;
      }
    }

    return true;
  }

  nlohmann::json build_driver_status(bool installed, const std::string &version, std::string_view minimum_version) {
    const auto minimum_version_text = std::string {minimum_version};

    nlohmann::json output_tree;
    output_tree["installed"] = installed;
    output_tree["version"] = version;
    output_tree["minimum_version"] = minimum_version_text;
    output_tree["supported_versions"] = minimum_version.empty() ? "Any" : std::format(">= {}", minimum_version_text);
    output_tree["development_version"] = installed && is_driver_version_development(version);
    output_tree["version_compatible"] = installed && is_driver_version_supported(version, minimum_version);

    return output_tree;
  }

  /**
   * @brief Return a stable Web UI name for a libvirtualhid license state.
   *
   * @param state License state.
   * @return Lowercase state name.
   */
  std::string_view virtualhid_license_state_name(lvh::LicenseState state) {
    using enum lvh::LicenseState;

    switch (state) {
      case unlicensed:
        return "unlicensed";
      case licensed:
        return "licensed";
      case expired:
        return "expired";
      case disabled:
        return "disabled";
      case invalid:
        return "invalid";
      case unavailable:
      default:
        return "unavailable";
    }
  }

  nlohmann::json build_virtualhid_license_status(const lvh::LicenseResult &result) {
    const auto &license = result.license;
    nlohmann::json output_tree;
    output_tree["operation_ok"] = result.status.ok();
    output_tree["service_available"] = license.service_available;
    output_tree["state"] = virtualhid_license_state_name(license.state);
    output_tree["licensed"] = license.licensed();
    output_tree["active_devices"] = license.active_devices;
    output_tree["activation_limit"] = license.activation_limit;
    output_tree["activation_usage"] = license.activation_usage;
    output_tree["plan_name"] = license.plan_name;
    output_tree["customer_email"] = license.customer_email;
    output_tree["message"] = license.message;
    output_tree["purchase_url"] = license.purchase_url;
    output_tree["manage_account_url"] = license.manage_account_url;
    output_tree["error"] = result.status.ok() ? "" : result.status.message();
    return output_tree;
  }

  namespace {
    /**
     * @brief Handle a virtual-input license request using the configured status provider.
     *
     * @param response HTTP response object.
     * @param request Authenticated HTTP request.
     */
    void get_virtual_input_license(const resp_https_t &response, const req_https_t &request) {
      if (!authenticate(response, request)) {
        return;
      }

      print_req(request);
      send_response(response, build_virtualhid_license_status(virtual_input_license_status_provider()()));
    }
  }  // namespace

#ifdef _WIN32
  /**
   * @brief RAII wrapper for a Windows registry key handle.
   */
  class registry_key_t {
  public:
    /**
     * @brief Construct an empty registry key wrapper.
     */
    registry_key_t() = default;

    /**
     * @brief Copy construction is disabled because the wrapper owns a handle.
     */
    registry_key_t(const registry_key_t &) = delete;

    /**
     * @brief Copy assignment is disabled because the wrapper owns a handle.
     *
     * @return This registry key wrapper.
     */
    registry_key_t &operator=(const registry_key_t &) = delete;

    /**
     * @brief Close the owned registry key handle.
     */
    ~registry_key_t() {
      close();
    }

    /**
     * @brief Get the owned registry key handle.
     *
     * @return Registry key handle.
     */
    HKEY get() const {
      return handle;
    }

    /**
     * @brief Prepare the wrapper to receive a registry key handle.
     *
     * @return Address of the wrapped handle.
     */
    HKEY *put() {
      close();
      return &handle;
    }

  private:
    /**
     * @brief Close the owned registry key handle if one is open.
     */
    void close() {
      if (handle) {
        RegCloseKey(handle);
        handle = nullptr;
      }
    }

    HKEY handle = nullptr;  ///< Owned Windows registry key handle.
  };

  /**
   * @brief Read a string value from a Windows registry key.
   *
   * @param key Registry key to query.
   * @param value_name Registry value name.
   * @return Registry string value, or empty when unavailable.
   */
  std::optional<std::wstring> read_registry_string_value(HKEY key, const wchar_t *value_name) {
    DWORD value_type = 0;
    DWORD value_size = 0;
    if (RegGetValueW(key, nullptr, value_name, RRF_RT_REG_SZ, &value_type, nullptr, &value_size) != ERROR_SUCCESS || value_size == 0) {
      return std::nullopt;
    }

    std::wstring value(value_size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, value_name, RRF_RT_REG_SZ, &value_type, value.data(), &value_size) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    while (!value.empty() && value.back() == L'\0') {
      value.pop_back();
    }
    return value;
  }

  /**
   * @brief Read the installed libvirtualhid driver version from the Windows device registry.
   *
   * @return Driver version string, or empty when unavailable.
   */
  std::string read_libvirtualhid_driver_version() {
    registry_key_t root_key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\ROOT\\LIBVIRTUALHID", 0, KEY_READ, root_key.put()) != ERROR_SUCCESS) {
      return {};
    }

    for (DWORD index = 0;; ++index) {
      std::wstring subkey_name(256, L'\0');
      auto subkey_name_size = static_cast<DWORD>(subkey_name.size());
      const auto enum_status = RegEnumKeyExW(root_key.get(), index, subkey_name.data(), &subkey_name_size, nullptr, nullptr, nullptr, nullptr);
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      std::wstring device_key_path = L"SYSTEM\\CurrentControlSet\\Enum\\ROOT\\LIBVIRTUALHID\\";
      device_key_path.append(subkey_name, 0, subkey_name_size);

      registry_key_t device_key;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, device_key_path.c_str(), 0, KEY_READ, device_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      const auto driver_key_suffix = read_registry_string_value(device_key.get(), L"Driver");
      if (!driver_key_suffix) {
        continue;
      }

      std::wstring driver_key_path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
      driver_key_path += *driver_key_suffix;

      registry_key_t driver_key;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, driver_key_path.c_str(), 0, KEY_READ, driver_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      if (const auto version = read_registry_string_value(driver_key.get(), L"DriverVersion")) {
        return utf_utils::to_utf8(*version);
      }
    }

    return {};
  }

#endif

  /**
   * @brief Allow a bounded number of sensitive requests in a rolling time window.
   *
   * @param attempts Request timestamps indexed by remote address.
   * @param address Normalized remote address.
   * @param maximum_attempts Maximum permitted requests in the window.
   * @param window Rolling time window.
   * @return True when this request is allowed and recorded.
   */
  bool consume_steamshine_rate_limit(std::map<std::string, rate_limit_t, std::less<>> &attempts, const std::string_view address, const std::size_t maximum_attempts, const std::chrono::steady_clock::duration window) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(steamshine_rate_limit_mutex);
    auto &entry = attempts[std::string {address}].attempts;
    while (!entry.empty() && entry.front() <= now - window) {
      entry.pop_front();
    }
    if (entry.size() >= maximum_attempts) {
      return false;
    }
    entry.emplace_back(now);
    return true;
  }

  /**
   * @brief Check whether one remote address has exhausted a rolling request limit.
   *
   * Expired entries are removed while checking. The current request is not
   * recorded, which allows callers to count only failed operations.
   *
   * @param attempts Request timestamps indexed by remote address.
   * @param address Normalized remote address.
   * @param maximum_attempts Maximum permitted failed requests in the window.
   * @param window Rolling time window.
   * @return True when the address must be rejected.
   */
  bool steamshine_rate_limit_exhausted(std::map<std::string, rate_limit_t, std::less<>> &attempts, const std::string_view address, const std::size_t maximum_attempts, const std::chrono::steady_clock::duration window) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(steamshine_rate_limit_mutex);
    auto &entry = attempts[std::string {address}].attempts;
    while (!entry.empty() && entry.front() <= now - window) {
      entry.pop_front();
    }
    return entry.size() >= maximum_attempts;
  }

  /**
   * @brief Record one failed protected operation for a remote address.
   *
   * @param attempts Request timestamps indexed by remote address.
   * @param address Normalized remote address.
   */
  void record_steamshine_rate_limit_failure(std::map<std::string, rate_limit_t, std::less<>> &attempts, const std::string_view address) {
    std::scoped_lock lock(steamshine_rate_limit_mutex);
    attempts[std::string {address}].attempts.emplace_back(std::chrono::steady_clock::now());
  }

  /**
   * @brief Clear failed protected operations after successful authentication.
   *
   * @param attempts Request timestamps indexed by remote address.
   * @param address Normalized remote address.
   */
  void clear_steamshine_rate_limit(std::map<std::string, rate_limit_t, std::less<>> &attempts, const std::string_view address) {
    std::scoped_lock lock(steamshine_rate_limit_mutex);
    attempts.erase(std::string {address});
  }

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(const resp_https_t &response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a SteamShine facade response with its dedicated security policy.
   *
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   * @param headers Additional response headers, such as session cookies.
   * @param status HTTP response status.
   */
  void send_steamshine_response(
    const resp_https_t &response,
    const nlohmann::json &output_tree,
    SimpleWeb::CaseInsensitiveMultimap headers = {},
    const SimpleWeb::StatusCode status = SimpleWeb::StatusCode::success_ok
  ) {
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self';");
    headers.emplace("Cache-Control", "no-store");
    response->write(status, output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;

    constexpr auto code = SimpleWeb::StatusCode::client_error_unauthorized;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";

    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"WWW-Authenticate", R"(Basic realm="Sunshine Gamestream Host", charset="UTF-8")"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(const resp_https_t &response, const req_https_t &request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Authenticate the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if the user is authenticated, false otherwise.
   */
  bool authenticate(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());

    if (const auto ip_type = net::from_address(address); ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    const auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    const auto &rawAuth = auth->second;
    auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));

    const auto index = static_cast<int>(authData.find(':'));
    if (index >= authData.size() - 1) {
      return false;
    }

    const auto username = authData.substr(0, index);
    const auto password = authData.substr(index + 1);

    if (const auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string(); !boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message to include in the response.
   */
  void not_found(const resp_https_t &response, [[maybe_unused]] const req_https_t &request, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_not_found;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message to include in the response.
   */
  void bad_request(const resp_https_t &response, [[maybe_unused]] const req_https_t &request, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_bad_request;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send a bad request when mismatched.
   */
  bool check_content_type(const resp_https_t &response, const req_https_t &request, const std::string_view &contentType) {
    const auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }
    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    if (const size_t semicolonPos = actualContentType.find(';'); semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;
  }

  /**
   * @brief Get a unique client identifier for CSRF token management.
   * @param request The HTTP request object.
   * @return A unique identifier based on username or IP address.
   */
  std::string get_client_id(const req_https_t &request) {
    // Try to use the authenticated username as client ID
    if (const auto auth = request->header.find("authorization"); !config::sunshine.username.empty() && auth != request->header.end()) {
      if (const auto &rawAuth = auth->second; rawAuth.rfind("Basic "sv, 0) == 0) {
        auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));
        if (const auto index = static_cast<int>(authData.find(':')); index < authData.size() - 1) {
          return authData.substr(0, index);  // Return username
        }
      }
    }

    // Fall back to IP address if no username
    return net::addr_to_normalized_string(request->remote_endpoint().address());
  }

  /**
   * @brief Generate a new CSRF token for a client.
   * @param client_id A unique identifier for the client (e.g., session ID or username).
   * @return The generated CSRF token.
   */
  std::string generate_csrf_token(const std::string &client_id) {
    // Generate a cryptographically secure random token
    std::string token = crypto::rand_alphabet(CSRF_TOKEN_SIZE);

    std::scoped_lock lock(csrf_tokens_mutex);

    // Clean up expired tokens first
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(csrf_tokens, [&now](const auto &entry) {
      return entry.second.expiration < now;
    });

    // Store the token with expiration
    csrf_tokens[client_id] = csrf_token_t {
      token,
      now + CSRF_TOKEN_LIFETIME
    };

    return token;
  }

  /**
   * @brief Validate a stored CSRF token for a client against a provided token string.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param client_id A unique identifier for the client.
   * @param provided_token The token string to validate.
   * @return True if the token is valid, false otherwise.
   */
  bool validate_stored_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string_view client_id, const std::string_view provided_token) {
    std::scoped_lock lock(csrf_tokens_mutex);
    const auto token_it = csrf_tokens.find(client_id);

    if (token_it == csrf_tokens.end()) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: no token found for client"sv;
      bad_request(response, request, "Invalid CSRF token");
      return false;
    }

    if (const auto now = std::chrono::steady_clock::now(); token_it->second.expiration < now) {
      csrf_tokens.erase(token_it);
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token expired"sv;
      bad_request(response, request, "CSRF token expired");
      return false;
    }

    if (token_it->second.token != provided_token) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token mismatch"sv;
      bad_request(response, request, "Invalid CSRF token");
      return false;
    }

    return true;
  }

  /**
   * @brief Validate CSRF token.
   */
  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id) {
    // Helper function to check if a URL starts with any allowed origin
    auto is_allowed_origin = [](const std::string_view url) {
      return std::ranges::any_of(config::sunshine.csrf_allowed_origins, [&url](const std::string &allowed_origin) {
        // Ensure exact prefix match (with ":" or "/" after to prevent malicious.com matching allowed.com)
        if (url.rfind(allowed_origin, 0) != 0) {  // rfind with pos=0 checks if the url starts with allowed_origin
          return false;
        }
        // Check that it's followed by ":" (port) or "/" (path) or is an exact match
        const size_t len = allowed_origin.length();
        return url.length() == len || url[len] == ':' || url[len] == '/';
      });
    };

    // Check if the request is from the same origin (Origin or Referer header matches configured allowed origins)
    const auto origin_it = request->header.find("Origin");
    if (origin_it != request->header.end() && is_allowed_origin(origin_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If we have a Referer header, check if it's same-origin
    const auto referer_it = request->header.find("Referer");
    if (referer_it != request->header.end() && is_allowed_origin(referer_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If neither Origin nor Referer is present, this cannot be a browser-initiated CSRF attack.
    // Non-browser clients (e.g. curl, scripts) never send these headers, and a malicious web page
    // cannot cause a non-browser client to make requests on a user's behalf.
    if (origin_it == request->header.end() && referer_it == request->header.end()) {
      return true;
    }

    // A browser-like request arrived with an Origin/Referer that doesn't match an allowed origin.
    // Require a CSRF token.
    const std::string_view blocked_origin = (origin_it != request->header.end()) ? origin_it->second : referer_it->second;
    // Extract token from X-CSRF-Token header
    const auto header_it = request->header.find("X-CSRF-Token");
    if (header_it == request->header.end()) {
      // Also check query parameters as fallback
      auto query_params = request->parse_query_string();
      const auto query_it = query_params.find("csrf_token");
      if (query_it == query_params.end()) {
        auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
        BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF protection blocked request from origin: "sv << blocked_origin;
        BOOST_LOG(error) << "Web UI: To allow this origin, add it to the 'csrf_allowed_origins' option in your Sunshine configuration"sv;
        bad_request(response, request, "Missing CSRF token");
        return false;
      }

      return validate_stored_csrf_token(response, request, client_id, query_it->second);
    }

    // Validate token from header
    return validate_stored_csrf_token(response, request, client_id, header_it->second);
  }

  /**
   * @brief Validates the application index and sends an error response if invalid.
   */
  bool check_app_index(const resp_https_t &response, const req_https_t &request, int index) {
    std::string file = file_handler::read_file(config::stream.file_apps.c_str());
    nlohmann::json file_tree = nlohmann::json::parse(file);
    if (const auto &apps = file_tree["apps"]; index < 0 || index >= static_cast<int>(apps.size())) {
      std::string error;
      if (const int max_index = static_cast<int>(apps.size()) - 1; max_index < 0) {
        error = "No applications found";
      } else {
        error = std::format("'index' {} out of range, max index is {}", index, max_index);
      }
      bad_request(response, request, error);
      return false;
    }
    return true;
  }

  /**
   * @brief Get an HTML page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param html_file The HTML file to serve (relative to WEB_DIR).
   * @param require_auth Whether to require authentication (default: true).
   * @param redirect_if_username If true, redirect to "/" when the username is set (for welcome page).
   */
  void getPage(const resp_https_t &response, const req_https_t &request, const char *html_file, const bool require_auth, const bool redirect_if_username) {
    // Special handling for welcome page: redirect if the username is already set
    if (redirect_if_username && !config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    if (require_auth && !authenticate(response, request)) {
      return;
    }

    print_req(request);

    const std::string content = file_handler::read_file((std::string(WEB_DIR) + html_file).c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");

    // prevent click jacking
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getSunshineLogoImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getFaviconImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/sunshine.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Sunshine logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getSunshineLogoImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-sunshine-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAsset(const resp_https_t &response, const req_https_t &request) {
    print_req(request);
    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if the file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }
    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    // check if the extension is in the map at the x position
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }

    // if it is, set the content type to the mime type
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Return the SteamShine single-page frontend without upstream Basic authentication.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @note Inline scripts remain forbidden. Inline styles are allowed because live metric bars and
   *       the bundled xterm renderer create style elements and update geometry at runtime.
   */
  void getSteamshinePage(const resp_https_t &response, const req_https_t &request) {
    if (!config::sunshine.steamshine_web_ui_enabled) {
      not_found(response, request);
      return;
    }
    print_req(request);
    const std::string content = file_handler::read_file((std::string(SUNSHINE_ASSETS_DIR) + "/steamshine/index.html").c_str());
    const auto host = request->header.find("Host");
    const auto content_security_policy = steamshine_page_content_security_policy(
      host == request->header.end() ? std::string_view {} : std::string_view {host->second},
      net::map_port(PORT_STEAMSHINE_TERMINAL)
    );
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "text/html; charset=utf-8"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", content_security_policy},
      {"Cache-Control", "no-store"}
    };
    response->write(content, headers);
  }

  /**
   * @brief Return a path-safe static SteamShine frontend asset.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getSteamshineAsset(const resp_https_t &response, const req_https_t &request) {
    if (!config::sunshine.steamshine_web_ui_enabled) {
      not_found(response, request);
      return;
    }
    print_req(request);
    const fs::path root = fs::weakly_canonical(fs::path {SUNSHINE_ASSETS_DIR} / "steamshine");
    // Resolve the requested path (which may reference nested static assets such as
    // images/*.png or vendor/xterm/*.js) against root and reject any traversal outside it.
    const fs::path relative = fs::path {request->path}.lexically_relative("/steamshine");
    const fs::path requested = fs::weakly_canonical(root / relative);
    if (!isChildPath(requested, root) || !fs::exists(requested) || !fs::is_regular_file(requested)) {
      not_found(response, request);
      return;
    }
    const auto mime_type = mime_types.find(requested.extension().string().substr(1));
    if (mime_type == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", mime_type->second},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none';"},
      {"Cache-Control", "no-store"}
    };
    std::ifstream input {requested, std::ios::binary};
    response->write(SimpleWeb::StatusCode::success_ok, input, headers);
  }

  /**
   * @brief Get a CSRF token for the authenticated user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/csrf-token| GET| null}
   */
  void getCSRFToken(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string client_id = get_client_id(request);
    std::string token = generate_csrf_token(client_id);

    nlohmann::json output_tree;
    output_tree["csrf_token"] = token;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      // Legacy versions of Sunshine used strings for boolean and integers, let's convert them
      // List of keys to convert to boolean
      const std::vector<std::string> boolean_keys = {
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all"
      };

      // List of keys to convert to integers
      std::vector<std::string> integer_keys = {
        "exit-timeout"
      };

      // Walk fileTree and convert true/false strings to boolean or integer values
      for (auto &app : file_tree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = app[key] == "true";
          }
        }
        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = std::stoi(app[key].get<std::string>());
          }
        }
        if (app.contains("prep-cmd")) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              prep["elevated"] = prep["elevated"] == "true";
            }
          }
        }
      }

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application, the index must be `-1`. To update an existing application, you must provide the current index of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "index": -1,
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file."
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","index":-1}}
   */
  void saveApp(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      BOOST_LOG(info) << file;
      nlohmann::json file_tree = nlohmann::json::parse(file);

      if (input_tree["prep-cmd"].empty()) {
        input_tree.erase("prep-cmd");
      }

      if (input_tree["detached"].empty()) {
        input_tree.erase("detached");
      }

      auto &apps_node = file_tree["apps"];
      int index = input_tree["index"].get<int>();  // this will intentionally cause an exception if the provided value is the wrong type

      input_tree.erase("index");

      if (index == -1) {
        apps_node.push_back(input_tree);
      } else {
        nlohmann::json newApps = nlohmann::json::array();
        for (size_t i = 0; i < apps_node.size(); ++i) {
          if (i == index) {
            newApps.push_back(input_tree);
          } else {
            newApps.push_back(apps_node[i]);
          }
        }
        file_tree["apps"] = newApps;
      }

      // Sort the apps array by name
      std::sort(apps_node.begin(), apps_node.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/9999| DELETE| null}
   */
  void deleteApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    try {
      nlohmann::json output_tree;
      nlohmann::json new_apps = nlohmann::json::array();
      const int index = std::stoi(request->path_match[1]);

      if (!check_app_index(response, request, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      for (size_t i = 0; i < apps.size(); ++i) {
        if (i != index) {
          new_apps.push_back(apps[i]);
        }
      }
      file_tree["apps"] = new_apps;

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      output_tree["result"] = std::format("application {} deleted", index);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const nlohmann::json named_certs = client_service.list();

    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Enable or disable a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "enabled": true
   * }
   * @endcode
   *
   * @api_examples{/api/clients/update| POST| {"uuid":"<uuid>","enabled":true}}
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      bool enabled = input_tree.value("enabled", true);
      output_tree["status"] = nvhttp::set_client_enabled(uuid, enabled);

      if (!enabled && output_tree["status"]) {
        auto cert = nvhttp::get_cert_by_uuid(uuid);
        if (!cert.empty()) {
          rtsp_stream::terminate_sessions_by_cert(cert);
        }

        if (rtsp_stream::session_count() == 0 && proc::proc.running() > 0) {
          proc::proc.terminate();
        }
      }

      send_response(response, output_tree);
    } catch (nlohmann::json::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      nlohmann::json output_tree;
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string uuid = input_tree.value("uuid", "");
      const auto result = client_service.revoke(uuid);
      output_tree["status"] = result.success;
      output_tree["code"] = result.code;
      if (!result.success) {
        output_tree["error"] = result.message;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/config| GET| null}
   */
  void getConfig(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = build_info::version();

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));

    for (auto &[name, value] : vars) {
      output_tree[name] = std::move(value);
    }

    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting. This endpoint does not require authentication.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(const resp_https_t &response, const req_https_t &request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      // GPU profiles are managed by their own API. A stale upstream settings
      // page must not replace newly saved profiles with its earlier snapshot.
      const auto persisted = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (const auto *key : {"steamshine_gpu_profiles", "steamshine_gpu_active_profile"}) {
        input_tree.erase(key);
        if (const auto entry = persisted.find(key); entry != persisted.end()) {
          input_tree[key] = entry->second;
        }
      }
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config, right now
        // we should migrate the config file to straight JSON and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get an application's image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @note{The index in the url path is the application index.}
   *
   * @api_examples{/api/covers/9999 | GET| null}
   */
  void getCover(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const int index = std::stoi(request->path_match[1]);
      if (!check_app_index(response, request, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      auto &app = apps[index];

      // Get the image path from the app configuration
      std::string app_image_path;
      if (app.contains("image-path") && !app["image-path"].is_null()) {
        app_image_path = app["image-path"];
      }

      // Use validate_app_image_path to resolve and validate the path
      // This handles extension validation, PNG signature validation, and path resolution
      std::string validated_path = proc::validate_app_image_path(app_image_path);

      // Check if we got the default image path (means validation failed or no image configured)
      if (validated_path == DEFAULT_APP_IMAGE_PATH) {
        BOOST_LOG(debug) << "Application at index " << index << " does not have a valid cover image";
        not_found(response, request, "Cover image not found");
        return;
      }

      // Open and stream the validated file
      std::ifstream in(validated_path, std::ios::binary);
      if (!in) {
        BOOST_LOG(warning) << "Unable to read cover image file: " << validated_path;
        bad_request(response, request, "Unable to read cover image file");
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "image/png");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "igdb_<game_id>",
   *   "url": "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/<slug>.png"
   * }
   * @endcode
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);

      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);

      std::basic_string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));

        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const std::string content = diagnostic_service.recent_logs();
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!config::sunshine.username.empty() && !authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      const auto result = credential_service.save(
        input_tree.value("currentUsername", ""),
        input_tree.value("currentPassword", ""),
        input_tree.value("newUsername", input_tree.value("currentUsername", "")),
        input_tree.value("newPassword", ""),
        input_tree.value("confirmNewPassword", "")
      );
      if (!result.success) {
        bad_request(response, request, result.message);
        return;
      }
      session_service.invalidate_all();
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief List client pairing requests that are waiting for PIN approval.
   *
   * @api_examples{/api/pin| GET| null}
   */
  void getPendingPairings(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["pairings"] = nlohmann::json::array();
    for (const auto &pairing : nvhttp::get_pending_pairings()) {
      output_tree["pairings"].push_back({
        {"id", pairing.id},
        {"name", pairing.name},
        {"address", pairing.address},
      });
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Cancel a client pairing request that is waiting for PIN approval.
   * The body for the delete request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| DELETE| {"pairing_id":"0123456789abcdef0123456789abcdef"}}
   */
  void cancelPairing(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    const std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string pairing_id = input_tree.value("pairing_id", "");
      if (!nvhttp::is_valid_pairing_id(pairing_id)) {
        bad_request(response, request, "pairing_id must contain exactly 32 hexadecimal characters");
        return;
      }

      nlohmann::json output_tree;
      output_tree["status"] = nvhttp::cancel_pairing(pairing_id);
      send_response(response, output_tree);
    } catch (nlohmann::json::exception &e) {
      BOOST_LOG(warning) << "CancelPairing: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Submit a PIN and return whether the selected client completes pairing.
   *
   * The request remains open for up to the configured `ping_timeout` while
   * Moonlight completes the cryptographic handshake. A wrong PIN, protocol
   * failure, cancellation, or timeout returns `{"status":false}`.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>",
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pairing_id":"0123456789abcdef0123456789abcdef","pin":"1234","name":"My PC"}}
   */
  void savePin(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string pairing_id = input_tree.value("pairing_id", "");
      const std::string pin = input_tree.value("pin", "");
      const std::string name = input_tree.value("name", "");
      if (!nvhttp::is_valid_pairing_id(pairing_id) || !nvhttp::is_valid_pairing_pin(pin) || !nvhttp::is_valid_pairing_name(name)) {
        bad_request(response, request, "A pending pairing ID, four-digit PIN, and valid client name are required");
        return;
      }
      const auto result = pairing_service.submit_pin(pairing_id, pin, name);
      output_tree["status"] = result.success;
      output_tree["code"] = result.code;
      if (!result.success) {
        output_tree["error"] = result.message;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Maximum accepted SteamShine facade JSON request size.
   */
  constexpr auto STEAMSHINE_MAX_REQUEST_BYTES = 65536U;

  /**
   * @brief Obtain one named HTTP cookie without logging its value.
   *
   * @param request The HTTP request object.
   * @param name Cookie name to find.
   * @return Cookie value or an empty string when it was not present.
   */
  std::string get_cookie_value(const req_https_t &request, const std::string_view name) {
    const auto cookie_header = request->header.find("cookie");
    if (cookie_header == request->header.end()) {
      return {};
    }
    std::string_view cookies {cookie_header->second};
    while (!cookies.empty()) {
      const auto separator = cookies.find(';');
      const auto cookie = cookies.substr(0, separator);
      const auto equals = cookie.find('=');
      const auto cookie_name = cookie.substr(0, equals);
      if (equals != std::string_view::npos && boost::algorithm::trim_copy(std::string {cookie_name}) == name) {
        return std::string {cookie.substr(equals + 1)};
      }
      if (separator == std::string_view::npos) {
        break;
      }
      cookies.remove_prefix(separator + 1);
    }
    return {};
  }

  /**
   * @brief Reject a SteamShine facade request whose browser origin differs from its host.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param required Whether a missing Origin header is rejected.
   * @return True when the request origin is acceptable.
   */
  bool validate_steamshine_origin(const resp_https_t &response, const req_https_t &request, const bool required) {
    const auto origin = request->header.find("origin");
    if (origin == request->header.end()) {
      if (!required) {
        return true;
      }
      bad_request(response, request, "Missing Origin header");
      return false;
    }
    const auto host = request->header.find("host");
    if (host == request->header.end() || origin->second != "https://" + host->second) {
      bad_request(response, request, "Origin mismatch");
      return false;
    }
    return true;
  }

  /**
   * @brief Obtain one named cookie from a SteamShine Terminal WebSocket handshake request.
   *
   * Mirrors get_cookie_value(), duplicated because the pre-handshake request
   * here is a Boost.Beast HTTP request rather than a Simple-Web-Server one.
   *
   * @param cookie_header Raw `Cookie` header value, or empty when absent.
   * @param name Cookie name to find.
   * @return Cookie value or an empty string when it was not present.
   */
  std::string get_ws_cookie_value(std::string_view cookies, const std::string_view name) {
    if (cookies.empty()) {
      return {};
    }
    while (!cookies.empty()) {
      const auto separator = cookies.find(';');
      const auto cookie = cookies.substr(0, separator);
      const auto equals = cookie.find('=');
      const auto cookie_name = cookie.substr(0, equals);
      if (equals != std::string_view::npos && boost::algorithm::trim_copy(std::string {cookie_name}) == name) {
        return std::string {cookie.substr(equals + 1)};
      }
      if (separator == std::string_view::npos) {
        break;
      }
      cookies.remove_prefix(separator + 1);
    }
    return {};
  }

  /**
   * @brief Read a bounded JSON request body for a SteamShine facade handler.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param output Parsed JSON object written on success.
   * @return True when the body is JSON and within the allowed size.
   */
  bool read_steamshine_json(const resp_https_t &response, const req_https_t &request, nlohmann::json &output) {
    if (!check_content_type(response, request, "application/json")) {
      return false;
    }
    std::string content {std::istreambuf_iterator<char> {request->content}, {}};
    if (content.size() > STEAMSHINE_MAX_REQUEST_BYTES) {
      bad_request(response, request, "Request body is too large");
      return false;
    }
    try {
      output = nlohmann::json::parse(content);
      return output.is_object();
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "Malformed JSON");
      return false;
    }
  }

  /**
   * @brief Authorize a SteamShine facade request using its HTTP-only session cookie.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return Validated session identifier, or an empty string after responding with an error.
   */
  std::string require_steamshine_session(const resp_https_t &response, const req_https_t &request) {
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (net::from_address(address) > http::origin_web_ui_allowed) {
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return {};
    }
    const auto session_id = get_cookie_value(request, "steamshine_session");
    if (!session_service.validate(session_id).has_value()) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      return {};
    }
    return session_id;
  }

  /**
   * @brief Require a valid SteamShine session and matching CSRF header for a mutation.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return Validated session identifier, or an empty string after responding with an error.
   */
  std::string require_steamshine_mutation(const resp_https_t &response, const req_https_t &request) {
    if (!validate_steamshine_origin(response, request, true)) {
      return {};
    }
    const auto session_id = require_steamshine_session(response, request);
    const auto csrf_token = request->header.find("x-steamshine-csrf-token");
    if (session_id.empty() || csrf_token == request->header.end() || !session_service.validate_csrf(session_id, csrf_token->second)) {
      if (!session_id.empty()) {
        bad_request(response, request, "Invalid CSRF token");
      }
      return {};
    }
    BOOST_LOG(info) << "SteamShine Web action: " << request->method << ' ' << request->path;
    return session_id;
  }

  /**
   * @brief Request Web administrator authentication only when a required helper is unavailable.
   * @param response HTTP response.
   * @param helper Fixed helper name.
   * @return Whether the operation may proceed.
   */
  bool require_steamshine_management(const resp_https_t &response, const std::string_view helper) {
    if (steamshine_addons::management_ready(helper)) {
      return true;
    }
    send_steamshine_response(response, {{"status", false}, {"code", "admin_authorization_required"}, {"message", "Administrator authentication is required."}}, {}, SimpleWeb::StatusCode::client_error_forbidden);
    return false;
  }

  /**
   * @brief Return fixed-operation management readiness.
   * @param response HTTP response.
   * @param request Authenticated request.
   */
  void steamshine_management_status(const resp_https_t &response, const req_https_t &request) {
    if (!require_steamshine_session(response, request).empty()) {
      send_steamshine_response(response, steamshine_addons::management_status());
    }
  }

  /**
   * @brief Authenticate a transient administrator password without logging or saving its contents.
   * @param response HTTP response.
   * @param request Session- and CSRF-protected HTTPS request.
   */
  void steamshine_authorize_management(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    const auto address {net::addr_to_normalized_string(request->remote_endpoint().address())};
    if (steamshine_rate_limit_exhausted(steamshine_admin_attempts, address, 5U, std::chrono::minutes(5))) {
      send_steamshine_response(response, {{"status", false}, {"message", "Too many authentication attempts. Try again in five minutes."}}, {}, SimpleWeb::StatusCode::client_error_too_many_requests);
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    if (!input.contains("password") || !input["password"].is_string()) {
      bad_request(response, request, "An administrator password is required");
      return;
    }
    auto password {input["password"].get<std::string>()};
    input["password"] = nullptr;
    record_steamshine_rate_limit_failure(steamshine_admin_attempts, address);
    const auto result = steamshine_addons::authorize_management(password);
    std::fill(password.begin(), password.end(), '\0');
    if (!result.value("success", false)) {
      send_steamshine_response(response, result, {}, SimpleWeb::StatusCode::client_error_forbidden);
      return;
    }
    clear_steamshine_rate_limit(steamshine_admin_attempts, address);
    steamshine_gpuctl::refresh_capabilities();
    send_steamshine_response(response, result);
  }

  /**
   * @brief Return whether the shared Web credential has been initialized.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_setup_status(const resp_https_t &response, const req_https_t &request) {
    send_steamshine_response(response, {{"configured", credential_service.is_configured()}});
  }

  /**
   * @brief Create the initial shared Web credential through the SteamShine facade.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_setup_credentials(const resp_https_t &response, const req_https_t &request) {
    if (!validate_steamshine_origin(response, request, true) || credential_service.is_configured()) {
      if (credential_service.is_configured()) {
        response->write(SimpleWeb::StatusCode::client_error_forbidden);
      }
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto result = credential_service.save({}, {}, input.value("username", ""), input.value("password", ""), input.value("confirm_password", ""));
    if (result.success) {
      session_service.invalidate_all();
    }
    send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief Authenticate a SteamShine browser and issue its HTTP-only session cookie.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_login(const resp_https_t &response, const req_https_t &request) {
    if (!validate_steamshine_origin(response, request, true)) {
      return;
    }
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (steamshine_rate_limit_exhausted(steamshine_login_attempts, address, 5U, std::chrono::minutes(5))) {
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests);
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto session = session_service.login(credential_service, input.value("username", ""), input.value("password", ""));
    if (!session.has_value()) {
      record_steamshine_rate_limit_failure(steamshine_login_attempts, address);
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      return;
    }
    clear_steamshine_rate_limit(steamshine_login_attempts, address);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Set-Cookie", "steamshine_session=" + session->id + "; Path=/; Secure; HttpOnly; SameSite=Strict");
    send_steamshine_response(response, {{"status", true}, {"username", session->username}, {"csrf_token", session->csrf_token}}, std::move(headers));
  }

  /**
   * @brief Invalidate the current SteamShine browser session.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_logout(const resp_https_t &response, const req_https_t &request) {
    const auto session_id = require_steamshine_mutation(response, request);
    if (session_id.empty()) {
      return;
    }
    session_service.logout(session_id);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Set-Cookie", "steamshine_session=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    send_steamshine_response(response, {{"status", true}}, std::move(headers));
  }

  /**
   * @brief Authenticate and schedule a graceful SteamShine exit or restart.
   *
   * The response is queued before lifecycle cleanup begins so a LAN browser
   * receives confirmation instead of an ambiguous connection reset.
   *
   * @param response The HTTP response object.
   * @param request The authenticated request carrying the CSRF token.
   * @param restart_process True to restart SteamShine; false to exit it.
   */
  void steamshine_lifecycle(const resp_https_t &response, const req_https_t &request, const bool restart_process) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    bool expected {false};
    if (!steamshine_lifecycle_pending.compare_exchange_strong(expected, true)) {
      send_steamshine_response(
        response,
        {{"status", false}, {"message", "A SteamShine lifecycle action is already in progress."}},
        {},
        SimpleWeb::StatusCode::client_error_conflict
      );
      return;
    }
    send_steamshine_response(response, {{"status", true}, {"action", restart_process ? "restart" : "quit"}});
    std::thread {[restart_process] {
      std::this_thread::sleep_for(200ms);
      if (restart_process) {
        platf::restart();
      } else {
        lifetime::exit_sunshine(0, true);
      }
    }}.detach();
  }

  /**
   * @brief Gracefully stop SteamShine from its authenticated management UI.
   *
   * @param response The HTTP response object.
   * @param request The authenticated request carrying the CSRF token.
   */
  void steamshine_quit(const resp_https_t &response, const req_https_t &request) {
    steamshine_lifecycle(response, request, false);
  }

  /**
   * @brief Gracefully restart SteamShine from its authenticated management UI.
   *
   * @param response The HTTP response object.
   * @param request The authenticated request carrying the CSRF token.
   */
  void steamshine_restart(const resp_https_t &response, const req_https_t &request) {
    steamshine_lifecycle(response, request, true);
  }

  /**
   * @brief Return the current SteamShine browser session state.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_session(const resp_https_t &response, const req_https_t &request) {
    const auto session_id = require_steamshine_session(response, request);
    if (session_id.empty()) {
      return;
    }
    const auto session = session_service.validate(session_id);
    send_steamshine_response(response, {{"authenticated", true}, {"username", session->username}, {"csrf_token", session->csrf_token}});
  }

  /**
   * @brief Return shared stream and virtual-display status to SteamShine.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_status(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, status_snapshot_service.snapshot());
  }

  /**
   * @brief Return bounded per-client and network stream profiles.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_stream_profiles(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, web::stream_profile_service().snapshot());
  }

  /**
   * @brief Validate and persist one stream negotiation profile.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_save_stream_profile(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      const web::stream_profile_t profile {
        .client_id = input.at("client_id").get<std::string>(),
        .network_class = input.at("network_class").get<std::string>(),
        .capability_signature = input.at("capability_signature").get<std::string>(),
        .geometry_policy = input.value("geometry_policy", "fit"),
        .fps_policy = input.value("fps_policy", "auto"),
        .fps_ceiling = input.value("fps_ceiling", 0),
        .codec_policy = input.value("codec_policy", "auto"),
        .hdr_policy = input.value("hdr_policy", "auto"),
        .bitrate_ceiling_kbps = input.value("bitrate_ceiling_kbps", 0),
        .quality_preset = input.value("quality_preset", "balanced"),
        .orientation = input.value("orientation", "auto"),
        .safe_area_percent = input.value("safe_area_percent", 0),
        .learned_start_kbps = input.value("learned_start_kbps", 0),
        .active = input.value("active", false),
      };
      const auto result {web::stream_profile_service().save(profile)};
      send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "Invalid stream profile payload");
    }
  }

  /**
   * @brief Reset one exact client/network stream profile.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_reset_stream_profile(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      const auto result {web::stream_profile_service().reset(input.at("client_id").get<std::string>(), input.at("network_class").get<std::string>())};
      send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "Invalid stream profile reset payload");
    }
  }

  /**
   * @brief Return a live CPU/memory/AMD GPU telemetry snapshot to SteamShine.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_system_metrics(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    auto snapshot {steamshine_hwmonitor::sample()};
    const auto profile {steamshine_gpuctl::active_profile()};
    steamshine_hwmonitor::annotate_selected_profile_power_cap(
      snapshot,
      profile ? std::optional<double> {profile->power_cap_watts} : std::nullopt
    );
    send_steamshine_response(response, snapshot);
  }

  /**
   * @brief Coerce a legacy boolean-ish JSON field (bool or `"true"`/`"false"` string) to `bool`.
   */
  bool steamshine_coerce_bool(const nlohmann::json &node, const char *key) {
    if (!node.contains(key)) {
      return false;
    }
    const auto &value = node.at(key);
    if (value.is_boolean()) {
      return value.get<bool>();
    }
    if (value.is_string()) {
      return value.get<std::string>() == "true";
    }
    return false;
  }

  /**
   * @brief Return the configured application list to SteamShine, with a per-entry running flag.
   *
   * Reuses the same `config::stream.file_apps` store as the legacy `/api/apps`
   * endpoint, but under the SteamShine session/CSRF scheme rather than the
   * legacy Basic-auth session so the new frontend does not need to juggle
   * two different authentication mechanisms on one page.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_get_apps(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    try {
      const std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      const nlohmann::json file_tree = nlohmann::json::parse(file);
      const auto apps = file_tree.value("apps", nlohmann::json::array());
      const bool anything_running = proc::proc.running() != 0;
      const std::string running_name = anything_running ? proc::proc.get_last_run_app_name() : std::string {};
      nlohmann::json result = nlohmann::json::array();
      for (std::size_t i = 0; i < apps.size(); ++i) {
        const auto &app = apps[i];
        const auto name = app.value("name", "");
        nlohmann::json entry;
        entry["index"] = static_cast<int>(i);
        entry["name"] = name;
        entry["cmd"] = app.value("cmd", "");
        entry["image-path"] = app.value("image-path", "");
        entry["elevated"] = steamshine_coerce_bool(app, "elevated");
        entry["running"] = anything_running && name == running_name;
        result.push_back(entry);
      }
      send_steamshine_response(response, {{"apps", result}});
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "steamshine_get_apps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Create or update one application entry from the SteamShine Applications page.
   *
   * Only the curated field subset shown on that page (name/cmd/image-path/elevated)
   * is touched; when updating an existing entry, every other key already stored
   * for it (prep-cmd, detached commands, output path, ...) is preserved untouched.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_save_app(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      if (!input.contains("name") || !input["name"].is_string() || input["name"].get<std::string>().empty()) {
        bad_request(response, request, "Missing application name");
        return;
      }
      const int index = input.value("index", -1);
      const std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      if (!file_tree.contains("apps") || !file_tree["apps"].is_array()) {
        file_tree["apps"] = nlohmann::json::array();
      }
      auto &apps_node = file_tree["apps"];
      if (index == -1) {
        nlohmann::json entry;
        entry["name"] = input["name"];
        entry["cmd"] = input.value("cmd", "");
        entry["image-path"] = input.value("image-path", "");
        entry["elevated"] = input.value("elevated", false);
        apps_node.push_back(entry);
      } else {
        if (index < 0 || index >= static_cast<int>(apps_node.size())) {
          bad_request(response, request, "Invalid application index");
          return;
        }
        auto &existing = apps_node[index];
        existing["name"] = input["name"];
        existing["cmd"] = input.value("cmd", "");
        existing["image-path"] = input.value("image-path", "");
        existing["elevated"] = input.value("elevated", false);
      }
      std::sort(apps_node.begin(), apps_node.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
        return a.value("name", "") < b.value("name", "");
      });
      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);
      send_steamshine_response(response, {{"status", true}});
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "steamshine_save_app: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete one application entry from the SteamShine Applications page.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_delete_app(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    try {
      const int index = std::stoi(request->path_match[1]);
      const std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps_node = file_tree["apps"];
      if (index < 0 || index >= static_cast<int>(apps_node.size())) {
        bad_request(response, request, "Invalid application index");
        return;
      }
      apps_node.erase(apps_node.begin() + index);
      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);
      send_steamshine_response(response, {{"status", true}});
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "steamshine_delete_app: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Terminate the currently running application from the SteamShine Applications page.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_close_app(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    proc::proc.terminate();
    send_steamshine_response(response, {{"status", true}});
  }

  /**
   * @brief Return detected AMD GPU/CPU performance-control capabilities to SteamShine.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_gpu_capabilities(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    steamshine_gpuctl::refresh_capabilities();
    auto capabilities {steamshine_gpuctl::capabilities()};
    capabilities.runtime_write_authorized = steamshine_addons::management_ready("runtime");
    send_steamshine_response(response, capabilities);
  }

  /**
   * @brief Return built-in and custom GPU/CPU performance profiles, plus the active one.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_gpu_profiles(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    nlohmann::json profiles = nlohmann::json::array();
    for (const auto &profile : steamshine_gpuctl::builtin_profiles()) {
      profiles.push_back(profile);
    }
    for (const auto &profile : steamshine_gpuctl::custom_profiles()) {
      profiles.push_back(profile);
    }
    send_steamshine_response(response, {{"profiles", profiles}, {"active", steamshine_gpuctl::active_profile_name()}});
  }

  /**
   * @brief Create or update one custom GPU/CPU performance profile.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_save_gpu_profile(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      const auto profile {input.get<steamshine_gpuctl::profile_t>()};
      std::string error;
      if (!steamshine_gpuctl::save_custom_profile(profile, error)) {
        bad_request(response, request, error);
        return;
      }
      send_steamshine_response(response, {{"status", true}});
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "steamshine_save_gpu_profile: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete one custom GPU/CPU performance profile by name.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_delete_gpu_profile(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    std::string error;
    if (!steamshine_gpuctl::delete_custom_profile(http::url_unescape(request->path_match[1]), error)) {
      bad_request(response, request, error);
      return;
    }
    send_steamshine_response(response, {{"status", true}});
  }

  /**
   * @brief Apply one GPU/CPU performance profile by name.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_activate_gpu_profile(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    if (!require_steamshine_management(response, "runtime")) {
      return;
    }
    const auto result {steamshine_gpuctl::activate_profile(http::url_unescape(request->path_match[1]))};
    if (!result.success) {
      bad_request(response, request, result.error.empty() ? "GPU profile could not be applied" : result.error);
      return;
    }
    send_steamshine_response(response, result);
  }

  /**
   * @brief Return Decky Loader installation and service state for the Addon tab.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_decky_status(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, steamshine_addons::decky_status());
  }

  /**
   * @brief Run one allow-listed Decky Loader lifecycle operation.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_decky_action(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto action {steamshine_addons::parse_decky_action(input.value("action", ""))};
    if (!action) {
      bad_request(response, request, "Unsupported Decky Loader action");
      return;
    }
    const auto result {steamshine_addons::perform_decky_action(*action)};
    if (!result.success) {
      bad_request(response, request, result.message);
      return;
    }
    send_steamshine_response(response, result);
  }

  /**
   * @brief Return sender recording state, capacity, usage, and completed files.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_recordings(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, stream_recording::service().snapshot());
  }

  /**
   * @brief Arm or stop lossless sender-side stream recording.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_toggle_recording(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      const auto enabled {input.at("enabled").get<bool>()};
      const auto result {stream_recording::service().set_enabled(enabled)};
      if (!result.success) {
        bad_request(response, request, result.message);
        return;
      }
      if (enabled) {
        stream::request_recording_key_frame();
      }
      send_steamshine_response(response, {{"status", true}, {"code", result.code}, {"message", result.message}});
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "The enabled field must be a boolean.");
    }
  }

  /**
   * @brief Persist and enforce the total completed-recording capacity.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_recording_settings(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      const auto result {stream_recording::service().set_capacity_megabytes(input.at("capacity_mb").get<std::uint64_t>())};
      if (!result.success) {
        bad_request(response, request, result.message);
        return;
      }
      send_steamshine_response(response, {{"status", true}, {"code", result.code}, {"message", result.message}});
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "Recording capacity must be an integer number of megabytes.");
    }
  }

  /**
   * @brief Delete one completed sender recording.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object containing the generated recording identifier.
   */
  void steamshine_delete_recording(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    const std::string id {request->path_match[1].str()};
    const auto result {stream_recording::service().remove(id)};
    if (!result.success) {
      bad_request(response, request, result.message);
      return;
    }
    send_steamshine_response(response, {{"status", true}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief Serve one completed MP4 for inline playback or attachment download.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object containing the identifier and disposition path.
   */
  void steamshine_recording_media(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    const std::string id {request->path_match[1].str()};
    const auto path {stream_recording::service().resolve(id)};
    if (path.empty()) {
      not_found(response, request, "Recording was not found.");
      return;
    }
    std::ifstream input {path, std::ios::binary};
    if (!input) {
      not_found(response, request, "Recording was not found.");
      return;
    }
    const bool download {request->path.ends_with("/download")};
    SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "video/mp4"},
      {"Content-Disposition", std::format("{}; filename=\"{}.mp4\"", download ? "attachment" : "inline", id)},
      {"Cache-Control", "no-store"},
      {"X-Content-Type-Options", "nosniff"},
      {"Content-Security-Policy", "default-src 'none'; frame-ancestors 'none';"},
    };
    response->write(SimpleWeb::StatusCode::success_ok, input, headers);
  }

  /**
   * @brief Report every retained Terminal shell session.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_terminal_status(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    // The Terminal WebSocket listens on its own port (Simple-Web-Server, used
    // for every other route, has no WebSocket support); the frontend needs
    // this to know where to connect since it cannot be the same port as the
    // page it was loaded from.
    nlohmann::json sessions = nlohmann::json::array();
    bool any_running {false};
    for (const auto &session : steamshine_terminal::list()) {
      any_running = any_running || session.running;
      sessions.push_back({
        {"id", session.id},
        {"name", session.name},
        {"explicit_end_token", session.explicit_end_token},
        {"created_at", session.created_at},
        {"running", session.running},
        {"persistent", session.persistent},
      });
    }
    send_steamshine_response(response, {{"running", any_running}, {"sessions", std::move(sessions)}, {"ws_port", net::map_port(PORT_STEAMSHINE_TERMINAL)}});
  }

  /**
   * @brief Create a new independent Terminal shell session.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_terminal_start(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    const auto session_id {steamshine_terminal::create()};
    if (session_id.empty()) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      return;
    }
    send_steamshine_response(response, {{"status", true}, {"running", true}, {"id", session_id}});
  }

  /**
   * @brief Terminate one Terminal shell session.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_terminal_stop(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto session_id {input.value("session_id", "")};
    const auto explicit_end_token {input.value("explicit_end_token", "")};
    const auto explicit_intent {input.value("intent", "")};
    const auto address {net::addr_to_normalized_string(request->remote_endpoint().address())};
    if (session_id.empty() || explicit_intent != "explicit_user_end" || !steamshine_terminal::stop(session_id, explicit_end_token)) {
      BOOST_LOG(info) << "TERMINAL_SESSION_END_REJECTED id=" << session_id
                      << " source=web address=" << address << " reason=stale_or_missing_confirmation";
      bad_request(response, request, "Refresh the Terminal page and confirm End again");
      return;
    }
    BOOST_LOG(info) << "TERMINAL_SESSION_EXPLICIT_END id=" << session_id
                    << " source=web address=" << address;
    send_steamshine_response(response, {{"status", true}});
  }

  /**
   * @brief Return the full Sunshine configuration to the SteamShine Advanced Settings page.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_get_config(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    nlohmann::json output;
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output[name] = std::move(value);
    }
    send_steamshine_response(response, output);
  }

  /**
   * @brief Merge posted keys into the existing Sunshine configuration.
   *
   * Unlike the legacy `/api/config` POST (which intentionally rewrites the
   * whole config file from exactly the keys the full-form Vue editor posts),
   * this endpoint backs a curated settings page that only ever posts a small
   * subset of keys. It therefore merges onto the current on-disk configuration
   * instead of replacing it, so saving from the curated page cannot silently
   * discard settings the page does not show.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_save_config(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    try {
      auto merged = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (const auto &[key, value] : input.items()) {
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
          continue;
        }
        merged[key] = value.is_string() ? value.get<std::string>() : value.dump();
      }
      std::stringstream config_stream;
      for (const auto &[key, value] : merged) {
        config_stream << key << " = " << value << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      send_steamshine_response(response, {{"status", true}});
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "steamshine_save_config: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Return the persisted SteamOS virtual-display policy to SteamShine.
   *
   * @param response HTTPS response to populate.
   * @param request Authenticated HTTPS request.
   */
  void steamshine_virtual_display_config(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, configuration_service.snapshot());
  }

  /**
   * @brief Return verified resident Game Mode Gamescope candidates.
   *
   * @param response HTTPS response to populate.
   * @param request Authenticated HTTPS request.
   */
  void steamshine_gamescope_sources(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, configuration_service.gamescope_sources());
  }

  /**
   * @brief Save the selected SteamOS virtual-display policy for the next restart.
   *
   * @param response HTTPS response to populate.
   * @param request CSRF-protected HTTPS request carrying virtual-display policy fields.
   */
  void steamshine_save_virtual_display_config(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto result = configuration_service.save_virtual_display(
      input.value("enabled", config::steamos_virtual_display.enabled),
      input.value("mode", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.mode)}),
      input.value("session_source", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.session_source)}),
      input.value("local_presentation", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.local_presentation)}),
      input.value("keep_session_alive", config::steamos_virtual_display.keep_session_alive),
      input.value("existing_gamescope_pid", config::steamos_virtual_display.existing_gamescope_pid),
      input.value("steam_migration", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.steam_migration)}),
      input.value("stock_session_handoff", std::string {steamos_virtual_session::to_string(config::steamos_virtual_display.stock_session_handoff)})
    );
    send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief List pending requests for an authenticated SteamShine pairing page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_pending_pairings(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    auto pairings = nlohmann::json::array();
    for (const auto &pairing : nvhttp::get_pending_pairings()) {
      pairings.push_back({{"id", pairing.id}, {"name", pairing.name}, {"address", pairing.address}});
    }
    send_steamshine_response(response, {{"pairings", pairings}});
  }

  /**
   * @brief Submit a Moonlight pairing PIN through the shared pairing service.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_pairing_pin(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (!consume_steamshine_rate_limit(steamshine_pin_attempts, address, 5U, std::chrono::minutes(1))) {
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests);
      return;
    }
    nlohmann::json input;
    if (!read_steamshine_json(response, request, input)) {
      return;
    }
    const auto result = pairing_service.submit_pin(input.value("pairing_id", ""), input.value("pin", ""), input.value("name", ""));
    send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief Return shared paired clients to SteamShine.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_clients(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, {{"named_certs", client_service.list()}});
  }

  /**
   * @brief Revoke one paired client through the shared client service.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_revoke_client(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    constexpr std::string_view prefix = "/api/steamshine/v1/clients/";
    const auto uuid = request->path.substr(prefix.size());
    const auto result = client_service.revoke(uuid);
    send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief Return bounded recent diagnostics to SteamShine.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_recent_logs(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    send_steamshine_response(response, {{"content", diagnostic_service.recent_logs()}});
  }

  /**
   * @brief Return bounded structured diagnostics for people and automated assistants.
   *
   * @param response The HTTP response object.
   * @param request The authenticated request.
   */
  void steamshine_diagnostics(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_session(response, request).empty()) {
      return;
    }
    auto diagnostics = diagnostic_service.snapshot();
    diagnostics["status"] = status_snapshot_service.snapshot();
    send_steamshine_response(response, diagnostics);
  }

  /**
   * @brief Reset the bounded diagnostic history visible to Web clients.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void steamshine_reset_diagnostics(const resp_https_t &response, const req_https_t &request) {
    if (require_steamshine_mutation(response, request).empty()) {
      return;
    }
    const auto result {diagnostic_service.reset_history()};
    send_steamshine_response(response, {{"status", result.success}, {"code", result.code}, {"message", result.message}});
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Authenticate a Web UI request and delete the saved XDG Portal restore token.
   * @details On platforms without XDG Portal capture, this operation succeeds without changing the filesystem.
   *
   * @param response HTTP response used for authentication, CSRF, and status output.
   * @param request HTTP request carrying the client identity and CSRF token.
   *
   * @api_examples{/api/reset-portal-token| POST| null}
   */
  void resetPortalToken(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    bool status = true;
#if defined(linux) || defined(__FreeBSD__)
    std::error_code ec;
    fs::remove(portal_token_path_provider()(), ec);
    if (ec) {
      BOOST_LOG(error) << "Failed to delete XDG Portal restore token: "sv << ec.message();
      status = false;
    }
#endif

    nlohmann::json output_tree;
    output_tree["status"] = status;
    send_response(response, output_tree);
  }

  /**
   * @brief Authenticate a Web UI request and restart the Sunshine process.
   *
   * @param response HTTP response used for authentication or CSRF failures.
   * @param request HTTP request carrying the client identity and CSRF token.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Build libvirtualhid driver version and installation status.
   *
   * @return libvirtualhid driver status JSON.
   */
  nlohmann::json get_virtualhid_driver_status() {
#ifdef _WIN32
    const auto version_str = read_libvirtualhid_driver_version();
    const auto driver_detected = !version_str.empty();
    auto output_tree = build_driver_status(driver_detected, version_str, libvirtualhid_minimum_version);
    bool requires_installed_driver = true;
    std::string backend_name;
    std::string runtime_error_message;

    try {
      const auto runtime = platf::virtualhid::create_runtime();
      if (runtime) {
        const auto &capabilities = runtime->capabilities();
        backend_name = capabilities.backend_name;
        requires_installed_driver = capabilities.requires_installed_driver;
        output_tree = build_driver_status(driver_detected || capabilities.supports_gamepad, version_str, libvirtualhid_minimum_version);
      }
    } catch (const std::bad_alloc &exception) {
      runtime_error_message = exception.what();
    }

    output_tree["backend_name"] = backend_name;
    output_tree["requires_installed_driver"] = requires_installed_driver;
    if (!runtime_error_message.empty()) {
      output_tree["error"] = runtime_error_message;
    }
#else
    auto output_tree = build_driver_status(false, "", libvirtualhid_minimum_version);
    output_tree["error"] = "libvirtualhid driver status is only available on Windows";
    output_tree["backend_name"] = "";
    output_tree["requires_installed_driver"] = false;
#endif

    return output_tree;
  }

  /**
   * @brief Build ViGEmBus fallback driver version and installation status.
   *
   * @return ViGEmBus fallback driver status JSON.
   */
  nlohmann::json get_vigembus_driver_status() {
#ifdef _WIN32
    std::string version_str;

    // Check if ViGEmBus driver exists
    std::string system_root;
    if (!lizardbyte::common::get_env("SystemRoot", system_root)) {
      system_root = "C:\\Windows";
    }
    const std::filesystem::path driver_path = std::filesystem::path(system_root) / "System32" / "drivers" / "ViGEmBus.sys";
    const auto installed = std::filesystem::exists(driver_path);
    if (installed) {
      platf::getFileVersionInfo(driver_path, version_str);
    }

    auto output_tree = build_driver_status(installed, version_str, VIGEMBUS_MINIMUM_VERSION);
#else
    auto output_tree = build_driver_status(false, "", VIGEMBUS_MINIMUM_VERSION);
    output_tree["error"] = "ViGEmBus is only available on Windows";
#endif

    return output_tree;
  }

  /**
   * @brief Get virtual input driver version and installation status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/virtual-input/status| GET| null}
   */
  void getVirtualInputStatus(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["virtualhid"] = get_virtualhid_driver_status();
    output_tree["vigembus"] = get_vigembus_driver_status();
    send_response(response, output_tree);
  }

  /**
   * @brief Get the current libvirtualhid machine license status.
   *
   * @param response HTTP response object.
   * @param request Authenticated HTTP request.
   *
   * @api_examples{/api/virtual-input/license| GET| null}
   */
  void getVirtualInputLicense(const resp_https_t &response, const req_https_t &request) {
    get_virtual_input_license(response, request);
  }

  /**
   * @brief Activate, validate, or deactivate the libvirtualhid machine license.
   *
   * Submitted license keys are used only for the synchronous broker call. They are
   * never logged or saved in Sunshine's configuration, and extracted mutable copies
   * are overwritten before the handler returns.
   *
   * @param response HTTP response object.
   * @param request Authenticated HTTP request with a JSON action.
   *
   * @api_examples{/api/virtual-input/license| POST| {"action":"validate"}}
   */
  void updateVirtualInputLicense(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    const auto client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);
    try {
      std::stringstream content;
      content << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(content);
      const auto action = input_tree.value("action", "");

      lvh::LicenseResult result;
      if (action == "activate") {
        auto license_key = input_tree.value("license_key", "");
        input_tree["license_key"] = "";
        if (license_key.empty()) {
          bad_request(response, request, "License key is required");
          return;
        }

        const scoped_sensitive_string_clear_t clear_license_key {license_key};
        result = lvh::activate_license(license_key);
      } else if (action == "validate") {
        result = lvh::validate_license();
      } else if (action == "deactivate") {
        result = lvh::deactivate_license();
      } else {
        bad_request(response, request, "Unknown license action");
        return;
      }

#ifdef _WIN32
      config::select_all_gamepad_drivers_if_licensed(result.license.licensed());
#endif
#if defined(_WIN32) && defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
      system_tray::update_tray_virtualhid_license(result.license, false);
#endif
#ifdef _WIN32
      if (result.status.ok()) {
        input::refresh_virtual_input();
      }
#endif
      send_response(response, build_virtualhid_license_status(result));
    } catch (const nlohmann::json::exception &) {
      bad_request(response, request, "Invalid license request");
    }
  }

  /**
   * @brief Checks whether a directory entry qualifies as an executable file.
   * @param entry The directory entry to check.
   * @param status The cached file status for the entry.
   * @return True if the file should be included in an executable-type listing.
   */
  bool is_browsable_executable([[maybe_unused]] const fs::directory_entry &entry, [[maybe_unused]] const fs::file_status &status) {
#ifdef _WIN32
    auto ext = entry.path().extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1";
#else
    const auto perms = status.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none ||
           (perms & fs::perms::group_exec) != fs::perms::none ||
           (perms & fs::perms::others_exec) != fs::perms::none;
#endif
  }

#ifdef _WIN32
  /**
   * @brief Builds a JSON array of available Windows drive letters.
   * @return JSON array of drive-letter entries.
   */
  nlohmann::json get_windows_drives() {
    nlohmann::json entries = nlohmann::json::array();
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (drives & (1 << i)) {
        const auto drive_letter = static_cast<char>('A' + i);
        const auto drive_path = std::string(1, drive_letter) + ":\\";
        nlohmann::json entry;
        entry["name"] = drive_path;
        entry["type"] = "directory";
        entry["path"] = drive_path;
        entries.push_back(entry);
      }
    }
    return entries;
  }
#endif

  /**
   * @brief Lists, filters, and sorts the entries of a directory for the browse API.
   * @param dir_path The directory to list.
   * @param type_str Filter type: "directory", "executable", "file", or "any".
   * @return Sorted JSON array of entry objects with name/type/path fields.
   */
  nlohmann::json build_browse_entries(const fs::path &dir_path, const std::string &type_str) {
    nlohmann::json entries = nlohmann::json::array();

    std::error_code iter_ec;
    for (auto it = fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, iter_ec);
         !iter_ec && it != fs::directory_iterator();
         it.increment(iter_ec)) {
      try {
        const auto status = it->status();
        const bool is_dir = fs::is_directory(status);

        if (const bool is_regular = fs::is_regular_file(status); !is_dir && !is_regular) {
          continue;
        }

        // Apply type filter (directories are always included for navigation)
        if (type_str == "directory" && !is_dir) {
          continue;
        }

        if (type_str == "executable" && !is_dir && !is_browsable_executable(*it, status)) {
          continue;
        }

        nlohmann::json file_entry;
        file_entry["name"] = it->path().filename().string();
        file_entry["path"] = it->path().string();
        file_entry["type"] = is_dir ? "directory" : "file";
        entries.push_back(file_entry);
      } catch (const fs::filesystem_error &e) {
        BOOST_LOG(debug) << "BrowseDirectory: skipping entry due to error: "sv << e.what();
      }
    }

    if (iter_ec) {
      BOOST_LOG(debug) << "BrowseDirectory: directory iteration error: "sv << iter_ec.message();
    }

    // Sort: directories first, then files; both case-insensitively alphabetical
    std::sort(entries.begin(), entries.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
      const bool a_dir = (a["type"] == "directory");
      if (const bool b_dir = (b["type"] == "directory"); a_dir != b_dir) {
        return a_dir && !b_dir;
      }
      auto a_name = a["name"].get<std::string>();
      auto b_name = b["name"].get<std::string>();
      boost::algorithm::to_lower(a_name);
      boost::algorithm::to_lower(b_name);
      return a_name < b_name;
    });

    return entries;
  }

  /**
   * @brief Browse the server filesystem.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @note On Windows, an empty or root path returns the list of available drive letters.
   * @note On non-Windows, an empty path defaults to the filesystem root ("/").
   *
   * @api_examples{/api/browse?path=/home/user&type=directory| GET| null}
   */
  void browseDirectory(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const auto query_params = request->parse_query_string();

      std::string path_str;
      if (const auto path_it = query_params.find("path"); path_it != query_params.end()) {
        path_str = path_it->second;
      }

      std::string type_str = "any";
      if (const auto type_it = query_params.find("type"); type_it != query_params.end() && !type_it->second.empty()) {
        type_str = type_it->second;
      }

      nlohmann::json output_tree;

#ifdef _WIN32
      // On Windows with an empty or root path, return the list of available drive letters
      if (path_str.empty() || path_str == "/" || path_str == "\\") {
        output_tree["path"] = "";
        output_tree["parent"] = "";
        output_tree["entries"] = get_windows_drives();
        send_response(response, output_tree);
        return;
      }
#else
      // On non-Windows, default an empty path to the filesystem root
      if (path_str.empty()) {
        path_str = "/";
      }
#endif

      // Normalize the path
      fs::path dir_path = fs::weakly_canonical(fs::path(path_str));

      // If the path points to a file, use its parent directory
      std::error_code ec;
      if (fs::is_regular_file(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      // If the path doesn't exist, try the parent
      if (!fs::exists(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      if (!fs::is_directory(dir_path, ec)) {
        bad_request(response, request, "Path is not a directory");
        return;
      }

      output_tree["path"] = dir_path.string();

      // Determine the parent path for the "Up" navigation
      const fs::path parent = dir_path.parent_path();
#ifdef _WIN32
      // At a drive root (e.g., C:\) the parent equals itself; signal the drive list with an empty string
      output_tree["parent"] = (parent == dir_path) ? "" : parent.string();
#else
      output_tree["parent"] = parent.string();
#endif

      output_tree["entries"] = build_browse_entries(dir_path, type_str);
      send_response(response, output_tree);
    } catch (const fs::filesystem_error &e) {
      BOOST_LOG(warning) << "BrowseDirectory: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Serve one SteamShine Terminal WebSocket connection to completion.
   *
   * Performs the TLS handshake, reads the HTTP upgrade request manually
   * (rather than letting Boost.Beast's `ws.accept()` read it) so the
   * `steamshine_session` cookie can be validated before the WebSocket
   * handshake is allowed to complete, completes the handshake, then blocks
   * reading client frames until the connection closes. No input/output is
   * relayed until the first client message proves CSRF-token possession.
   * Runs on its own joined connection thread; see accept_and_run_ws() in start().
   *
   * @param socket Freshly accepted TCP socket.
   * @param ssl_ctx Shared TLS context (certificate already loaded).
   * @param connections Registry used to close this transport during shutdown.
   */
  void handle_terminal_ws_connection(
    boost::asio::ip::tcp::socket socket,
    boost::asio::ssl::context &ssl_ctx,
    terminal_connection_registry_t &connections
  ) {
    namespace websocket = boost::beast::websocket;
    namespace http = boost::beast::http;
    using terminal_stream_t = websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;
    try {
      const auto remote_address {net::addr_to_normalized_string(socket.remote_endpoint().address())};
      if (!terminal_peer_is_allowed(remote_address)) {
        BOOST_LOG(info) << "SteamShine Terminal: [" << remote_address << "] -- denied";
        return;
      }
      auto ws {std::make_shared<terminal_stream_t>(std::move(socket), ssl_ctx)};
      const auto connection_id {connections.add([weak_ws = std::weak_ptr<terminal_stream_t> {ws}] {
        if (const auto active_ws {weak_ws.lock()}) {
          boost::system::error_code error;
          auto &transport {boost::beast::get_lowest_layer(*active_ws)};
          transport.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
          transport.cancel(error);
          transport.close(error);
        }
      })};

      struct connection_guard_t {
        terminal_connection_registry_t &connections;  ///< Registry holding the live transport.
        std::uint64_t id;  ///< Registration to remove.

        /**
         * @brief Remove the transport from the shutdown registry.
         */
        ~connection_guard_t() {
          connections.remove(id);
        }
      } connection_guard {connections, connection_id};

      ws->next_layer().handshake(boost::asio::ssl::stream_base::server);

      boost::beast::flat_buffer handshake_buffer;
      http::request<http::string_body> request;
      http::read(ws->next_layer(), handshake_buffer, request);

      const std::string cookie_header {request.count(http::field::cookie) ? std::string {request[http::field::cookie]} : std::string {}};
      const auto session_id {get_ws_cookie_value(cookie_header, "steamshine_session")};
      const auto handshake_session {session_service.validate(session_id)};
      if (!config::sunshine.steamshine_web_ui_enabled || !handshake_session.has_value()) {
        http::response<http::string_body> response {http::status::unauthorized, request.version()};
        response.prepare_payload();
        http::write(ws->next_layer(), response);
        return;
      }
      if (!websocket::is_upgrade(request)) {
        http::response<http::string_body> response {http::status::ok, request.version()};
        response.set(http::field::content_type, "text/html; charset=utf-8");
        response.set(http::field::cache_control, "no-store");
        response.set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'");
        response.body() = "<!doctype html><meta charset=utf-8><title>SteamShine Terminal</title><style>body{font:16px system-ui;background:#111;color:#eee;padding:2rem}strong{color:#6ee7b7}</style><p><strong>Terminal connection is trusted.</strong></p><p>Return to SteamShine; it will reconnect automatically.</p>";
        response.prepare_payload();
        http::write(ws->next_layer(), response);
        return;
      }

      ws->binary(true);
      ws->accept(request);

      // Declaration order matters: locals are destroyed in reverse order, so
      // `unsubscribe_guard` (declared last) runs its destructor *first* --
      // before `write_mutex`/`ws` are torn down -- guaranteeing no output
      // callback can still be executing (or start) once they go away.
      auto write_mutex {std::make_shared<std::mutex>()};
      bool authenticated {false};
      std::string terminal_session_id;
      std::uint64_t subscription_id {0};

      struct unsubscribe_guard_t {
        bool &authenticated;
        std::string &terminal_session_id;
        std::uint64_t &subscription_id;

        ~unsubscribe_guard_t() {
          if (authenticated) {
            steamshine_terminal::unsubscribe(terminal_session_id, subscription_id);
          }
        }
      } unsubscribe_guard {authenticated, terminal_session_id, subscription_id};

      boost::beast::flat_buffer read_buffer;
      while (true) {
        read_buffer.clear();
        ws->read(read_buffer);  // Throws on peer close/error, breaking the loop below via the outer catch.
        nlohmann::json payload;
        try {
          payload = nlohmann::json::parse(boost::beast::buffers_to_string(read_buffer.data()));
        } catch (const nlohmann::json::exception &) {
          continue;
        }
        if (!authenticated) {
          // Re-validate fresh in case the session expired between handshake and this message.
          const auto session {session_service.validate(session_id)};
          if (!session.has_value() || payload.value("type", "") != "auth" || payload.value("csrf_token", "") != session->csrf_token) {
            break;
          }
          terminal_session_id = payload.value("session_id", "");
          if (!steamshine_terminal::running(terminal_session_id)) {
            break;
          }
          subscription_id = steamshine_terminal::subscribe(terminal_session_id, [ws, write_mutex](std::string_view chunk) {
            std::lock_guard lock {*write_mutex};
            try {
              ws->write(boost::asio::buffer(chunk.data(), chunk.size()));
            } catch (...) {
              // The read loop will observe the same failure and clean up.
            }
          });
          if (subscription_id == 0) {
            break;
          }
          {
            std::lock_guard lock {*write_mutex};
            ws->text(true);
            constexpr std::string_view ready_message {R"({"type":"ready"})"};
            ws->write(boost::asio::buffer(ready_message));
            ws->binary(true);
          }
          authenticated = true;
          continue;
        }
        const auto type {payload.value("type", "")};
        if (type == "input") {
          steamshine_terminal::write_input(terminal_session_id, payload.value("data", ""));
        } else if (type == "scroll") {
          const auto lines {std::clamp(payload.value("lines", 0), -200, 200)};
          if (lines != 0) {
            steamshine_terminal::scroll(terminal_session_id, lines);
          }
        } else if (type == "resize") {
          steamshine_terminal::resize(
            terminal_session_id,
            static_cast<unsigned short>(std::clamp(payload.value("cols", 80), 1, 500)),
            static_cast<unsigned short>(std::clamp(payload.value("rows", 24), 1, 200))
          );
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(debug) << "SteamShine Terminal connection ended: "sv << e.what();
    }
  }

  /**
   * @brief Start the HTTPS configuration server.
   */
  void start() {
    platf::set_thread_name("confighttp");
    const auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    const auto port_https = net::map_port(PORT_HTTPS);
    const auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server {config::nvhttp.cert, config::nvhttp.pkey};

    // Helper to create page handler lambdas without repeating the signature
    auto page_handler = [](const char *file, bool require_auth = true, bool redirect_if_username = false) {
      return [file, require_auth, redirect_if_username](const resp_https_t &response, const req_https_t &request) {
        if (!config::sunshine.upstream_web_ui_enabled) {
          not_found(response, request);
          return;
        }
        getPage(response, request, file, require_auth, redirect_if_username);
      };
    };
    auto steamshine_handler = [](const https_handler_t &handler) {
      return [handler](const resp_https_t &response, const req_https_t &request) {
        if (!config::sunshine.steamshine_web_ui_enabled) {
          not_found(response, request);
          return;
        }
        handler(response, request);
      };
    };

    // Default resource handlers
    const https_handler_t bad_request_handler = [](const resp_https_t &response, const req_https_t &request) {
      bad_request(response, request);
    };
    const https_handler_t not_found_handler = [](const resp_https_t &response, const req_https_t &request) {
      not_found(response, request);
    };

    // error by default
    server.default_resource["DELETE"] = bad_request_handler;
    server.default_resource["PATCH"] = bad_request_handler;
    server.default_resource["POST"] = bad_request_handler;
    server.default_resource["PUT"] = bad_request_handler;
    server.default_resource["GET"] = not_found_handler;

    // web pages
    server.resource["^/$"]["GET"] = [](const resp_https_t &response, const req_https_t &request) {
      if (config::sunshine.steamshine_web_ui_default && config::sunshine.steamshine_web_ui_enabled) {
        getSteamshinePage(response, request);
        return;
      }
      if (!config::sunshine.upstream_web_ui_enabled) {
        not_found(response, request);
        return;
      }
      getPage(response, request, "index.html", true, false);
    };
    server.resource["^/sunshine/?$"]["GET"] = [](const resp_https_t &response, const req_https_t &request) {
      if (!config::sunshine.upstream_web_ui_enabled || !config::sunshine.upstream_web_ui_visible) {
        not_found(response, request);
        return;
      }
      getPage(response, request, "index.html", true, false);
    };
    server.resource["^/apps/?$"]["GET"] = page_handler("apps.html");
    server.resource["^/clients/?$"]["GET"] = page_handler("clients.html");
    server.resource["^/config/?$"]["GET"] = page_handler("config.html");
    server.resource["^/featured/?$"]["GET"] = page_handler("featured.html");
    server.resource["^/logout/?$"]["GET"] = page_handler("logout.html", false);
    server.resource["^/password/?$"]["GET"] = page_handler("password.html");
    server.resource["^/pin/?$"]["GET"] = page_handler("pin.html");
    server.resource["^/troubleshooting/?$"]["GET"] = page_handler("troubleshooting.html");
    server.resource["^/welcome/?$"]["GET"] = page_handler("welcome.html", false, true);
    server.resource["^/steamshine/?(?:setup|login|monitor|stream|applications|gpu|addons|settings|config|pairing|clients|diagnostics|terminal)?/?$"]["GET"] = getSteamshinePage;

    // rest api
    server.resource["^/api/browse$"]["GET"] = browseDirectory;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/covers/([0-9]+)$"]["GET"] = getCover;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/csrf-token$"]["GET"] = getCSRFToken;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/pin$"]["DELETE"] = cancelPairing;
    server.resource["^/api/pin$"]["GET"] = getPendingPairings;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/reset-portal-token$"]["POST"] = resetPortalToken;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/virtual-input/license$"]["GET"] = getVirtualInputLicense;
    server.resource["^/api/virtual-input/license$"]["POST"] = updateVirtualInputLicense;
    server.resource["^/api/virtual-input/status$"]["GET"] = getVirtualInputStatus;

    // SteamShine facade API. It uses a distinct cookie and server-side session store.
    server.resource["^/api/steamshine/v1/setup/status$"]["GET"] = steamshine_handler(steamshine_setup_status);
    server.resource["^/api/steamshine/v1/setup/credentials$"]["POST"] = steamshine_handler(steamshine_setup_credentials);
    server.resource["^/api/steamshine/v1/auth/login$"]["POST"] = steamshine_handler(steamshine_login);
    server.resource["^/api/steamshine/v1/auth/logout$"]["POST"] = steamshine_handler(steamshine_logout);
    server.resource["^/api/steamshine/v1/system/quit$"]["POST"] = steamshine_handler(steamshine_quit);
    server.resource["^/api/steamshine/v1/system/restart$"]["POST"] = steamshine_handler(steamshine_restart);
    server.resource["^/api/steamshine/v1/status$"]["GET"] = steamshine_handler(steamshine_status);
    server.resource["^/api/steamshine/v1/stream/profiles$"]["GET"] = steamshine_handler(steamshine_stream_profiles);
    server.resource["^/api/steamshine/v1/stream/profiles$"]["POST"] = steamshine_handler(steamshine_save_stream_profile);
    server.resource["^/api/steamshine/v1/stream/profiles/reset$"]["POST"] = steamshine_handler(steamshine_reset_stream_profile);
    server.resource["^/api/steamshine/v1/stream/recordings$"]["GET"] = steamshine_handler(steamshine_recordings);
    server.resource["^/api/steamshine/v1/stream/recordings/toggle$"]["POST"] = steamshine_handler(steamshine_toggle_recording);
    server.resource["^/api/steamshine/v1/stream/recordings/settings$"]["POST"] = steamshine_handler(steamshine_recording_settings);
    server.resource["^/api/steamshine/v1/stream/recordings/([A-Za-z0-9-]+)$"]["DELETE"] = steamshine_handler(steamshine_delete_recording);
    server.resource["^/api/steamshine/v1/stream/recordings/([A-Za-z0-9-]+)/(?:video|download)$"]["GET"] = steamshine_handler(steamshine_recording_media);
    server.resource["^/api/steamshine/v1/system/metrics$"]["GET"] = steamshine_handler(steamshine_system_metrics);
    server.resource["^/api/steamshine/v1/apps$"]["GET"] = steamshine_handler(steamshine_get_apps);
    server.resource["^/api/steamshine/v1/apps$"]["POST"] = steamshine_handler(steamshine_save_app);
    server.resource["^/api/steamshine/v1/apps/([0-9]+)$"]["DELETE"] = steamshine_handler(steamshine_delete_app);
    server.resource["^/api/steamshine/v1/apps/([0-9]+)/close$"]["POST"] = steamshine_handler(steamshine_close_app);
    server.resource["^/api/steamshine/v1/config$"]["GET"] = steamshine_handler(steamshine_get_config);
    server.resource["^/api/steamshine/v1/config$"]["POST"] = steamshine_handler(steamshine_save_config);
    server.resource["^/api/steamshine/v1/gpu/capabilities$"]["GET"] = steamshine_handler(steamshine_gpu_capabilities);
    server.resource["^/api/steamshine/v1/gpu/profiles$"]["GET"] = steamshine_handler(steamshine_gpu_profiles);
    server.resource["^/api/steamshine/v1/gpu/profiles$"]["POST"] = steamshine_handler(steamshine_save_gpu_profile);
    server.resource["^/api/steamshine/v1/gpu/profiles/([^/]+)$"]["DELETE"] = steamshine_handler(steamshine_delete_gpu_profile);
    server.resource["^/api/steamshine/v1/gpu/profiles/([^/]+)/activate$"]["POST"] = steamshine_handler(steamshine_activate_gpu_profile);
    server.resource["^/api/steamshine/v1/addons/decky$"]["GET"] = steamshine_handler(steamshine_decky_status);
    server.resource["^/api/steamshine/v1/addons/decky/action$"]["POST"] = steamshine_handler(steamshine_decky_action);
    server.resource["^/api/steamshine/v1/system/authorize$"]["POST"] = steamshine_handler(steamshine_authorize_management);
    server.resource["^/api/steamshine/v1/system/management$"]["GET"] = steamshine_handler(steamshine_management_status);
    server.resource["^/api/steamshine/v1/terminal/status$"]["GET"] = steamshine_handler(steamshine_terminal_status);
    server.resource["^/api/steamshine/v1/terminal/start$"]["POST"] = steamshine_handler(steamshine_terminal_start);
    server.resource["^/api/steamshine/v1/terminal/stop$"]["POST"] = steamshine_handler(steamshine_terminal_stop);
    server.resource["^/api/steamshine/v1/config/virtual-display$"]["GET"] = steamshine_handler(steamshine_virtual_display_config);
    server.resource["^/api/steamshine/v1/config/virtual-display$"]["POST"] = steamshine_handler(steamshine_save_virtual_display_config);
    server.resource["^/api/steamshine/v1/config/virtual-display/sources$"]["GET"] = steamshine_handler(steamshine_gamescope_sources);
    server.resource["^/api/steamshine/v1/session$"]["GET"] = steamshine_handler(steamshine_session);
    server.resource["^/api/steamshine/v1/pairing/pin$"]["GET"] = steamshine_handler(steamshine_pending_pairings);
    server.resource["^/api/steamshine/v1/pairing/pin$"]["POST"] = steamshine_handler(steamshine_pairing_pin);
    server.resource["^/api/steamshine/v1/clients$"]["GET"] = steamshine_handler(steamshine_clients);
    server.resource["^/api/steamshine/v1/clients/.+$"]["DELETE"] = steamshine_handler(steamshine_revoke_client);
    server.resource["^/api/steamshine/v1/logs/recent$"]["GET"] = steamshine_handler(steamshine_recent_logs);
    server.resource["^/api/steamshine/v1/diagnostics$"]["GET"] = steamshine_handler(steamshine_diagnostics);
    server.resource["^/api/steamshine/v1/diagnostics/reset$"]["POST"] = steamshine_handler(steamshine_reset_diagnostics);

    // static/dynamic resources
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-45.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/steamshine/.+\\.(css|js|png|jpg|jpeg|svg|ico)$"]["GET"] = getSteamshineAsset;
    server.resource["^/assets\\/.+$"]["GET"] = getAsset;

    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;

    const auto display_addr = net::get_bind_address_url_host();

    auto accept_and_run = [&](auto *server) {
      try {
        platf::set_thread_name("confighttp::tcp");
        server->start([&display_addr](const unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://"sv << display_addr << ":" << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread tcp {accept_and_run, &server};

    // SteamShine Terminal: a dedicated Secure WebSocket acceptor (the shared
    // Simple-Web-Server fork used above has no WebSocket support) carrying a
    // multiple PTY-backed shell sessions (see steamshine_terminal.cpp). The
    // handshake's `steamshine_session` cookie is validated before the
    // WebSocket handshake completes, and no input/output is allowed until the
    // client proves CSRF-token possession with its first message -- the same
    // two checks require_steamshine_mutation() performs for every other
    // mutating SteamShine endpoint. One thread per connection is acceptable
    // here: this is a single-user admin feature, not a public-facing server.
    const auto port_terminal_ws = net::map_port(PORT_STEAMSHINE_TERMINAL);
    boost::asio::io_context terminal_ioc {1};
    boost::asio::ssl::context terminal_ssl_ctx {boost::asio::ssl::context::tlsv12};
    boost::asio::ip::tcp::acceptor terminal_acceptor {terminal_ioc};
    terminal_connection_registry_t terminal_connections;

    auto accept_and_run_ws = [&] {
      platf::set_thread_name("confighttp::terminal_ws");
      std::vector<std::jthread> connection_threads;
      try {
        terminal_ssl_ctx.use_certificate_chain_file(config::nvhttp.cert);
        terminal_ssl_ctx.use_private_key_file(config::nvhttp.pkey, boost::asio::ssl::context::pem);

        const boost::asio::ip::tcp::endpoint endpoint {boost::asio::ip::make_address(net::get_bind_address(address_family)), port_terminal_ws};
        terminal_acceptor.open(endpoint.protocol());
        terminal_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        terminal_acceptor.bind(endpoint);
        terminal_acceptor.listen();
        terminal_acceptor.non_blocking(true);
        BOOST_LOG(info) << "SteamShine Terminal WebSocket available on [" << endpoint.address().to_string() << ':' << endpoint.port() << "]";

        while (!shutdown_event->peek()) {
          boost::asio::ip::tcp::socket socket {terminal_ioc};
          boost::system::error_code accept_error;
          terminal_acceptor.accept(socket, accept_error);
          if (accept_error) {
            if (terminal_accept_is_retryable(accept_error)) {
              shutdown_event->view(50ms);
              continue;
            }
            break;
          }
          connection_threads.emplace_back(handle_terminal_ws_connection, std::move(socket), std::ref(terminal_ssl_ctx), std::ref(terminal_connections));
        }
      } catch (const std::exception &err) {
        if (!shutdown_event->peek()) {
          BOOST_LOG(warning) << "Couldn't start SteamShine Terminal WebSocket server on port ["sv << port_terminal_ws << "]: "sv << err.what();
        }
      }
      terminal_connections.close_all();
      for (auto &thread : connection_threads) {
        thread.join();
      }
    };
    std::jthread terminal_ws_thread {accept_and_run_ws};

    // Wait for any event
    shutdown_event->view();

    server.stop();
    boost::system::error_code close_error;
    terminal_acceptor.close(close_error);
    terminal_connections.close_all();

    tcp.join();
    terminal_ws_thread.join();
    steamshine_terminal::detach_all();
  }
}  // namespace confighttp
