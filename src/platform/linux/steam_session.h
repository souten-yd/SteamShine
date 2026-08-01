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
    std::string executable_path;  ///< Canonical executable path used for a verified graceful shutdown request.
    std::string xdg_runtime_directory;  ///< `XDG_RUNTIME_DIR` environment value.
    std::string wayland_display;  ///< `WAYLAND_DISPLAY` environment value.
    std::string x11_display;  ///< `DISPLAY` environment value.
    std::string xauthority;  ///< `XAUTHORITY` environment value.
    std::string gamescope_wayland_display;  ///< `GAMESCOPE_WAYLAND_DISPLAY` environment value.
    std::string dbus_session_bus_address;  ///< `DBUS_SESSION_BUS_ADDRESS` environment value.
    std::string xdg_session_type;  ///< `XDG_SESSION_TYPE` environment value.
    std::string xdg_current_desktop;  ///< `XDG_CURRENT_DESKTOP` environment value.
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
    std::string xdg_session_type;  ///< Resident display protocol classification.
    std::string xdg_current_desktop;  ///< Resident desktop identity.
    bool allows_authless_xwayland {false};  ///< Whether the exact SteamOS vendor unit pair permits an omitted `XAUTHORITY`.
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
   * @brief Result of checking whether Desktop Steam can be migrated safely.
   */
  enum class migration_idle_result_e {
    idle,  ///< One verified Desktop Steam exists and no game process is active.
    active_game,  ///< A non-reaper process remains in a Steam game scope.
    unknown,  ///< Steam identity, endpoint, or scope metadata is ambiguous.
  };

  /**
   * @brief Verified Desktop Steam process and allow-listed shutdown environment.
   */
  struct migration_candidate_t {
    migration_idle_result_e result {migration_idle_result_e::unknown};  ///< Safety classification.
    int steam_pid {-1};  ///< Unique Steam PID when verified.
    uint64_t steam_start_time {0};  ///< Start time binding the candidate against PID reuse.
    std::string executable_path;  ///< Canonical Steam executable invoked with `-shutdown`.
    resident_environment_t environment;  ///< Allow-listed original Desktop environment.
  };

  /**
   * @brief Assess immutable process metadata for safe Desktop Steam migration.
   *
   * A game scope containing only residual `reaper` processes is idle. Any
   * other process in such a scope blocks migration. Missing metadata,
   * multiple Steam executables, or endpoint mismatches fail closed.
   *
   * @param records Current-user process snapshot.
   * @param desktop_runtime Verified KWin runtime directory.
   * @param desktop_wayland Verified KWin Wayland display name.
   * @param current_uid Required process owner.
   * @return Candidate and classification.
   */
  migration_candidate_t assess_idle_desktop_migration(const std::vector<process_record_t> &records, std::string_view desktop_runtime, std::string_view desktop_wayland, int current_uid);

  /**
   * @brief Inspect live procfs for a safe idle Desktop Steam candidate.
   *
   * @param desktop_runtime Verified KWin runtime directory.
   * @param desktop_wayland Verified KWin Wayland display name.
   * @return Candidate and fail-closed classification.
   */
  migration_candidate_t inspect_idle_desktop_migration(std::string_view desktop_runtime, std::string_view desktop_wayland);

  /**
   * @brief Assess whether a verified stock Game Mode Steam session is idle.
   *
   * The target must belong to the exact vendor Gamescope unit and Steam must
   * be the unique current-user singleton in the sibling launcher unit. Any
   * non-reaper Steam game-scope process reports an active game.
   *
   * @param records Current-user process snapshot.
   * @param target Verified stock Gamescope identity.
   * @param current_uid Required process owner.
   * @return Candidate and fail-closed activity classification.
   */
  migration_candidate_t assess_idle_stock_session(const std::vector<process_record_t> &records, const target_session_t &target, int current_uid);

  /**
   * @brief Inspect live procfs for a safe idle stock Game Mode candidate.
   *
   * @param target Verified stock Gamescope identity.
   * @return Candidate and fail-closed activity classification.
   */
  migration_candidate_t inspect_idle_stock_session(const target_session_t &target);

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
   * Selection validates the unique current-user `steam` executable itself.
   * Steam-family game children outside the target do not invalidate a vendor
   * Game Mode endpoint; singleton placement checks remain the responsibility
   * of `classify_instance_location()`.
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
   * @brief Determine whether a command asks Steam to enter Big Picture mode.
   *
   * @param command Configured application, prep, or detached command.
   * @return True when the command contains the canonical Big Picture open URI.
   */
  bool command_opens_big_picture(std::string_view command);

  /**
   * @brief Determine whether a command asks Steam to leave Big Picture mode.
   *
   * @param command Configured application or undo command.
   * @return True when the command contains the canonical Big Picture close URI.
   */
  bool command_closes_big_picture(std::string_view command);

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

  /**
   * @brief Return the stable idle-migration classification spelling.
   *
   * @param result Classification to serialize.
   * @return Lowercase status label.
   */
  std::string_view to_string(migration_idle_result_e result);
}  // namespace steam_session
