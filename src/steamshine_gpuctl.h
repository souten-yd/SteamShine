/**
 * @file src/steamshine_gpuctl.h
 * @brief AMD GPU/CPU performance-profile detection and control for the SteamShine web UI.
 *
 * Sunshine runs as an unprivileged systemd user service, but the sysfs
 * attributes that expose GPU power limits, performance levels, overdrive
 * clock/voltage offsets, and CPU frequency scaling are root-owned (0644).
 * Writes here briefly raise `CAP_DAC_OVERRIDE` (granted to the Sunshine
 * binary at install time via `setcap`, alongside the existing
 * `CAP_SYS_ADMIN`/`CAP_SYS_NICE` grant used for capture) immediately before
 * each write and drop it again immediately after, so the process only ever
 * bypasses file permission checks for the exact instant it needs to.
 *
 * All writes target an allow-list of absolute paths resolved once at
 * detection time from real sysfs enumeration -- no path is ever built from
 * caller-supplied input -- and every value is clamped to the range the
 * hardware itself reports before being written. Unsupported fields (for
 * example, GPUs/drivers that do not expose `pp_od_clk_voltage`) are skipped
 * rather than failing the whole profile.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace steamshine_gpuctl {

  /**
   * @brief Sysfs-derived hardware capabilities, detected once and cached for the process lifetime.
   */
  struct capabilities_t {
    bool gpu_present {false};  ///< Whether an AMD (`amdgpu`-driven) GPU was found.
    std::string gpu_name;  ///< Human-readable identifier for the detected GPU.
    bool power_cap_supported {false};  ///< Whether hwmon `power1_cap` (+ `_min`/`_max`/`_default`) is exposed.
    double power_cap_min_watts {0.0};  ///< Lowest power limit the hardware will accept.
    double power_cap_max_watts {0.0};  ///< Highest power limit the hardware will accept.
    double power_cap_default_watts {0.0};  ///< Factory-default power limit.
    bool perf_level_supported {false};  ///< Whether `power_dpm_force_performance_level` is exposed.
    bool od_clk_voltage_supported {false};  ///< Whether `pp_od_clk_voltage` (GPU overdrive) is exposed.
    bool cpu_freq_supported {false};  ///< Whether cpufreq scaling is exposed for cpu0.
    double cpu_min_freq_mhz {0.0};  ///< Lowest CPU clock the hardware will accept.
    double cpu_max_freq_mhz {0.0};  ///< Highest CPU clock the hardware will accept.
    std::vector<std::string> cpu_governors;  ///< Governors reported by `scaling_available_governors`.
  };

  /**
   * @brief One named GPU/CPU performance profile.
   */
  struct profile_t {
    std::string name;  ///< Unique profile name.
    std::string description;  ///< Short human-readable description.
    bool builtin {false};  ///< Whether this is one of the four built-in profiles (cannot be edited/deleted).
    double power_cap_watts {0.0};  ///< Desired GPU power limit.
    std::string cpu_governor;  ///< Desired CPU governor (must be one reported by capabilities().cpu_governors).
    double cpu_max_freq_mhz {0.0};  ///< Desired CPU maximum clock.
    int gpu_clock_offset_mhz {0};  ///< Desired GPU core clock offset from the hardware's default maximum, when supported.
    int gpu_voltage_offset_mv {0};  ///< Desired GPU core voltage offset (typically negative, for undervolting), when supported.
  };

  /**
   * @brief Outcome of applying one profile.
   */
  struct apply_result_t {
    bool success {true};  ///< False only when the profile itself could not be found/read.
    std::vector<std::string> applied;  ///< Field names that were written successfully.
    std::vector<std::string> skipped;  ///< Field names skipped because the hardware does not support them, or that failed to write.
  };

  /**
   * @brief Detect and cache hardware capabilities for the process lifetime.
   *
   * @return The detected capabilities.
   */
  const capabilities_t &capabilities();

  /**
   * @brief Return the four built-in profiles (Silent/Balanced/Performance/OC),
   * scaled from the detected capability bounds so they are always in range.
   */
  std::vector<profile_t> builtin_profiles();

  /**
   * @brief Return all custom profiles persisted in the SteamShine configuration.
   */
  std::vector<profile_t> custom_profiles();

  /**
   * @brief Create or update one custom profile.
   *
   * @param profile Profile to save; rejected when its name matches a built-in profile.
   * @param error Set to a human-readable reason when this call returns false.
   * @return True on success.
   */
  bool save_custom_profile(const profile_t &profile, std::string &error);

  /**
   * @brief Delete one custom profile by name.
   *
   * @param name Profile name; rejected when it matches a built-in profile.
   * @param error Set to a human-readable reason when this call returns false.
   * @return True on success.
   */
  bool delete_custom_profile(const std::string &name, std::string &error);

  /**
   * @brief Return the name of the profile most recently activated, or an empty string.
   */
  std::string active_profile_name();

  /**
   * @brief Apply a profile (built-in or custom) by name.
   *
   * Every field is clamped to the bounds reported by capabilities() and
   * written independently via a briefly-elevated capability; a field the
   * hardware does not support, or that fails to write, is recorded in
   * `apply_result_t::skipped` rather than aborting the remaining fields.
   *
   * @param name Profile name to look up and apply.
   * @return The outcome of the attempt.
   */
  apply_result_t activate_profile(const std::string &name);

  void to_json(nlohmann::json &json, const capabilities_t &value);
  void to_json(nlohmann::json &json, const profile_t &value);
  void from_json(const nlohmann::json &json, profile_t &value);
  void to_json(nlohmann::json &json, const apply_result_t &value);

}  // namespace steamshine_gpuctl
