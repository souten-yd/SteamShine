/**
 * @file src/platform/linux/steam_session.h
 * @brief Steam process location classification for shared Gamescope sessions.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace steam_session {
  /**
   * @brief Location of the current-user Steam singleton relative to a target Gamescope.
   */
  enum class instance_location_e {
    absent,  ///< No current-user Steam-family process was discovered.
    inside_target_gamescope,  ///< Steam is verified to belong to the target Gamescope session.
    outside_target_gamescope,  ///< Steam exists but is verified outside the target session.
    unknown,  ///< Process metadata could not be read safely.
  };

  /**
   * @brief Metadata read from one current-user Steam-family process.
   */
  struct process_record_t {
    int pid {-1};  ///< Process ID.
    int uid {-1};  ///< Process owner UID.
    int parent_pid {-1};  ///< Parent process ID.
    uint64_t start_time {0};  ///< Kernel process start time used to reject PID reuse.
    std::string executable_name;  ///< Basename of `/proc/<pid>/exe`.
    std::string xdg_runtime_directory;  ///< `XDG_RUNTIME_DIR` environment value.
    std::string wayland_display;  ///< `WAYLAND_DISPLAY` environment value.
    std::string x11_display;  ///< `DISPLAY` environment value.
    std::string xauthority;  ///< `XAUTHORITY` environment value.
    std::string gamescope_wayland_display;  ///< `GAMESCOPE_WAYLAND_DISPLAY` environment value.
    std::string dbus_session_bus_address;  ///< `DBUS_SESSION_BUS_ADDRESS` environment value.
    std::string cgroup;  ///< Process cgroup membership.
    bool metadata_readable {true};  ///< Whether required process metadata was read safely.
  };

  /**
   * @brief Allow-listed environment inherited from one verified resident Steam process.
   */
  struct resident_environment_t {
    int steam_pid {-1};  ///< Resident Steam process supplying the environment.
    uint64_t steam_start_time {0};  ///< Start time binding the snapshot to that process.
    std::string xdg_runtime_directory;  ///< `XDG_RUNTIME_DIR` value.
    std::string wayland_display;  ///< `WAYLAND_DISPLAY` value.
    std::string gamescope_wayland_display;  ///< `GAMESCOPE_WAYLAND_DISPLAY` value.
    std::string x11_display;  ///< Dynamic Gamescope `DISPLAY` value.
    std::string xauthority;  ///< Xwayland authorization file path.
    std::string dbus_session_bus_address;  ///< Resident session bus address.
  };

  /**
   * @brief Immutable target session identity used for Steam placement checks.
   */
  struct target_session_t {
    int gamescope_pid {-1};  ///< Target Gamescope PID.
    std::string runtime_directory;  ///< Target private runtime directory, when owned.
    std::string wayland_display;  ///< Target private Wayland display name, when owned.
    std::string cgroup;  ///< Target Gamescope cgroup, when available.
  };

  /**
   * @brief Classify the current-user Steam singleton relative to one Gamescope target.
   *
   * @param records Current current-user Steam-family process records.
   * @param target Verified Gamescope session identity.
   * @return Safe Steam location classification.
   */
  instance_location_e classify_instance_location(const std::vector<process_record_t> &records, const target_session_t &target);

  /**
   * @brief Inspect current-user `/proc` entries and classify the live Steam singleton.
   *
   * @param target Verified Gamescope session identity.
   * @return Live Steam location, or unknown when required target metadata is unreadable.
   */
  instance_location_e classify_current_user_instance(const target_session_t &target);

  /**
   * @brief Read an allow-listed endpoint from the unique verified resident Steam process.
   *
   * UID, executable, start time, cgroup, and Gamescope parent membership are
   * re-read from procfs. Ambiguous, outside, or unreadable instances fail closed.
   *
   * @param target Verified Gamescope target used for membership validation.
   * @return Resident endpoint environment, or no value when it is unavailable.
   */
  std::optional<resident_environment_t> verified_resident_environment(const target_session_t &target);

  /**
   * @brief Select one resident Steam environment from an immutable process snapshot.
   *
   * @param records Process metadata snapshot, including parent-chain records.
   * @param target Verified Gamescope target.
   * @param current_uid UID which every accepted record must match.
   * @return Unique allow-listed resident environment, or no value on ambiguity or failed identity.
   */
  std::optional<resident_environment_t> select_resident_environment(const std::vector<process_record_t> &records, const target_session_t &target, int current_uid);

  /**
   * @brief Determine whether a shell command can start or address Steam.
   *
   * The check is intentionally conservative: a Steam executable path and a
   * `steam://` URI both count as a Steam launch request. It is used only to
   * prevent creating a second Steam singleton while a verified one is outside
   * the selected Gamescope session.
   *
   * @param command Configured application, prep, or detached command.
   * @return True when the command contains a standalone Steam reference.
   */
  bool command_references_steam(std::string_view command);

  /**
   * @brief Read the cgroup membership of one process without invoking external tools.
   *
   * @param pid Process ID to inspect.
   * @return Raw cgroup membership, or an empty string when it cannot be read.
   */
  std::string cgroup_for_process(int pid);

  /**
   * @brief Return a stable diagnostic spelling for a Steam location classification.
   *
   * @param location Classification to serialize.
   * @return Lowercase status label.
   */
  std::string_view to_string(instance_location_e location);
}  // namespace steam_session
