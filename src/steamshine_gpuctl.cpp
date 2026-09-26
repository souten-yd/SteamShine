/**
 * @file src/steamshine_gpuctl.cpp
 * @brief AMD GPU/CPU performance-profile detection and control for the SteamShine web UI.
 */
#include "steamshine_gpuctl.h"

#include "config.h"
#include "file_handler.h"
#include "logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#if defined(__linux__)
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char **environ;
#endif

using namespace std::literals;

namespace steamshine_gpuctl {

  namespace fs = std::filesystem;

  namespace {

    constexpr std::array<const char *, 4> BUILTIN_NAMES {"Silent", "Balanced", "Performance", "OC"};
    constexpr std::string_view RUNTIME_HELPER {"/var/lib/steamshine/helpers/steamshine-runtime-helper"};
    constexpr std::size_t MAX_HELPER_OUTPUT {8 * 1024};

    /**
     * @brief Read one trimmed sysfs attribute without invoking external tools.
     */
    std::string read_attribute(const fs::path &path) {
      std::ifstream input {path};
      std::string value;
      std::getline(input, value);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
      }
      return value;
    }

    std::optional<double> read_double(const fs::path &path) {
      const auto text {read_attribute(path)};
      if (text.empty()) {
        return std::nullopt;
      }
      try {
        return std::stod(text);
      } catch (...) {
        return std::nullopt;
      }
    }

    /**
     * @brief Absolute, allow-listed sysfs write targets resolved once at detection time.
     *
     * Nothing outside this struct's paths is ever written; every path here was
     * discovered by enumerating real sysfs directories, never built from a
     * caller-supplied string.
     */
    struct hardware_paths_t {
      fs::path gpu_device_dir;  ///< `/sys/class/drm/card*/device`.
      fs::path gpu_hwmon_dir;  ///< `gpu_device_dir/hwmon/hwmon*`.
      fs::path perf_level_path;  ///< `gpu_device_dir/power_dpm_force_performance_level`.
      fs::path od_clk_voltage_path;  ///< `gpu_device_dir/pp_od_clk_voltage`.
      std::vector<fs::path> cpu_governor_paths;  ///< One `cpuN/cpufreq/scaling_governor` per online core.
      std::vector<fs::path> cpu_max_freq_paths;  ///< One `cpuN/cpufreq/scaling_max_freq` per online core.
    };

    /**
     * @brief Locate the first AMD (`amdgpu`-driven) DRM card, if any.
     */
    std::optional<fs::path> locate_amd_gpu_device_dir() {
      std::error_code error;
      for (const auto &entry : fs::directory_iterator {"/sys/class/drm", error}) {
        const auto filename {entry.path().filename().string()};
        if (filename.rfind("card", 0) != 0 || filename.find('-') != std::string::npos) {
          continue;
        }
        const auto device_dir {entry.path() / "device"};
        if (read_attribute(device_dir / "vendor") != "0x1002") {
          continue;
        }
        std::error_code driver_error;
        const auto driver_link {fs::read_symlink(device_dir / "driver", driver_error)};
        if (driver_error || driver_link.filename() != "amdgpu") {
          continue;
        }
        return device_dir;
      }
      return std::nullopt;
    }

    std::once_flag g_detect_once;
    capabilities_t g_capabilities;
    std::mutex g_capabilities_mutex;  ///< Protect refreshed capability snapshots after Web authorization.
    hardware_paths_t g_paths;

    /**
     * @brief Execute the fixed runtime helper without involving a shell.
     *
     * @param helper_arguments Arguments following the helper executable.
     * @return Normalized exit code and bounded combined standard output/error.
     */
    std::pair<int, std::string> run_runtime_helper(const std::vector<std::string> &helper_arguments) {
      static std::mutex helper_mutex;  ///< Serialize probes and writes while the helper holds the GPU active.
      std::lock_guard lock {helper_mutex};
#if defined(__linux__)
      std::vector<std::string> arguments {"/usr/bin/sudo", "-n", std::string {RUNTIME_HELPER}};
      arguments.insert(arguments.end(), helper_arguments.begin(), helper_arguments.end());
      int output_pipe[2] {-1, -1};
      if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
        return {-1, "could not create helper output pipe"};
      }
      std::vector<char *> argv;
      argv.reserve(arguments.size() + 1);
      for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
      }
      argv.push_back(nullptr);
      posix_spawn_file_actions_t file_actions;
      if (::posix_spawn_file_actions_init(&file_actions) != 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        return {-1, "could not initialize runtime helper file actions"};
      }
      ::posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);
      ::posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
      ::posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
      ::posix_spawn_file_actions_addclose(&file_actions, output_pipe[1]);
      pid_t child {};
      const int spawn_error {::posix_spawn(&child, arguments.front().c_str(), &file_actions, nullptr, argv.data(), environ)};
      ::posix_spawn_file_actions_destroy(&file_actions);
      if (spawn_error != 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        return {-1, "could not spawn runtime helper"};
      }
      ::close(output_pipe[1]);
      std::string output;
      std::array<char, 1024> buffer {};
      while (true) {
        const auto count {::read(output_pipe[0], buffer.data(), buffer.size())};
        if (count > 0) {
          if (output.size() < MAX_HELPER_OUTPUT) {
            output.append(buffer.data(), std::min<std::size_t>(static_cast<std::size_t>(count), MAX_HELPER_OUTPUT - output.size()));
          }
        } else if (count < 0 && errno == EINTR) {
          continue;
        } else {
          break;
        }
      }
      ::close(output_pipe[0]);
      int status {};
      pid_t waited {};
      do {
        waited = ::waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
        output.pop_back();
      }
      return {waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(output)};
#else
      (void) helper_arguments;
      return {-1, "runtime helper is only available on Linux"};
#endif
    }

    /**
     * @brief Check whether the helper accepts non-interactive authorization.
     *
     * @return True when the exact helper command is authorized by sudoers.
     */
    bool runtime_write_authorized() {
      return run_runtime_helper({"authorize"}).first == 0;
    }

    /**
     * @brief Ask the privileged helper to inspect a possibly suspended AMD GPU.
     *
     * @return Validated GPU capabilities, or no value when the helper is unavailable.
     */
    std::optional<gpu_capability_probe_t> probe_gpu_capabilities() {
      const auto [exit_code, output] {run_runtime_helper({"probe-gpu"})};
      if (exit_code != 0) {
        BOOST_LOG(warning) << "steamshine_gpuctl: privileged GPU capability probe failed"
                           << " exit_code=" << exit_code
                           << " output=" << nlohmann::json(output).dump();
        return std::nullopt;
      }
      const auto parsed {parse_gpu_capability_probe(output)};
      if (!parsed) {
        BOOST_LOG(warning) << "steamshine_gpuctl: privileged GPU capability probe returned invalid data";
      }
      return parsed;
    }

    /**
     * @brief Enumerate present CPU cores' cpufreq directories.
     */
    std::vector<fs::path> locate_cpu_cpufreq_dirs() {
      std::vector<fs::path> result;
      std::error_code error;
      for (int core = 0;; ++core) {
        const auto dir {fs::path {"/sys/devices/system/cpu"} / ("cpu" + std::to_string(core)) / "cpufreq"};
        if (!fs::exists(dir, error)) {
          break;
        }
        result.push_back(dir);
      }
      return result;
    }

    /**
     * @brief Perform hardware detection exactly once per process.
     */
    void detect() {
      std::call_once(g_detect_once, [] {
        g_capabilities.runtime_write_authorized = runtime_write_authorized();
        const auto privileged_gpu {g_capabilities.runtime_write_authorized ? probe_gpu_capabilities() : std::nullopt};
        const auto device_dir {locate_amd_gpu_device_dir()};
        if (device_dir) {
          g_paths.gpu_device_dir = *device_dir;
          std::error_code hwmon_error;
          for (const auto &hwmon_entry : fs::directory_iterator {*device_dir / "hwmon", hwmon_error}) {
            g_paths.gpu_hwmon_dir = hwmon_entry.path();
            break;
          }
          g_capabilities.gpu_present = true;
          const auto device_id {read_attribute(*device_dir / "device")};
          g_capabilities.gpu_name = device_id.empty() ? "AMD GPU" : ("AMD GPU (1002:" + (device_id.rfind("0x", 0) == 0 ? device_id.substr(2) : device_id) + ")");

          if (privileged_gpu && privileged_gpu->gpu_present) {
            g_capabilities.power_cap_supported = privileged_gpu->power_cap_supported;
            g_capabilities.power_cap_min_watts = privileged_gpu->power_cap_min_watts;
            g_capabilities.power_cap_max_watts = privileged_gpu->power_cap_max_watts;
            g_capabilities.power_cap_default_watts = privileged_gpu->power_cap_default_watts;
            g_capabilities.perf_level_supported = privileged_gpu->perf_level_supported;
            g_capabilities.od_clk_voltage_supported = privileged_gpu->od_clk_voltage_supported;
          } else if (!g_paths.gpu_hwmon_dir.empty()) {
            const auto power_cap_path {g_paths.gpu_hwmon_dir / "power1_cap"};
            const auto cap_min {read_double(g_paths.gpu_hwmon_dir / "power1_cap_min")};
            const auto cap_max {read_double(g_paths.gpu_hwmon_dir / "power1_cap_max")};
            const auto cap_default {read_double(g_paths.gpu_hwmon_dir / "power1_cap_default")};
            if (cap_min && cap_max && *cap_min > 0 && *cap_max >= *cap_min && fs::exists(power_cap_path)) {
              g_capabilities.power_cap_supported = true;
              g_capabilities.power_cap_min_watts = *cap_min / 1'000'000.0;
              g_capabilities.power_cap_max_watts = *cap_max / 1'000'000.0;
              g_capabilities.power_cap_default_watts = cap_default.value_or(*cap_max) / 1'000'000.0;
            }
          }

          g_paths.perf_level_path = *device_dir / "power_dpm_force_performance_level";
          g_paths.od_clk_voltage_path = *device_dir / "pp_od_clk_voltage";
          if (!privileged_gpu || !privileged_gpu->gpu_present) {
            g_capabilities.perf_level_supported = fs::exists(g_paths.perf_level_path);
            g_capabilities.od_clk_voltage_supported = fs::exists(g_paths.od_clk_voltage_path);
          }
        }

        g_paths.cpu_governor_paths.clear();
        g_paths.cpu_max_freq_paths.clear();
        for (const auto &cpufreq_dir : locate_cpu_cpufreq_dirs()) {
          g_paths.cpu_governor_paths.push_back(cpufreq_dir / "scaling_governor");
          g_paths.cpu_max_freq_paths.push_back(cpufreq_dir / "scaling_max_freq");
        }
        if (!g_paths.cpu_governor_paths.empty()) {
          const auto &first_dir = g_paths.cpu_governor_paths.front().parent_path();
          const auto min_khz {read_double(first_dir / "cpuinfo_min_freq")};
          const auto max_khz {read_double(first_dir / "cpuinfo_max_freq")};
          const auto governors {read_attribute(first_dir / "scaling_available_governors")};
          if (min_khz && max_khz) {
            g_capabilities.cpu_freq_supported = true;
            g_capabilities.cpu_min_freq_mhz = *min_khz / 1000.0;
            g_capabilities.cpu_max_freq_mhz = *max_khz / 1000.0;
          }
          std::istringstream governor_stream {governors};
          std::string governor;
          while (governor_stream >> governor) {
            g_capabilities.cpu_governors.push_back(governor);
          }
        }
      });
    }

    double clamp(double value, double lo, double hi) {
      return std::clamp(value, std::min(lo, hi), std::max(lo, hi));
    }

    /**
     * @brief Build one built-in profile scaled from the detected capability bounds.
     */
    profile_t make_builtin(std::string_view name, double power_fraction, std::string_view governor, double cpu_fraction, int clock_offset, int voltage_offset, std::string_view description) {
      const auto &caps {capabilities()};
      profile_t profile;
      profile.name = std::string {name};
      profile.description = std::string {description};
      profile.builtin = true;
      profile.power_cap_watts = caps.power_cap_supported ? caps.power_cap_min_watts + power_fraction * (caps.power_cap_max_watts - caps.power_cap_min_watts) : 0.0;
      profile.cpu_governor = std::string {governor};
      profile.cpu_max_freq_mhz = caps.cpu_freq_supported ? caps.cpu_min_freq_mhz + cpu_fraction * (caps.cpu_max_freq_mhz - caps.cpu_min_freq_mhz) : 0.0;
      profile.gpu_clock_offset_mhz = caps.od_clk_voltage_supported ? clock_offset : 0;
      profile.gpu_voltage_offset_mv = caps.od_clk_voltage_supported ? voltage_offset : 0;
      return profile;
    }

    std::mutex g_profiles_mutex;

  }  // namespace

  capabilities_t capabilities() {
    detect();
    std::scoped_lock lock {g_capabilities_mutex};
    return g_capabilities;
  }

  capabilities_t merge_authorization_probe(capabilities_t previous, const bool authorized, const std::optional<gpu_capability_probe_t> &probe) {
    previous.runtime_write_authorized = authorized;
    if (authorized && probe && probe->gpu_present) {
      previous.gpu_present = true;
      previous.power_cap_supported = probe->power_cap_supported;
      previous.power_cap_min_watts = probe->power_cap_min_watts;
      previous.power_cap_max_watts = probe->power_cap_max_watts;
      previous.power_cap_default_watts = probe->power_cap_default_watts;
      previous.perf_level_supported = probe->perf_level_supported;
      previous.od_clk_voltage_supported = probe->od_clk_voltage_supported;
    }
    return previous;
  }

  void refresh_capabilities() {
    detect();
    const bool authorized {runtime_write_authorized()};
    const auto probe {authorized ? probe_gpu_capabilities() : std::nullopt};
    std::scoped_lock lock {g_capabilities_mutex};
    g_capabilities = merge_authorization_probe(g_capabilities, authorized, probe);
  }

  std::optional<gpu_capability_probe_t> parse_gpu_capability_probe(const std::string_view output) {
    try {
      const nlohmann::json json = nlohmann::json::parse(std::string {output});
      gpu_capability_probe_t result;
      result.gpu_present = json.at("gpu_present").get<bool>();
      result.power_cap_supported = json.at("power_cap_supported").get<bool>();
      result.perf_level_supported = json.at("perf_level_supported").get<bool>();
      result.od_clk_voltage_supported = json.at("od_clk_voltage_supported").get<bool>();
      for (const auto key : {"power_cap_min_microwatts", "power_cap_max_microwatts", "power_cap_default_microwatts"}) {
        if (!json.at(key).is_number_unsigned()) {
          return std::nullopt;
        }
      }
      const auto minimum {json.at("power_cap_min_microwatts").get<std::uint64_t>()};
      const auto maximum {json.at("power_cap_max_microwatts").get<std::uint64_t>()};
      const auto default_value {json.at("power_cap_default_microwatts").get<std::uint64_t>()};
      if (!result.gpu_present && (result.power_cap_supported || result.perf_level_supported || result.od_clk_voltage_supported)) {
        return std::nullopt;
      }
      if (result.power_cap_supported) {
        if (minimum == 0 || maximum < minimum || default_value < minimum || default_value > maximum) {
          return std::nullopt;
        }
        constexpr double MICROWATTS_PER_WATT {1'000'000.0};
        result.power_cap_min_watts = static_cast<double>(minimum) / MICROWATTS_PER_WATT;
        result.power_cap_max_watts = static_cast<double>(maximum) / MICROWATTS_PER_WATT;
        result.power_cap_default_watts = static_cast<double>(default_value) / MICROWATTS_PER_WATT;
      } else if (minimum != 0 || maximum != 0 || default_value != 0) {
        return std::nullopt;
      }
      return result;
    } catch (const std::exception &error) {
      BOOST_LOG(warning) << "steamshine_gpuctl: could not parse privileged GPU capability response: " << error.what();
      return std::nullopt;
    }
  }

  std::vector<profile_t> builtin_profiles() {
    detect();
    const auto governors {capabilities().cpu_governors};
    const auto has = [&](std::string_view value) {
      return std::find(governors.begin(), governors.end(), value) != governors.end();
    };
    const std::string powersave {has("powersave") ? "powersave" : (governors.empty() ? "" : governors.front())};
    const std::string performance_governor {has("performance") ? "performance" : (governors.empty() ? "" : governors.back())};
    return {
      make_builtin("Silent", 0.15, powersave, 0.2, 0, 0, "Lowest power and fan noise; reduced performance."),
      make_builtin("Balanced", 0.55, powersave, 0.6, 0, 0, "Factory-like balance of performance and thermals."),
      make_builtin("Performance", 0.85, performance_governor, 0.9, 0, 0, "Higher sustained clocks and power limit."),
      make_builtin("OC", 1.0, performance_governor, 1.0, 50, -25, "Maximum power limit with a conservative clock/voltage offset. Verify stability yourself."),
    };
  }

  namespace {
    /**
     * @brief Parse the stored custom-profile list. Caller must already hold `g_profiles_mutex`.
     * @param valid Optional output indicating whether every stored entry was decoded.
     * @return Profiles decoded from the persisted top-level JSON array.
     */
    std::vector<profile_t> custom_profiles_locked(bool *valid = nullptr) {
      if (valid) {
        *valid = true;
      }
      std::vector<profile_t> result;
      if (config::sunshine.steamshine_gpu_profiles.empty()) {
        return result;
      }
      try {
        // List initialization would wrap the parsed array in another JSON array.
        const nlohmann::json parsed = nlohmann::json::parse(config::sunshine.steamshine_gpu_profiles);
        if (!parsed.is_array()) {
          throw std::invalid_argument("Stored GPU profiles must be an array");
        }
        for (const auto &entry : parsed) {
          result.push_back(entry.get<profile_t>());
        }
      } catch (const std::exception &e) {
        if (valid) {
          *valid = false;
        }
        BOOST_LOG(warning) << "steamshine_gpuctl: failed to parse stored profiles: "sv << e.what();
      }
      return result;
    }
  }  // namespace

  std::vector<profile_t> custom_profiles() {
    std::lock_guard lock {g_profiles_mutex};
    return custom_profiles_locked();
  }

  namespace {
    /**
     * @brief Replace the configuration before publishing custom-profile changes in memory.
     * @param profiles Complete profile list to persist.
     * @param error Failure reason, leaving the previous in-memory list intact.
     * @param selection Optional new active profile, published only after durable replacement.
     * @return True only after replacing the saved configuration.
     */
    bool persist_custom_profiles(const std::vector<profile_t> &profiles, std::string &error, const std::optional<std::string> &selection = std::nullopt) {
      nlohmann::json array = nlohmann::json::array();
      for (const auto &profile : profiles) {
        array.push_back(profile);
      }
      const auto serialized = array.dump();
      std::stringstream config_stream;
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      vars["steamshine_gpu_profiles"] = serialized;
      if (selection) {
        vars["steamshine_gpu_active_profile"] = *selection;
      }
      for (const auto &[key, value] : vars) {
        config_stream << key << " = " << value << std::endl;
      }
      const fs::path destination {config::sunshine.config_file};
      const auto temporary = destination.string() + ".gpu-profiles.tmp";
      {
        std::ofstream output {temporary, std::ios::trunc};
        output << config_stream.str();
        output.close();
        if (!output) {
          std::error_code ignored;
          fs::remove(temporary, ignored);
          error = "Unable to save GPU profiles; existing settings were retained";
          return false;
        }
      }
      std::error_code write_error;
      const auto permissions = fs::exists(destination, write_error) ? fs::status(destination, write_error).permissions() : fs::perms::owner_read | fs::perms::owner_write;
      if (!write_error) {
        fs::permissions(temporary, permissions, write_error);
      }
      if (!write_error) {
        fs::rename(temporary, destination, write_error);
      }
      if (write_error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        error = "Unable to replace GPU profile configuration; existing settings were retained";
        return false;
      }
      config::sunshine.steamshine_gpu_profiles = serialized;
      if (selection) {
        config::sunshine.steamshine_gpu_active_profile = *selection;
      }
      return true;
    }

    bool is_builtin_name(const std::string &name) {
      return std::find(BUILTIN_NAMES.begin(), BUILTIN_NAMES.end(), name) != BUILTIN_NAMES.end();
    }
  }  // namespace

  bool save_custom_profile(const profile_t &profile, std::string &error) {
    if (profile.name.empty()) {
      error = "Profile name is required";
      return false;
    }
    if (is_builtin_name(profile.name)) {
      error = "Cannot overwrite a built-in profile; choose a different name";
      return false;
    }
    if (!std::isfinite(profile.power_cap_watts) || profile.power_cap_watts < 0 ||
        !std::isfinite(profile.cpu_max_freq_mhz) || profile.cpu_max_freq_mhz < 0) {
      error = "Profile limits must be finite, non-negative values";
      return false;
    }
    std::lock_guard lock {g_profiles_mutex};
    bool valid;
    auto profiles {custom_profiles_locked(&valid)};
    if (!valid) {
      error = "Stored GPU profiles could not be read; refusing to overwrite them";
      return false;
    }
    const auto it {std::find_if(profiles.begin(), profiles.end(), [&](const profile_t &existing) {
      return existing.name == profile.name;
    })};
    if (it != profiles.end()) {
      *it = profile;
    } else {
      profiles.push_back(profile);
    }
    return persist_custom_profiles(profiles, error);
  }

  bool delete_custom_profile(const std::string &name, std::string &error) {
    if (is_builtin_name(name)) {
      error = "Cannot delete a built-in profile";
      return false;
    }
    std::lock_guard lock {g_profiles_mutex};
    bool valid;
    auto profiles {custom_profiles_locked(&valid)};
    if (!valid) {
      error = "Stored GPU profiles could not be read; refusing to overwrite them";
      return false;
    }
    const auto before_size {profiles.size()};
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const profile_t &existing) {
                     return existing.name == name;
                   }),
                   profiles.end());
    if (profiles.size() == before_size) {
      error = "Profile not found";
      return false;
    }
    return persist_custom_profiles(profiles, error);
  }

  std::string active_profile_name() {
    return config::sunshine.steamshine_gpu_active_profile;
  }

  std::optional<profile_t> active_profile() {
    const auto name {active_profile_name()};
    if (name.empty()) {
      return std::nullopt;
    }
    const auto find_named = [&name](const std::vector<profile_t> &profiles) -> std::optional<profile_t> {
      const auto found {std::ranges::find(profiles, name, &profile_t::name)};
      return found == profiles.end() ? std::nullopt : std::optional<profile_t> {*found};
    };
    if (const auto builtin {find_named(builtin_profiles())}) {
      return builtin;
    }
    return find_named(custom_profiles());
  }

  namespace {
    /**
     * @brief Apply a resolved profile and optionally persist it as selected.
     *
     * @param target Resolved profile.
     * @param persist_selection Whether to write the selected name to config.
     * @return Verified application result.
     */
    apply_result_t apply_profile(const profile_t &target, const bool persist_selection) {
      detect();
      apply_result_t result;
      const auto &caps {capabilities()};
      if (caps.power_cap_supported && (!std::isfinite(target.power_cap_watts) || target.power_cap_watts < caps.power_cap_min_watts || target.power_cap_watts > caps.power_cap_max_watts)) {
        result.success = false;
        result.error = "Profile power limit is outside the detected GPU range; edit the profile before applying";
        return result;
      }
      const bool wants_overdrive {target.gpu_clock_offset_mhz != 0 || target.gpu_voltage_offset_mv != 0};
      std::string power_microwatts {"-"};
      std::string performance_level {"-"};
      std::string cpu_governor {"-"};
      std::string cpu_max_khz {"-"};
      std::string clock_offset_mhz {"-"};
      std::string voltage_offset_mv {"-"};
      std::uint64_t requested_power_microwatts {};

      if (caps.power_cap_supported) {
        const double watts {clamp(target.power_cap_watts, caps.power_cap_min_watts, caps.power_cap_max_watts)};
        requested_power_microwatts = static_cast<std::uint64_t>(watts * 1'000'000.0);
        power_microwatts = std::to_string(requested_power_microwatts);
      } else {
        result.skipped.push_back("power_cap_watts");
      }

      if (caps.perf_level_supported) {
        performance_level = wants_overdrive ? "manual" : "auto";
      } else {
        result.skipped.push_back("gpu_perf_level");
      }

      if (caps.cpu_freq_supported) {
        const auto khz {static_cast<std::uint64_t>(clamp(target.cpu_max_freq_mhz, caps.cpu_min_freq_mhz, caps.cpu_max_freq_mhz) * 1000.0)};
        cpu_governor = target.cpu_governor;
        cpu_max_khz = std::to_string(khz);
      } else {
        result.skipped.push_back("cpu_governor");
        result.skipped.push_back("cpu_max_freq_mhz");
      }

      if (caps.od_clk_voltage_supported && wants_overdrive) {
        clock_offset_mhz = target.gpu_clock_offset_mhz == 0 ? "-" : std::to_string(target.gpu_clock_offset_mhz);
        voltage_offset_mv = target.gpu_voltage_offset_mv == 0 ? "-" : std::to_string(target.gpu_voltage_offset_mv);
      } else if (target.gpu_clock_offset_mhz != 0 || target.gpu_voltage_offset_mv != 0) {
        result.skipped.push_back("gpu_clock_offset_mhz");
        result.skipped.push_back("gpu_voltage_offset_mv");
      }

      const auto [helper_exit_code, helper_output] {run_runtime_helper({
        "apply-profile",
        power_microwatts,
        performance_level,
        cpu_governor,
        cpu_max_khz,
        clock_offset_mhz,
        voltage_offset_mv,
      })};
      const bool helper_ok {helper_exit_code == 0};
      if (caps.power_cap_supported) {
        // The helper verifies the cap while it still holds the GPU active; a
        // second unprivileged read here can race runtime suspend.
        (helper_ok ? result.applied : result.skipped).push_back("power_cap_watts");
      }
      if (caps.perf_level_supported) {
        (helper_ok ? result.applied : result.skipped).push_back("gpu_perf_level");
      }
      if (caps.cpu_freq_supported) {
        (helper_ok ? result.applied : result.skipped).push_back("cpu_governor");
        (helper_ok ? result.applied : result.skipped).push_back("cpu_max_freq_mhz");
      }
      if (caps.od_clk_voltage_supported && wants_overdrive) {
        if (target.gpu_clock_offset_mhz != 0) {
          (helper_ok ? result.applied : result.skipped).push_back("gpu_clock_offset_mhz");
        }
        if (target.gpu_voltage_offset_mv != 0) {
          (helper_ok ? result.applied : result.skipped).push_back("gpu_voltage_offset_mv");
        }
        if (!helper_ok) {
          result.skipped.push_back("gpu_overdrive_commit");
        }
      }

      const auto applied = [&](const std::string_view field) {
        return std::ranges::find(result.applied, field) != result.applied.end();
      };
      std::vector<std::string> failed_required;
      if ((caps.power_cap_supported || target.power_cap_watts > 0) && !applied("power_cap_watts")) {
        failed_required.push_back("power_cap_watts");
      }
      if (caps.perf_level_supported && !applied("gpu_perf_level")) {
        failed_required.push_back("gpu_perf_level");
      }
      if (caps.cpu_freq_supported && (!applied("cpu_governor") || !applied("cpu_max_freq_mhz"))) {
        failed_required.push_back("cpu_policy");
      }
      if (caps.od_clk_voltage_supported && wants_overdrive &&
          ((target.gpu_clock_offset_mhz != 0 && !applied("gpu_clock_offset_mhz")) ||
           (target.gpu_voltage_offset_mv != 0 && !applied("gpu_voltage_offset_mv")) ||
           std::ranges::find(result.skipped, "gpu_overdrive_commit") != result.skipped.end())) {
        failed_required.push_back("gpu_overdrive");
      }
      if (!failed_required.empty()) {
        result.success = false;
        result.error = caps.runtime_write_authorized ? "GPU profile write or verification failed" : "GPU profile writes require administrator authorization from the Web GPU page";
        BOOST_LOG(error) << "GPU_PROFILE_APPLY_FAILED profile=" << target.name
                         << " error=" << result.error
                         << " failed_fields=" << nlohmann::json(failed_required).dump()
                         << " helper_exit_code=" << helper_exit_code
                         << " helper_output=" << nlohmann::json(helper_output).dump();
      } else {
        BOOST_LOG(info) << "GPU_PROFILE_APPLIED profile=" << target.name
                        << " applied_fields=" << nlohmann::json(result.applied).dump();
      }

      if (persist_selection && result.success) {
        std::lock_guard lock {g_profiles_mutex};
        bool valid;
        const auto profiles {custom_profiles_locked(&valid)};
        if (!valid) {
          result.success = false;
          result.error = "Stored GPU profiles could not be read; selection was not saved";
        } else if (!persist_custom_profiles(profiles, result.error, target.name)) {
          result.success = false;
        }
      }
      return result;
    }
  }  // namespace

  apply_result_t activate_profile(const std::string &name) {
    refresh_capabilities();
    apply_result_t result;
    std::optional<profile_t> target;
    for (const auto &profile : builtin_profiles()) {
      if (profile.name == name) {
        target = profile;
      }
    }
    if (!target) {
      for (const auto &profile : custom_profiles()) {
        if (profile.name == name) {
          target = profile;
        }
      }
    }
    if (!target) {
      result.success = false;
      result.error = "Profile not found";
      return result;
    }
    return apply_profile(*target, true);
  }

  apply_result_t reapply_active_profile() {
    refresh_capabilities();
    const auto target {active_profile()};
    return target ? apply_profile(*target, false) : apply_result_t {};
  }

  void to_json(nlohmann::json &json, const capabilities_t &value) {
    json = nlohmann::json {
      {"gpu_present", value.gpu_present},
      {"gpu_name", value.gpu_name},
      {"runtime_write_authorized", value.runtime_write_authorized},
      {"power_cap_supported", value.power_cap_supported},
      {"power_cap_min_watts", value.power_cap_min_watts},
      {"power_cap_max_watts", value.power_cap_max_watts},
      {"power_cap_default_watts", value.power_cap_default_watts},
      {"perf_level_supported", value.perf_level_supported},
      {"od_clk_voltage_supported", value.od_clk_voltage_supported},
      {"cpu_freq_supported", value.cpu_freq_supported},
      {"cpu_min_freq_mhz", value.cpu_min_freq_mhz},
      {"cpu_max_freq_mhz", value.cpu_max_freq_mhz},
      {"cpu_governors", value.cpu_governors},
    };
  }

  void to_json(nlohmann::json &json, const profile_t &value) {
    json = nlohmann::json {
      {"name", value.name},
      {"description", value.description},
      {"builtin", value.builtin},
      {"power_cap_watts", value.power_cap_watts},
      {"cpu_governor", value.cpu_governor},
      {"cpu_max_freq_mhz", value.cpu_max_freq_mhz},
      {"gpu_clock_offset_mhz", value.gpu_clock_offset_mhz},
      {"gpu_voltage_offset_mv", value.gpu_voltage_offset_mv},
    };
  }

  void from_json(const nlohmann::json &json, profile_t &value) {
    value.name = json.value("name", "");
    value.description = json.value("description", "");
    value.builtin = json.value("builtin", false);
    value.power_cap_watts = json.value("power_cap_watts", 0.0);
    value.cpu_governor = json.value("cpu_governor", "");
    value.cpu_max_freq_mhz = json.value("cpu_max_freq_mhz", 0.0);
    value.gpu_clock_offset_mhz = json.value("gpu_clock_offset_mhz", 0);
    value.gpu_voltage_offset_mv = json.value("gpu_voltage_offset_mv", 0);
  }

  void to_json(nlohmann::json &json, const apply_result_t &value) {
    json = nlohmann::json {
      {"success", value.success},
      {"error", value.error},
      {"applied", value.applied},
      {"skipped", value.skipped},
    };
  }

}  // namespace steamshine_gpuctl
