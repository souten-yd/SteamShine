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
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#if defined(__linux__)
  #include <sys/capability.h>
#endif

using namespace std::literals;

namespace steamshine_gpuctl {

  namespace fs = std::filesystem;

  namespace {

    constexpr std::array<const char *, 4> BUILTIN_NAMES {"Silent", "Balanced", "Performance", "OC"};

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
      fs::path power_cap_path;  ///< `gpu_hwmon_dir/power1_cap`.
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
    hardware_paths_t g_paths;

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

          if (!g_paths.gpu_hwmon_dir.empty()) {
            g_paths.power_cap_path = g_paths.gpu_hwmon_dir / "power1_cap";
            const auto cap_min {read_double(g_paths.gpu_hwmon_dir / "power1_cap_min")};
            const auto cap_max {read_double(g_paths.gpu_hwmon_dir / "power1_cap_max")};
            const auto cap_default {read_double(g_paths.gpu_hwmon_dir / "power1_cap_default")};
            if (cap_min && cap_max && fs::exists(g_paths.power_cap_path)) {
              g_capabilities.power_cap_supported = true;
              g_capabilities.power_cap_min_watts = *cap_min / 1'000'000.0;
              g_capabilities.power_cap_max_watts = *cap_max / 1'000'000.0;
              g_capabilities.power_cap_default_watts = cap_default.value_or(*cap_max) / 1'000'000.0;
            }
          }

          g_paths.perf_level_path = *device_dir / "power_dpm_force_performance_level";
          g_capabilities.perf_level_supported = fs::exists(g_paths.perf_level_path);

          g_paths.od_clk_voltage_path = *device_dir / "pp_od_clk_voltage";
          g_capabilities.od_clk_voltage_supported = fs::exists(g_paths.od_clk_voltage_path);
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

#if defined(__linux__)
    /**
     * @brief Run `fn` with `cap` briefly raised in the effective capability set, then drop it again.
     *
     * `cap` must already be present in the process's permitted set (granted via
     * `setcap ... +p` on the Sunshine binary at install time); this only toggles
     * the effective flag for the duration of `fn`, mirroring the existing
     * `has_elevated_privileges`/`drop_elevated_privileges` pattern used for capture.
     *
     * @return True when the capability could be raised (regardless of what `fn` did internally).
     */
    bool with_capability(cap_value_t cap, const std::function<void()> &fn) {
      cap_t caps {cap_get_proc()};
      if (!caps) {
        return false;
      }
      bool raised {false};
      if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap, CAP_SET) == 0 && cap_set_proc(caps) == 0) {
        raised = true;
        fn();
      }
      cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap, CAP_CLEAR);
      cap_set_proc(caps);
      cap_free(caps);
      return raised;
    }
#else
    bool with_capability(int /*cap*/, const std::function<void()> & /*fn*/) {
      return false;
    }
#endif

    /**
     * @brief Write one value to an allow-listed absolute path under a briefly-elevated capability.
     *
     * @return True when the capability could be raised and the write completed without an exception.
     */
    bool privileged_write(const fs::path &path, const std::string &content) {
      if (path.empty() || !fs::exists(path)) {
        return false;
      }
      bool wrote {false};
#if defined(__linux__)
      with_capability(CAP_DAC_OVERRIDE, [&] {
        std::ofstream out {path};
        if (out) {
          out << content;
          wrote = static_cast<bool>(out);
        }
      });
#endif
      return wrote;
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

  const capabilities_t &capabilities() {
    detect();
    return g_capabilities;
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
     */
    std::vector<profile_t> custom_profiles_locked() {
      std::vector<profile_t> result;
      if (config::sunshine.steamshine_gpu_profiles.empty()) {
        return result;
      }
      try {
        const auto parsed {nlohmann::json::parse(config::sunshine.steamshine_gpu_profiles)};
        for (const auto &entry : parsed) {
          result.push_back(entry.get<profile_t>());
        }
      } catch (const std::exception &e) {
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
     * @brief Persist the given custom-profile list back to the configuration file.
     */
    void persist_custom_profiles(const std::vector<profile_t> &profiles) {
      nlohmann::json array = nlohmann::json::array();
      for (const auto &profile : profiles) {
        array.push_back(profile);
      }
      config::sunshine.steamshine_gpu_profiles = array.dump();
      std::stringstream config_stream;
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      vars["steamshine_gpu_profiles"] = config::sunshine.steamshine_gpu_profiles;
      for (const auto &[key, value] : vars) {
        config_stream << key << " = " << value << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
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
    std::lock_guard lock {g_profiles_mutex};
    auto profiles {custom_profiles_locked()};
    const auto it {std::find_if(profiles.begin(), profiles.end(), [&](const profile_t &existing) {
      return existing.name == profile.name;
    })};
    if (it != profiles.end()) {
      *it = profile;
    } else {
      profiles.push_back(profile);
    }
    persist_custom_profiles(profiles);
    return true;
  }

  bool delete_custom_profile(const std::string &name, std::string &error) {
    if (is_builtin_name(name)) {
      error = "Cannot delete a built-in profile";
      return false;
    }
    std::lock_guard lock {g_profiles_mutex};
    auto profiles {custom_profiles_locked()};
    const auto before_size {profiles.size()};
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const profile_t &existing) {
                     return existing.name == name;
                   }),
                   profiles.end());
    if (profiles.size() == before_size) {
      error = "Profile not found";
      return false;
    }
    persist_custom_profiles(profiles);
    return true;
  }

  std::string active_profile_name() {
    return config::sunshine.steamshine_gpu_active_profile;
  }

  apply_result_t activate_profile(const std::string &name) {
    detect();
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
      return result;
    }

    const auto &caps {capabilities()};
    const bool wants_overdrive {target->gpu_clock_offset_mhz != 0 || target->gpu_voltage_offset_mv != 0};

    if (caps.power_cap_supported) {
      const double watts {clamp(target->power_cap_watts, caps.power_cap_min_watts, caps.power_cap_max_watts)};
      const auto microwatts {static_cast<std::uint64_t>(watts * 1'000'000.0)};
      (privileged_write(g_paths.power_cap_path, std::to_string(microwatts)) ? result.applied : result.skipped).push_back("power_cap_watts");
    } else {
      result.skipped.push_back("power_cap_watts");
    }

    if (caps.perf_level_supported) {
      (privileged_write(g_paths.perf_level_path, wants_overdrive ? "manual" : "auto") ? result.applied : result.skipped).push_back("gpu_perf_level");
    } else {
      result.skipped.push_back("gpu_perf_level");
    }

    if (caps.cpu_freq_supported) {
      bool governor_ok {!target->cpu_governor.empty()};
      bool freq_ok {true};
      const auto khz {static_cast<std::uint64_t>(clamp(target->cpu_max_freq_mhz, caps.cpu_min_freq_mhz, caps.cpu_max_freq_mhz) * 1000.0)};
      for (const auto &path : g_paths.cpu_governor_paths) {
        governor_ok = governor_ok && privileged_write(path, target->cpu_governor);
      }
      for (const auto &path : g_paths.cpu_max_freq_paths) {
        freq_ok = freq_ok && privileged_write(path, std::to_string(khz));
      }
      (governor_ok ? result.applied : result.skipped).push_back("cpu_governor");
      (freq_ok ? result.applied : result.skipped).push_back("cpu_max_freq_mhz");
    } else {
      result.skipped.push_back("cpu_governor");
      result.skipped.push_back("cpu_max_freq_mhz");
    }

    if (caps.od_clk_voltage_supported && wants_overdrive) {
      // Best-effort: the exact `pp_od_clk_voltage` command grammar (state index,
      // absolute vs. offset semantics, whether a separate voltage-offset command
      // is supported at all) varies by GPU generation and amdgpu driver version,
      // and could not be verified against real hardware in development (this
      // build environment's GPU does not expose this file). Each line is written
      // independently and a failure only skips that one field.
      const bool clock_ok {target->gpu_clock_offset_mhz == 0 || privileged_write(g_paths.od_clk_voltage_path, std::format("s 1 {}\n", target->gpu_clock_offset_mhz))};
      const bool voltage_ok {target->gpu_voltage_offset_mv == 0 || privileged_write(g_paths.od_clk_voltage_path, std::format("vo {}\n", target->gpu_voltage_offset_mv))};
      const bool commit_ok {privileged_write(g_paths.od_clk_voltage_path, "c\n")};
      (clock_ok ? result.applied : result.skipped).push_back("gpu_clock_offset_mhz");
      (voltage_ok ? result.applied : result.skipped).push_back("gpu_voltage_offset_mv");
      if (!commit_ok) {
        result.skipped.push_back("gpu_overdrive_commit");
      }
    } else if (target->gpu_clock_offset_mhz != 0 || target->gpu_voltage_offset_mv != 0) {
      result.skipped.push_back("gpu_clock_offset_mhz");
      result.skipped.push_back("gpu_voltage_offset_mv");
    }

    config::sunshine.steamshine_gpu_active_profile = target->name;
    {
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      vars["steamshine_gpu_active_profile"] = target->name;
      std::stringstream config_stream;
      for (const auto &[key, value] : vars) {
        config_stream << key << " = " << value << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
    }

    return result;
  }

  void to_json(nlohmann::json &json, const capabilities_t &value) {
    json = nlohmann::json {
      {"gpu_present", value.gpu_present},
      {"gpu_name", value.gpu_name},
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
      {"applied", value.applied},
      {"skipped", value.skipped},
    };
  }

}  // namespace steamshine_gpuctl
