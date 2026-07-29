/**
 * @file src/platform/linux/gamescope_source.h
 * @brief Verified Gamescope PipeWire source selection declarations.
 */
#pragma once

#include "src/steamos_virtual_session_core.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope_source {
  struct gamescope_source_t;

  /**
   * @brief Immutable process identity used to detect PID reuse.
   */
  struct process_identity_t {
    int pid {-1};  ///< Process identifier observed from PipeWire or `/proc`.
    int uid {-1};  ///< Owning user ID from `/proc/<pid>` metadata.
    uint64_t start_time {0};  ///< Field 22 from `/proc/<pid>/stat`.
    std::filesystem::path executable;  ///< Canonical `/proc/<pid>/exe` target.
  };

  /**
   * @brief Read a process identity without following a PID across reuse.
   *
   * @param pid Process ID to inspect.
   * @return Current process identity, or std::nullopt when it cannot be read safely.
   */
  std::optional<process_identity_t> read_process_identity(int pid);

  /**
   * @brief Validate procfs command evidence for a capability-restricted Gamescope.
   *
   * @param command_line Nul-separated `/proc/<pid>/cmdline` content.
   * @param comm `/proc/<pid>/comm` content without its trailing newline.
   * @return True only when both procfs fields identify a Gamescope executable.
   */
  bool has_gamescope_command_identity(std::string_view command_line, std::string_view comm);

  /**
   * @brief Validate that a Gamescope process belongs to a Steam Game Mode session.
   *
   * SteamOS vendor sessions are identified by their exact systemd user-unit
   * cgroup component. Other launchers must provide both a Steam command marker
   * and Steam-related cgroup evidence.
   *
   * @param command_line Nul-separated `/proc/<pid>/cmdline` content.
   * @param cgroup Complete `/proc/<pid>/cgroup` content.
   * @return True only when the process has verified Game Mode session evidence.
   */
  bool has_game_mode_session_identity(std::string_view command_line, std::string_view cgroup);

  /**
   * @brief Check whether a PipeWire media class can represent Gamescope capture output.
   *
   * Current Gamescope versions publish `Stream/Output/Video`; the legacy
   * `Video/Source` spelling remains accepted for compatible producers.
   *
   * @param media_class PipeWire `media.class` property.
   * @return True only for a supported video-producing media class.
   */
  bool is_gamescope_capture_media_class(std::string_view media_class);

  /**
   * @brief Verify that a source still identifies the originally discovered Gamescope process.
   *
   * @param source Source descriptor to validate against current `/proc` state.
   * @return True only when PID, UID, start time, and executable still match Gamescope.
   */
  bool source_identity_is_current(const gamescope_source_t &source);

  /**
   * @brief Discover current-user Gamescope capture-output nodes from one PipeWire core.
   *
   * @param runtime_directory Host PipeWire runtime directory.
   * @param remote_name Host PipeWire socket name.
   * @param timeout Maximum time to receive registry globals.
   * @param error Receives a stable non-secret failure reason.
   * @return Verified candidate descriptors; an empty result is safe to treat as unavailable.
   */
  std::vector<gamescope_source_t> discover_gamescope_sources(const std::string &runtime_directory, std::string_view remote_name, std::chrono::milliseconds timeout, std::string &error);

  /**
   * @brief Open a validated current-user host PipeWire socket for one consumer.
   *
   * The caller owns the returned descriptor and must transfer or close it. A
   * descriptor is intentionally never shared between separate consumers.
   *
   * @param runtime_directory Host PipeWire runtime directory.
   * @param remote_name Host PipeWire socket name.
   * @param error Receives a stable non-secret failure reason.
   * @return Connected descriptor, or std::nullopt when validation or connection fails.
   */
  std::optional<int> open_host_pipewire_socket(const std::string &runtime_directory, std::string_view remote_name, std::string &error);

  /**
   * @brief Stable identity and PipeWire metadata for one Gamescope capture-output node.
   */
  struct gamescope_source_t {
    uint32_t node_id {UINT32_MAX};  ///< Volatile PipeWire node ID.
    uint64_t object_serial {UINT64_MAX};  ///< Stable PipeWire object serial.
    uint32_t client_id {UINT32_MAX};  ///< PipeWire Client global ID.
    int producer_pid {-1};  ///< Gamescope producer process ID.
    int producer_uid {-1};  ///< Gamescope producer user ID.
    uint64_t producer_start_time {0};  ///< `/proc/<pid>/stat` process start time.
    std::string executable;  ///< Canonical `/proc/<pid>/exe` target.
    std::string node_name;  ///< PipeWire node name.
    std::string node_description;  ///< PipeWire node description.
    std::string application_name;  ///< PipeWire application name.
    std::string media_class;  ///< PipeWire media class.
    std::string render_node;  ///< Optional producer DRM render node.
    steamos_virtual_session::session_origin_e origin {steamos_virtual_session::session_origin_e::none};  ///< Ownership origin.
    bool identity_verified {false};  ///< PID, start time, executable, and UID were verified together.
    bool game_mode_verified {false};  ///< Candidate is a verified current-user Game Mode session.
  };

  /**
   * @brief Reason why source selection failed closed.
   */
  enum class source_error_e {
    unavailable,  ///< No candidate fulfilled the requested policy.
    ambiguous,  ///< More than one equally eligible candidate was found.
    explicit_pid_invalid,  ///< The configured producer PID did not identify one eligible source.
  };

  /**
   * @brief Constraints for choosing one verified Gamescope source.
   */
  struct source_selection_request_t {
    steamos_virtual_session::session_source_policy_e policy {steamos_virtual_session::session_source_policy_e::auto_select};  ///< Source policy from configuration.
    std::optional<int> explicit_gamescope_pid;  ///< Optional administrator-selected Gamescope PID.
    std::string required_render_node;  ///< Selected capture/encoder DRM render node; present producer metadata must match it.
  };

  /**
   * @brief Select one source without accepting ambiguous or unverified candidates.
   *
   * @param sources Registry snapshot of discovered candidate sources.
   * @param request Immutable source-selection constraints.
   * @return One verified source, or a fail-closed selection error.
   */
  std::expected<gamescope_source_t, source_error_e> select_gamescope_source(const std::vector<gamescope_source_t> &sources, const source_selection_request_t &request);
}  // namespace gamescope_source
