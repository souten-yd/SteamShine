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
     * @brief Determine whether cgroup membership identifies a Gamescope scope.
     *
     * A normal user-session cgroup is shared by unrelated desktop processes,
     * so equality alone is not sufficient evidence that Steam belongs to a
     * target Gamescope. SteamOS Game Mode scopes contain a Gamescope-specific
     * component and may be used as an additional identity signal.
     *
     * @param cgroup Raw cgroup membership text.
     * @return True when the cgroup path contains a Gamescope-specific scope.
     */
    bool is_gamescope_specific_cgroup(const std::string_view cgroup) {
      return cgroup.find("gamescope") != std::string_view::npos;
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
      const bool cgroup_match {!target.cgroup.empty() && record.cgroup == target.cgroup && is_gamescope_specific_cgroup(target.cgroup)};
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

    /**
     * @brief Read the process start time from a Linux stat record.
     *
     * @param contents Complete `/proc/<pid>/stat` contents.
     * @return Field 22, or zero for malformed input.
     */
    uint64_t start_time_from_stat(const std::string &contents) {
      const auto command_end {contents.rfind(')')};
      if (command_end == std::string::npos || command_end + 2 >= contents.size()) {
        return 0;
      }
      std::istringstream fields {contents.substr(command_end + 2)};
      std::string field;
      for (int index {}; index < 20; ++index) {
        if (!(fields >> field)) {
          return 0;
        }
      }
      try {
        size_t consumed {};
        const auto value {std::stoull(field, &consumed)};
        return consumed == field.size() ? value : 0;
      } catch (const std::exception &) {
        return 0;
      }
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
      process_record_t record {.pid = pid, .uid = static_cast<int>(process_stat.st_uid)};
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
      record.start_time = start_time_from_stat(stat_contents);
      std::ifstream environment_file {process_directory / "environ", std::ios::binary};
      const bool environment_readable {static_cast<bool>(environment_file)};
      const std::string environment_contents {std::istreambuf_iterator<char> {environment_file}, {}};
      std::ifstream cgroup_file {process_directory / "cgroup"};
      const bool cgroup_readable {static_cast<bool>(cgroup_file)};
      record.cgroup.assign(std::istreambuf_iterator<char> {cgroup_file}, {});
      record.metadata_readable = record.parent_pid > 0 && record.start_time != 0 && environment_readable && cgroup_readable;
      record.xdg_runtime_directory = environment_value(environment_contents, "XDG_RUNTIME_DIR");
      record.wayland_display = environment_value(environment_contents, "WAYLAND_DISPLAY");
      record.x11_display = environment_value(environment_contents, "DISPLAY");
      record.xauthority = environment_value(environment_contents, "XAUTHORITY");
      record.gamescope_wayland_display = environment_value(environment_contents, "GAMESCOPE_WAYLAND_DISPLAY");
      record.dbus_session_bus_address = environment_value(environment_contents, "DBUS_SESSION_BUS_ADDRESS");
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
    bool steam_inside_target {};
    bool steam_outside_target {};
    for (const auto &record : records) {
      if (!is_steam_family_process(record.executable_name)) {
        continue;
      }
      if (!record.metadata_readable) {
        return instance_location_e::unknown;
      }
      if (target.gamescope_pid <= 0) {
        return instance_location_e::unknown;
      }
      if (belongs_to_target(record, target, by_pid)) {
        steam_inside_target = true;
      } else {
        steam_outside_target = true;
      }
    }
    // An inner Steam process never makes an outer one safe.  Treat a mixed
    // placement as outside so a caller cannot start a second singleton.
    if (steam_outside_target) {
      return instance_location_e::outside_target_gamescope;
    }
    return steam_inside_target ? instance_location_e::inside_target_gamescope : instance_location_e::absent;
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

  std::optional<resident_environment_t> verified_resident_environment(const target_session_t &target) {
#if defined(__linux__)
    std::error_code error;
    std::vector<process_record_t> records;
    for (const auto &entry : std::filesystem::directory_iterator {"/proc", error}) {
      if (error || !entry.is_directory(error)) {
        continue;
      }
      const auto name {entry.path().filename().string()};
      if (name.empty() || !std::ranges::all_of(name, [](const unsigned char character) {
            return std::isdigit(character);
          })) {
        continue;
      }
      if (const auto record {read_process_record(entry.path())}) {
        records.emplace_back(*record);
      }
    }
    return select_resident_environment(records, target, static_cast<int>(::getuid()));
#else
    (void) target;
    return std::nullopt;
#endif
  }

  std::optional<resident_environment_t> select_resident_environment(const std::vector<process_record_t> &records, const target_session_t &target, const int current_uid) {
    if (classify_instance_location(records, target) != instance_location_e::inside_target_gamescope) {
      return std::nullopt;
    }
    std::unordered_map<int, const process_record_t *> by_pid;
    for (const auto &record : records) {
      by_pid.emplace(record.pid, &record);
    }
    const process_record_t *resident {};
    for (const auto &record : records) {
      if (record.executable_name != "steam" || record.uid != current_uid || record.start_time == 0 || !record.metadata_readable || !belongs_to_target(record, target, by_pid)) {
        continue;
      }
      if (resident) {
        return std::nullopt;
      }
      resident = &record;
    }
    if (!resident) {
      return std::nullopt;
    }
    return resident_environment_t {
      .steam_pid = resident->pid,
      .steam_start_time = resident->start_time,
      .xdg_runtime_directory = resident->xdg_runtime_directory,
      .wayland_display = resident->wayland_display,
      .gamescope_wayland_display = resident->gamescope_wayland_display,
      .x11_display = resident->x11_display,
      .xauthority = resident->xauthority,
      .dbus_session_bus_address = resident->dbus_session_bus_address,
    };
  }

  bool command_references_steam(const std::string_view command) {
    constexpr std::string_view steam {"steam"};
    size_t offset {};
    while ((offset = command.find(steam, offset)) != std::string_view::npos) {
      const auto is_identifier_character {[&command](const size_t position) {
        const auto character {static_cast<unsigned char>(command[position])};
        return std::isalnum(character) || character == '_';
      }};
      const bool preceding_identifier {offset > 0 && is_identifier_character(offset - 1)};
      const size_t after {offset + steam.size()};
      const bool following_identifier {after < command.size() && is_identifier_character(after)};
      if (!preceding_identifier && !following_identifier) {
        return true;
      }
      offset = after;
    }
    return false;
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
