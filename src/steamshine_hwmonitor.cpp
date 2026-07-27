/**
 * @file src/steamshine_hwmonitor.cpp
 * @brief Read-only CPU, memory, and AMD GPU telemetry for the SteamShine web UI.
 */
#include "steamshine_hwmonitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace steamshine_hwmonitor {

  namespace fs = std::filesystem;

  namespace {

    /**
     * @brief Read one trimmed sysfs/procfs attribute without invoking external tools.
     *
     * @param path Attribute path.
     * @return Trimmed content, or an empty string when the file could not be read.
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

    /**
     * @brief Read a sysfs attribute as an unsigned 64-bit integer.
     *
     * @param path Attribute path.
     * @return Parsed value, or no value when the file was missing or unparsable.
     */
    std::optional<std::uint64_t> read_uint(const fs::path &path) {
      const auto text {read_attribute(path)};
      if (text.empty()) {
        return std::nullopt;
      }
      try {
        return std::stoull(text);
      } catch (...) {
        return std::nullopt;
      }
    }

    /**
     * @brief Per-core CPU jiffie counters parsed from one `/proc/stat` line.
     */
    struct cpu_times_t {
      std::uint64_t idle {0};
      std::uint64_t total {0};
    };

    /**
     * @brief Parse one `/proc/stat` `cpu*` line into idle/total jiffie counters.
     *
     * @param line Raw `/proc/stat` line beginning with `cpu`.
     * @return Parsed counters.
     */
    cpu_times_t parse_stat_line(const std::string &line) {
      std::istringstream stream {line};
      std::string label;
      stream >> label;
      std::vector<std::uint64_t> fields;
      std::uint64_t field;
      while (stream >> field) {
        fields.push_back(field);
      }
      cpu_times_t times;
      for (std::size_t i = 0; i < fields.size(); ++i) {
        times.total += fields[i];
        if (i == 3 || i == 4) {  // idle + iowait
          times.idle += fields[i];
        }
      }
      return times;
    }

    /**
     * @brief Snapshot of every `/proc/stat` `cpu*` line, keyed by label (`cpu`, `cpu0`, ...).
     */
    std::unordered_map<std::string, cpu_times_t> read_proc_stat() {
      std::unordered_map<std::string, cpu_times_t> result;
      std::ifstream input {"/proc/stat"};
      std::string line;
      while (std::getline(input, line)) {
        if (line.rfind("cpu", 0) != 0) {
          continue;
        }
        std::istringstream stream {line};
        std::string label;
        stream >> label;
        result.emplace(label, parse_stat_line(line));
      }
      return result;
    }

    /**
     * @brief Previous `/proc/stat` sample, guarded for delta-based percentage math.
     */
    std::mutex g_cpu_mutex;
    std::unordered_map<std::string, cpu_times_t> g_previous_stat;

    /**
     * @brief Utilization percentage from a previous/current jiffie counter pair.
     */
    double percent_from_delta(const cpu_times_t &previous, const cpu_times_t &current) {
      if (current.total <= previous.total) {
        return 0.0;
      }
      const auto total_delta {static_cast<double>(current.total - previous.total)};
      const auto idle_delta {static_cast<double>(current.idle - std::min(previous.idle, current.idle))};
      return std::clamp(100.0 * (1.0 - idle_delta / total_delta), 0.0, 100.0);
    }

    /**
     * @brief Locate a CPU hwmon directory by sensor-chip name (`coretemp`, `k10temp`, `zenpower`).
     *
     * @return Highest `temp*_input` reading in degrees Celsius, when found.
     */
    std::optional<double> read_cpu_temperature() {
      std::error_code error;
      static const std::vector<std::string> known_names {"coretemp", "k10temp", "zenpower"};
      for (const auto &entry : fs::directory_iterator {"/sys/class/hwmon", error}) {
        const auto name {read_attribute(entry.path() / "name")};
        if (std::find(known_names.begin(), known_names.end(), name) == known_names.end()) {
          continue;
        }
        std::optional<double> highest;
        for (const auto &sensor : fs::directory_iterator {entry.path(), error}) {
          const auto filename {sensor.path().filename().string()};
          if (filename.rfind("temp", 0) != 0 || filename.find("_input") == std::string::npos) {
            continue;
          }
          if (const auto milli {read_uint(sensor.path())}) {
            const double celsius {static_cast<double>(*milli) / 1000.0};
            if (!highest || celsius > *highest) {
              highest = celsius;
            }
          }
        }
        if (highest) {
          return highest;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief AMD GPU sysfs locations resolved once and reused across samples.
     */
    struct amd_gpu_paths_t {
      fs::path device_dir;  ///< `/sys/class/drm/card*/device`.
      fs::path hwmon_dir;  ///< `device_dir/hwmon/hwmon*`, when present.
      std::string name;  ///< Fallback display name derived from the PCI ID.
    };

    /**
     * @brief Locate the first AMD (`amdgpu`-driven) DRM card, if any.
     *
     * @return Resolved sysfs locations, or no value when no AMD GPU is present.
     */
    std::optional<amd_gpu_paths_t> locate_amd_gpu() {
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
        amd_gpu_paths_t paths;
        paths.device_dir = device_dir;
        const auto device_id {read_attribute(device_dir / "device")};
        paths.name = device_id.empty() ? "AMD GPU" : ("AMD GPU (1002:" + (device_id.rfind("0x", 0) == 0 ? device_id.substr(2) : device_id) + ")");
        std::error_code hwmon_error;
        const auto hwmon_root {device_dir / "hwmon"};
        for (const auto &hwmon_entry : fs::directory_iterator {hwmon_root, hwmon_error}) {
          paths.hwmon_dir = hwmon_entry.path();
          break;
        }
        return paths;
      }
      return std::nullopt;
    }

    /**
     * @brief Sample AMD GPU telemetry for the resolved sysfs locations.
     */
    gpu_snapshot_t sample_gpu(const amd_gpu_paths_t &paths) {
      gpu_snapshot_t snapshot;
      snapshot.name = paths.name;
      if (const auto busy {read_uint(paths.device_dir / "gpu_busy_percent")}) {
        snapshot.utilization_percent = static_cast<double>(*busy);
      }
      snapshot.vram_used_bytes = read_uint(paths.device_dir / "mem_info_vram_used");
      snapshot.vram_total_bytes = read_uint(paths.device_dir / "mem_info_vram_total");
      if (!paths.hwmon_dir.empty()) {
        if (const auto milli {read_uint(paths.hwmon_dir / "temp1_input")}) {
          snapshot.temperature_c = static_cast<double>(*milli) / 1000.0;
        }
        if (const auto milli {read_uint(paths.hwmon_dir / "temp2_input")}) {
          snapshot.hotspot_c = static_cast<double>(*milli) / 1000.0;
        }
        if (const auto rpm {read_uint(paths.hwmon_dir / "fan1_input")}) {
          snapshot.fan_rpm = static_cast<int>(*rpm);
        }
        if (const auto micro {read_uint(paths.hwmon_dir / "power1_average")}) {
          snapshot.power_watts = static_cast<double>(*micro) / 1'000'000.0;
        }
        if (const auto micro {read_uint(paths.hwmon_dir / "power1_cap")}) {
          snapshot.power_cap_watts = static_cast<double>(*micro) / 1'000'000.0;
        }
      }
      return snapshot;
    }

  }  // namespace

  metrics_snapshot_t sample() {
    metrics_snapshot_t snapshot;

    // CPU: derive percentages from the delta against the previous sample.
    {
      auto current_stat {read_proc_stat()};
      std::lock_guard lock {g_cpu_mutex};
      if (const auto it {current_stat.find("cpu")}; it != current_stat.end()) {
        if (const auto previous_it {g_previous_stat.find("cpu")}; previous_it != g_previous_stat.end()) {
          snapshot.cpu.percent = percent_from_delta(previous_it->second, it->second);
        }
      }
      for (int core = 0;; ++core) {
        const auto label {"cpu" + std::to_string(core)};
        const auto it {current_stat.find(label)};
        if (it == current_stat.end()) {
          break;
        }
        double core_percent {0.0};
        if (const auto previous_it {g_previous_stat.find(label)}; previous_it != g_previous_stat.end()) {
          core_percent = percent_from_delta(previous_it->second, it->second);
        }
        snapshot.cpu.per_cpu.push_back(core_percent);
      }
      snapshot.cpu.cores = static_cast<int>(snapshot.cpu.per_cpu.size());
      g_previous_stat = std::move(current_stat);
    }

    // Load average.
    {
      std::ifstream input {"/proc/loadavg"};
      double one {}, five {}, fifteen {};
      if (input >> one >> five >> fifteen) {
        snapshot.cpu.load = {one, five, fifteen};
      }
    }

    // Current clock of cpu0, when exposed by cpufreq.
    if (const auto khz {read_uint("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")}) {
      snapshot.cpu.freq_mhz = static_cast<double>(*khz) / 1000.0;
    }

    snapshot.cpu.temperature_c = read_cpu_temperature();

    // Memory.
    {
      std::ifstream input {"/proc/meminfo"};
      std::string key;
      std::uint64_t value {};
      std::string unit;
      std::optional<std::uint64_t> total_kb;
      std::optional<std::uint64_t> available_kb;
      while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
          total_kb = value;
        } else if (key == "MemAvailable:") {
          available_kb = value;
        }
      }
      if (total_kb) {
        snapshot.memory.total = *total_kb * 1024;
        const auto available {available_kb.value_or(0)};
        snapshot.memory.used = (*total_kb > available ? (*total_kb - available) : 0) * 1024;
        snapshot.memory.percent = *total_kb > 0 ? std::clamp(100.0 * static_cast<double>(*total_kb - available) / static_cast<double>(*total_kb), 0.0, 100.0) : 0.0;
      }
    }

    // Uptime.
    {
      std::ifstream input {"/proc/uptime"};
      double uptime {};
      if (input >> uptime) {
        snapshot.uptime_seconds = uptime;
      }
    }

    // AMD GPU, when present.
    if (const auto paths {locate_amd_gpu()}) {
      snapshot.gpu = sample_gpu(*paths);
    }

    return snapshot;
  }

  void to_json(nlohmann::json &json, const gpu_snapshot_t &value) {
    json = nlohmann::json {
      {"name", value.name},
      {"utilization_percent", value.utilization_percent},
      {"vram_used_bytes", value.vram_used_bytes},
      {"vram_total_bytes", value.vram_total_bytes},
      {"temperature_c", value.temperature_c},
      {"hotspot_c", value.hotspot_c},
      {"fan_rpm", value.fan_rpm},
      {"power_watts", value.power_watts},
      {"power_cap_watts", value.power_cap_watts},
    };
  }

  void to_json(nlohmann::json &json, const metrics_snapshot_t &value) {
    json = nlohmann::json {
      {"uptime_seconds", value.uptime_seconds},
      {"cpu",
       {
         {"percent", value.cpu.percent},
         {"per_cpu", value.cpu.per_cpu},
         {"cores", value.cpu.cores},
         {"freq_mhz", value.cpu.freq_mhz},
         {"temperature_c", value.cpu.temperature_c},
         {"load", value.cpu.load},
       }},
      {"memory",
       {
         {"percent", value.memory.percent},
         {"used", value.memory.used},
         {"total", value.memory.total},
       }},
      {"gpu", value.gpu ? nlohmann::json(*value.gpu) : nlohmann::json(nullptr)},
    };
  }

}  // namespace steamshine_hwmonitor
