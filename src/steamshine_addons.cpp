/**
 * @file src/steamshine_addons.cpp
 * @brief Fixed-scope Decky Loader management for the SteamShine Addon tab.
 */
#include "steamshine_addons.h"

#include "logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <utility>

#if defined(__linux__)
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace steamshine_addons {

  namespace {

    constexpr std::string_view DECKY_BINARY {"/home/deck/homebrew/services/PluginLoader"};
    constexpr std::string_view DECKY_VERSION {"/home/deck/homebrew/services/.loader.version"};
    constexpr std::string_view DECKY_HELPER {"/var/lib/steamshine/helpers/steamshine-decky-helper"};
    constexpr std::size_t MAX_OPERATION_OUTPUT {32 * 1024};

    /**
     * @brief Trim trailing line endings and whitespace from command output.
     *
     * @param value Text to trim in place.
     */
    void trim_trailing_whitespace(std::string &value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
      }
    }

    /**
     * @brief Execute a fixed argv vector, capturing a bounded combined output.
     *
     * @param arguments Executable and arguments; no shell is involved.
     * @param input Optional transient input, written through an anonymous socket and never logged.
     * @return Normalized exit code and bounded output.
     */
    std::pair<int, std::string> run_command(const std::vector<std::string> &arguments, const std::string_view input = {}) {
#if defined(__linux__)
      if (arguments.empty()) {
        return {-1, {}};
      }
      int output_pipe[2] {-1, -1};
      if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
        return {-1, {}};
      }
      int input_pair[2] {-1, -1};
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input_pair) != 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        return {-1, {}};
      }
      const pid_t child {::fork()};
      if (child < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        ::close(input_pair[0]);
        ::close(input_pair[1]);
        return {-1, {}};
      }
      if (child == 0) {
        ::close(input_pair[0]);
        ::dup2(input_pair[1], STDIN_FILENO);
        ::close(input_pair[1]);
        ::close(output_pipe[0]);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[1]);
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) {
          argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        _exit(127);
      }
      ::close(input_pair[1]);
      std::size_t sent = 0;
      while (sent < input.size()) {
        const auto count {::send(input_pair[0], input.data() + sent, input.size() - sent, MSG_NOSIGNAL)};
        if (count > 0) {
          sent += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
          continue;
        } else {
          break;
        }
      }
      ::shutdown(input_pair[0], SHUT_WR);
      ::close(input_pair[0]);
      ::close(output_pipe[1]);
      std::string output;
      std::array<char, 4096> buffer {};
      while (true) {
        const auto count {::read(output_pipe[0], buffer.data(), buffer.size())};
        if (count > 0) {
          if (output.size() < MAX_OPERATION_OUTPUT) {
            output.append(buffer.data(), std::min<std::size_t>(static_cast<std::size_t>(count), MAX_OPERATION_OUTPUT - output.size()));
          }
        } else if (count < 0 && errno == EINTR) {
          continue;
        } else {
          break;
        }
      }
      ::close(output_pipe[0]);
      int status {};
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      trim_trailing_whitespace(output);
      return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(output)};
#else
      (void) arguments;
      (void) input;
      return {-1, {}};
#endif
    }

    /**
     * @brief Query one systemd predicate without changing service state.
     *
     * @param predicate Fixed `is-active` or `is-enabled` operation.
     * @return True when systemctl reports success.
     */
    bool systemd_predicate(const std::string &predicate) {
      return run_command({"/usr/bin/systemctl", predicate, "--quiet", "plugin_loader.service"}).first == 0;
    }

    /**
     * @brief Locate a script in this installation without accepting client-controlled paths.
     * @param filename Fixed packaged filename.
     * @return Script path or an empty path.
     */
    std::filesystem::path packaged_script(const std::string_view filename) {
#if defined(__linux__)
      std::error_code error;
      const auto executable {std::filesystem::canonical("/proc/self/exe", error)};
      const auto script {executable.parent_path().parent_path() / "scripts" / filename};
      if (!error && std::filesystem::is_regular_file(script, error)) {
        return script;
      }
#endif
      return {};
    }

    /**
     * @brief Decode one helper response while preserving failure status.
     * @param command_result Process result.
     * @return JSON with a safe error when parsing fails.
     */
    nlohmann::json helper_json(const std::pair<int, std::string> &command_result) {
      auto result = nlohmann::json::parse(command_result.second, nullptr, false);
      if (!result.is_object()) {
        return {{"success", false}, {"message", "Management helper did not return a valid response"}};
      }
      if (command_result.first != 0) {
        result["success"] = false;
      }
      return result;
    }

    /**
     * @brief Execute the cache helper from the same immutable installation as this binary.
     * @param library_id Empty for read-only status; otherwise a registered library identifier.
     * @return Parsed JSON or a bounded error without exposing a command execution interface.
     */
    nlohmann::json run_steam_cache_helper(const std::string_view library_id) {
      const auto helper {packaged_script("steamshine-steam-cache.py")};
      if (helper.empty()) {
        return {{"success", false}, {"message", "Steam cache management is unavailable; install a current SteamOS package."}};
      }
      const auto arguments {steam_cache_helper_arguments(helper.string(), library_id)};
      if (!arguments) {
        return {{"success", false}, {"message", "Invalid Steam library identifier"}};
      }
      return helper_json(run_command(*arguments));
    }

  }  // namespace

  std::optional<decky_action_e> parse_decky_action(const std::string_view value) {
    if (value == "install") {
      return decky_action_e::install;
    }
    if (value == "update") {
      return decky_action_e::update;
    }
    if (value == "uninstall") {
      return decky_action_e::uninstall;
    }
    return std::nullopt;
  }

  std::vector<std::string> decky_helper_arguments(const decky_action_e action, const std::string_view helper_path) {
    std::string action_name;
    switch (action) {
      case decky_action_e::install:
        action_name = "install";
        break;
      case decky_action_e::update:
        action_name = "update";
        break;
      case decky_action_e::uninstall:
        action_name = "uninstall";
        break;
      case decky_action_e::start:
        action_name = "start";
        break;
    }
    return {"/usr/bin/sudo", "-n", std::string {helper_path}, std::move(action_name)};
  }

  decky_status_t decky_status() {
    decky_status_t status;
#if defined(__linux__)
    status.installed = ::access(DECKY_BINARY.data(), X_OK) == 0;
    status.management_available = ::access(DECKY_HELPER.data(), X_OK) == 0 &&
                                  run_command({"/usr/bin/sudo", "-n", std::string {DECKY_HELPER}, "authorize"}).first == 0;
    std::ifstream version_file {DECKY_VERSION.data()};
    std::getline(version_file, status.version);
    trim_trailing_whitespace(status.version);
    status.service_enabled = systemd_predicate("is-enabled");
    status.service_active = systemd_predicate("is-active");
#endif
    return status;
  }

  decky_action_result_t perform_decky_action(const decky_action_e action) {
    const auto arguments {decky_helper_arguments(action, DECKY_HELPER)};
    BOOST_LOG(info) << "ADDON_EVENT addon=decky action=" << arguments.back() << " phase=requested";
    const auto [exit_code, output] {run_command(arguments)};
    decky_action_result_t result {
      .success = exit_code == 0,
      .exit_code = exit_code,
      .message = output.empty() ? (exit_code == 0 ? "Decky Loader operation completed" : "Decky Loader operation failed; privileged helper may be unavailable") : output,
      .status = decky_status(),
    };
    if (result.success) {
      BOOST_LOG(info) << "ADDON_EVENT addon=decky action=" << arguments.back()
                      << " phase=completed result=success exit_code=" << result.exit_code;
    } else {
      BOOST_LOG(error) << "ADDON_EVENT addon=decky action=" << arguments.back()
                       << " phase=completed result=failed exit_code=" << result.exit_code;
    }
    return result;
  }

  bool decky_start_required(const decky_status_t &status) {
    return status.installed && !status.service_active;
  }

  bool ensure_decky_active_for_owned_session() {
    const auto status {decky_status()};
    if (!decky_start_required(status)) {
      return true;
    }
    BOOST_LOG(info) << "ADDON_EVENT addon=decky action=start phase=owned_session_prepare reason=inactive";
    const auto result {perform_decky_action(decky_action_e::start)};
    return result.success && result.status.service_active;
  }

  std::optional<std::vector<std::string>> steam_cache_helper_arguments(const std::string_view helper_path, const std::string_view library_id) {
    if (library_id.empty()) {
      return std::vector<std::string> {"/usr/bin/python3", std::string {helper_path}, "status"};
    }
    if (library_id.size() != 24 || !std::all_of(library_id.begin(), library_id.end(), [](const char character) {
          return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        })) {
      return std::nullopt;
    }
    return std::vector<std::string> {"/usr/bin/python3", std::string {helper_path}, "configure", std::string {library_id}};
  }

  nlohmann::json steam_cache_status() {
    auto result = run_steam_cache_helper({});
    if (!result.value("success", true)) {
      return {{"available", false}, {"libraries", nlohmann::json::array()}, {"message", result.value("message", "Steam cache inspection failed")}};
    }
    return result;
  }

  nlohmann::json configure_steam_cache(const std::string_view library_id) {
    if (library_id.empty()) {
      return {{"success", false}, {"message", "A Steam library identifier is required"}};
    }
    return run_steam_cache_helper(library_id);
  }

  bool management_ready(const std::string_view name) {
    if (name != "runtime" && name != "decky" && name != "storage") {
      return false;
    }
    const auto stem {"steamshine-"s + std::string {name} + "-helper"};
    const auto source {packaged_script(stem + (name == "storage" ? ".py" : ".sh"))};
    const auto destination {"/var/lib/steamshine/helpers/"s + stem};
    std::ifstream packaged {source, std::ios::binary};
    std::ifstream installed {destination, std::ios::binary};
    if (!packaged || !installed || !std::equal(std::istreambuf_iterator<char> {packaged}, {}, std::istreambuf_iterator<char> {installed}, {})) {
      return false;
    }
    return run_command({"/usr/bin/sudo", "-n", destination, "authorize"}).first == 0;
  }

  nlohmann::json management_status() {
    return {{"runtime", management_ready("runtime")}, {"decky", management_ready("decky")}, {"storage", management_ready("storage")}, {"authorization_available", !packaged_script("steamshine-provision-management.py").empty()}};
  }

  bool management_password_valid(const std::string_view password) {
    return !password.empty() && password.size() <= 1024 && password.find_first_of("\r\n") == std::string_view::npos && password.find('\0') == std::string_view::npos;
  }

  nlohmann::json authorize_management(const std::string_view password) {
    const auto script {packaged_script("steamshine-provision-management.py")};
    if (!management_password_valid(password) || script.empty()) {
      return {{"success", false}, {"message", "Enter the local administrator password; a current SteamOS package is required."}};
    }
    std::string input {password};
    input.push_back('\n');
    const auto [code, output] {run_command({"/usr/bin/timeout", "90", "/usr/bin/sudo", "-S", "-k", "-p", "", "--", "/usr/bin/python3", script.string()}, input)};
    std::fill(input.begin(), input.end(), '\0');
    if (code != 0) {
      return {{"success", false}, {"message", "Administrator authentication or management setup failed. Check the password and try again."}};
    }
    const auto status = management_status();
    const bool ready {status.value("runtime", false) && status.value("decky", false) && status.value("storage", false)};
    return {{"success", ready}, {"message", ready ? "Web management is ready." : "Management authorization could not be verified."}, {"helpers", status}};
  }

  std::optional<std::vector<std::string>> storage_helper_arguments(const std::string_view action, const std::string_view uuid) {
    if (action != "status" && action != "remember" && action != "restore") {
      return std::nullopt;
    }
    if (action == "restore") {
      static const std::regex uuid_pattern {"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"};
      if (!std::regex_match(uuid.begin(), uuid.end(), uuid_pattern)) {
        return std::nullopt;
      }
    } else if (!uuid.empty()) {
      return std::nullopt;
    }
    std::vector<std::string> result {"/usr/bin/sudo", "-n", "/var/lib/steamshine/helpers/steamshine-storage-helper", std::string {action}};
    if (!uuid.empty()) {
      result.emplace_back(uuid);
    }
    return result;
  }

  nlohmann::json storage_status() {
    const bool ready {management_ready("storage")};
    const auto script {packaged_script("steamshine-storage-helper.py")};
    auto result = ready ? storage_action("status") : (script.empty() ? nlohmann::json {{"volumes", nlohmann::json::array()}} : helper_json(run_command({"/usr/bin/python3", script.string(), "status"})));
    result["management_available"] = ready;
    return result;
  }

  nlohmann::json storage_action(const std::string_view action, const std::string_view uuid) {
    const auto arguments {storage_helper_arguments(action, uuid)};
    if (!arguments) {
      return {{"success", false}, {"message", "Invalid storage operation or UUID"}};
    }
    return helper_json(run_command(*arguments));
  }

  void to_json(nlohmann::json &json, const decky_status_t &value) {
    json = nlohmann::json {
      {"installed", value.installed},
      {"version", value.version},
      {"service_enabled", value.service_enabled},
      {"service_active", value.service_active},
      {"management_available", value.management_available},
    };
  }

  void to_json(nlohmann::json &json, const decky_action_result_t &value) {
    json = nlohmann::json {
      {"success", value.success},
      {"exit_code", value.exit_code},
      {"message", value.message},
      {"status", value.status},
    };
  }

}  // namespace steamshine_addons
