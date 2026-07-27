/**
 * @file src/steamshine_hwmonitor.h
 * @brief Read-only CPU, memory, and AMD GPU telemetry for the SteamShine web UI.
 */
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace nlohmann {
  /**
   * @brief Serialize `std::optional<T>` as either its value or JSON `null`.
   *
   * nlohmann::json has no built-in support for std::optional; every telemetry
   * field below that may be unreadable on a given host is optional, so this
   * specialization is what lets to_json() below use plain initializer-list syntax.
   */
  template<typename T>
  struct adl_serializer<std::optional<T>> {
    static void to_json(json &json_value, const std::optional<T> &value) {
      if (value) {
        json_value = *value;
      } else {
        json_value = nullptr;
      }
    }
  };
}  // namespace nlohmann

namespace steamshine_hwmonitor {

  /**
   * @brief Point-in-time AMD GPU telemetry, when an AMD GPU is present.
   */
  struct gpu_snapshot_t {
    std::string name;  ///< Human-readable identifier (PCI vendor:device fallback).
    std::optional<double> utilization_percent;  ///< `gpu_busy_percent`, when exposed.
    std::optional<std::uint64_t> vram_used_bytes;  ///< `mem_info_vram_used`, when exposed.
    std::optional<std::uint64_t> vram_total_bytes;  ///< `mem_info_vram_total`, when exposed.
    std::optional<double> temperature_c;  ///< Edge temperature from hwmon `temp1_input`.
    std::optional<double> hotspot_c;  ///< Junction/hotspot temperature from hwmon `temp2_input`, when present.
    std::optional<int> fan_rpm;  ///< hwmon `fan1_input`, when present.
    std::optional<double> power_watts;  ///< hwmon `power1_average`, when present.
    std::optional<double> power_cap_watts;  ///< hwmon `power1_cap`, when present.
  };

  /**
   * @brief Point-in-time CPU telemetry, derived from a two-sample `/proc/stat` delta.
   */
  struct cpu_snapshot_t {
    double percent {0.0};  ///< Aggregate utilization across all cores.
    std::vector<double> per_cpu;  ///< Per-core utilization, in `/proc/stat` order.
    int cores {0};  ///< Logical core count.
    std::optional<double> freq_mhz;  ///< Current clock of cpu0, when exposed by cpufreq.
    std::optional<double> temperature_c;  ///< Highest reading from a detected CPU hwmon sensor.
    std::vector<double> load;  ///< 1/5/15 minute load averages from `/proc/loadavg`.
  };

  /**
   * @brief Point-in-time system memory telemetry.
   */
  struct memory_snapshot_t {
    double percent {0.0};  ///< `(total - available) / total * 100`.
    std::uint64_t used {0};  ///< Bytes in use.
    std::uint64_t total {0};  ///< Total installed bytes.
  };

  /**
   * @brief One polled snapshot returned to the web UI.
   */
  struct metrics_snapshot_t {
    double uptime_seconds {0.0};  ///< Host uptime from `/proc/uptime`.
    cpu_snapshot_t cpu;  ///< CPU telemetry.
    memory_snapshot_t memory;  ///< Memory telemetry.
    std::optional<gpu_snapshot_t> gpu;  ///< AMD GPU telemetry, when one was detected.
  };

  /**
   * @brief Sample current CPU, memory, and AMD GPU telemetry.
   *
   * Maintains mutex-guarded previous-sample state internally so CPU
   * percentages can be derived from the delta between successive calls;
   * the first call after process start returns a zeroed CPU percentage.
   * Safe to call from any thread. All sysfs/procfs reads are best-effort:
   * a value that cannot be read is left unset (`std::nullopt`) rather than
   * failing the whole snapshot.
   *
   * @return The current metrics snapshot.
   */
  metrics_snapshot_t sample();

  /**
   * @brief Serialize a metrics snapshot to JSON (found via ADL from `nlohmann::json`).
   */
  void to_json(nlohmann::json &json, const gpu_snapshot_t &value);

  /**
   * @brief Serialize a metrics snapshot to JSON (found via ADL from `nlohmann::json`).
   */
  void to_json(nlohmann::json &json, const metrics_snapshot_t &value);

}  // namespace steamshine_hwmonitor
