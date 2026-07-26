/**
 * @file src/platform/linux/steam_session.h
 * @brief Steam process location classification for shared Gamescope sessions.
 */
#pragma once

#include <cstdint>
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
    int parent_pid {-1};  ///< Parent process ID.
    std::string executable_name;  ///< Basename of `/proc/<pid>/exe`.
    std::string xdg_runtime_directory;  ///< `XDG_RUNTIME_DIR` environment value.
    std::string wayland_display;  ///< `WAYLAND_DISPLAY` environment value.
    std::string x11_display;  ///< `DISPLAY` environment value.
    std::string cgroup;  ///< Process cgroup membership.
    bool metadata_readable {true};  ///< Whether required process metadata was read safely.
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
