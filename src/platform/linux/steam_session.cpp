/**
 * @file src/platform/linux/steam_session.cpp
 * @brief Steam process location classification for shared Gamescope sessions.
 */
#include "steam_session.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

#if defined(__linux__)
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace steam_session {
  namespace {
    /**
     * @brief Check whether a process name belongs to the Steam singleton family.
     *
     * @param executable_name Process executable basename.
     * @return True when the process participates in Steam session placement.
     */
    bool is_steam_family_process(const std::string_view executable_name) {
      return executable_name == "steam" || executable_name == "steamwebhelper" || executable_name == "reaper" || executable_name == "pressure-vessel";
    }

    /**
     * @brief Check whether a process reaches the target Gamescope through parent links.
     *
     * @param record Process to inspect.
     * @param by_pid Records indexed by PID.
     * @param gamescope_pid Target Gamescope PID.
     * @return True only when the parent chain reaches the target without a cycle.
     */
    bool has_target_parent(const process_record_t &record, const std::unordered_map<int, const process_record_t *> &by_pid, const int gamescope_pid) {
      int current_pid {record.parent_pid};
      for (size_t depth {}; current_pid > 0 && depth < by_pid.size(); ++depth) {
        if (current_pid == gamescope_pid) {
          return true;
        }
        const auto current {by_pid.find(current_pid)};
        if (current == by_pid.end()) {
          return false;
        }
        current_pid = current->second->parent_pid;
      }
      return false;
    }

    /**
     * @brief Check direct environment or cgroup membership in a target Gamescope.
     *
     * @param record Steam-family process metadata.
     * @param target Target Gamescope identity.
     * @param by_pid Records indexed by PID.
     * @return True only when one independent target-membership signal matches.
     */
    bool belongs_to_target(const process_record_t &record, const target_session_t &target, const std::unordered_map<int, const process_record_t *> &by_pid) {
      const bool runtime_match {!target.runtime_directory.empty() && record.xdg_runtime_directory == target.runtime_directory && !target.wayland_display.empty() && record.wayland_display == target.wayland_display};
      const bool cgroup_match {!target.cgroup.empty() && record.cgroup == target.cgroup};
      return runtime_match || cgroup_match || has_target_parent(record, by_pid, target.gamescope_pid);
    }

    /**
     * @brief Read one NUL-separated environment value.
     *
     * @param contents Complete `/proc/<pid>/environ` contents.
     * @param name Environment variable name.
     * @return Value when present, otherwise an empty string.
     */
    std::string environment_value(const std::string &contents, const std::string_view name) {
      const std::string prefix {std::string {name} + '='};
      size_t start {};
      while (start < contents.size()) {
        const auto end {contents.find('\0', start)};
        const auto length {(end == std::string::npos ? contents.size() : end) - start};
        const std::string_view entry {contents.data() + start, length};
        if (entry.starts_with(prefix)) {
          return std::string {entry.substr(prefix.size())};
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return {};
    }

    /**
     * @brief Read the parent PID field from a Linux stat record.
     *
     * @param contents Complete `/proc/<pid>/stat` contents.
     * @return Parent PID, or -1 when the record is malformed.
     */
    int parent_pid_from_stat(const std::string &contents) {
      const auto command_end {contents.rfind(')')};
      if (command_end == std::string::npos || command_end + 2 >= contents.size()) {
        return -1;
      }
      std::istringstream fields {contents.substr(command_end + 2)};
      std::string state;
      int parent_pid {-1};
      return fields >> state >> parent_pid ? parent_pid : -1;
    }

#if defined(__linux__)
    /**
     * @brief Read one current-user process record from `/proc`.
     *
     * @param process_directory Numeric `/proc/<pid>` directory.
     * @return Process record when the entry belongs to this user.
     */
    std::optional<process_record_t> read_process_record(const std::filesystem::path &process_directory) {
      struct stat process_stat {};
      if (::stat(process_directory.c_str(), &process_stat) != 0 || process_stat.st_uid != ::getuid()) {
        return std::nullopt;
      }
      const auto name {process_directory.filename().string()};
      int pid {-1};
      try {
        pid = std::stoi(name);
      } catch (const std::exception &) {
        return std::nullopt;
      }
      process_record_t record {.pid = pid};
      std::error_code error;
      const auto executable {std::filesystem::canonical(process_directory / "exe", error)};
      if (error || executable.empty()) {
        record.metadata_readable = false;
        return record;
      }
      record.executable_name = executable.filename().string();
      std::ifstream stat_file {process_directory / "stat"};
      const std::string stat_contents {std::istreambuf_iterator<char> {stat_file}, {}};
      record.parent_pid = parent_pid_from_stat(stat_contents);
      std::ifstream environment_file {process_directory / "environ", std::ios::binary};
      const bool environment_readable {static_cast<bool>(environment_file)};
      const std::string environment_contents {std::istreambuf_iterator<char> {environment_file}, {}};
      std::ifstream cgroup_file {process_directory / "cgroup"};
      const bool cgroup_readable {static_cast<bool>(cgroup_file)};
      record.cgroup.assign(std::istreambuf_iterator<char> {cgroup_file}, {});
      record.metadata_readable = record.parent_pid > 0 && environment_readable && cgroup_readable;
      record.xdg_runtime_directory = environment_value(environment_contents, "XDG_RUNTIME_DIR");
      record.wayland_display = environment_value(environment_contents, "WAYLAND_DISPLAY");
      record.x11_display = environment_value(environment_contents, "DISPLAY");
      return record;
    }
#endif
  }  // namespace

  instance_location_e classify_instance_location(const std::vector<process_record_t> &records, const target_session_t &target) {
    std::unordered_map<int, const process_record_t *> by_pid;
    by_pid.reserve(records.size());
    for (const auto &record : records) {
      if (record.pid > 0) {
        by_pid.emplace(record.pid, &record);
      }
    }
    bool steam_present {};
    for (const auto &record : records) {
      if (!is_steam_family_process(record.executable_name)) {
        continue;
      }
      steam_present = true;
      if (!record.metadata_readable) {
        return instance_location_e::unknown;
      }
      if (target.gamescope_pid <= 0) {
        return instance_location_e::unknown;
      }
      if (belongs_to_target(record, target, by_pid)) {
        return instance_location_e::inside_target_gamescope;
      }
    }
    return steam_present ? instance_location_e::outside_target_gamescope : instance_location_e::absent;
  }

  instance_location_e classify_current_user_instance(const target_session_t &target) {
#if defined(__linux__)
    std::error_code error;
    std::vector<process_record_t> records;
    for (const auto &entry : std::filesystem::directory_iterator {"/proc", error}) {
      if (error || !entry.is_directory(error)) {
        continue;
      }
      const auto name {entry.path().filename().string()};
      if (name.empty() || !std::all_of(name.begin(), name.end(), [](const unsigned char character) {
            return std::isdigit(character);
          })) {
        continue;
      }
      if (const auto record {read_process_record(entry.path())}) {
        records.emplace_back(*record);
      }
    }
    return classify_instance_location(records, target);
#else
    (void) target;
    return instance_location_e::unknown;
#endif
  }

  std::string cgroup_for_process(const int pid) {
#if defined(__linux__)
    if (pid <= 0) {
      return {};
    }
    std::ifstream cgroup_file {"/proc/" + std::to_string(pid) + "/cgroup"};
    return {std::istreambuf_iterator<char> {cgroup_file}, {}};
#else
    (void) pid;
    return {};
#endif
  }

  std::string_view to_string(const instance_location_e location) {
    switch (location) {
      case instance_location_e::absent:
        return "absent";
      case instance_location_e::inside_target_gamescope:
        return "inside_target_gamescope";
      case instance_location_e::outside_target_gamescope:
        return "outside_target_gamescope";
      case instance_location_e::unknown:
        return "unknown";
    }
    return "unknown";
  }
}  // namespace steam_session
