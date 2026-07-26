/**
 * @file src/platform/linux/gamescope_source.h
 * @brief Verified Gamescope PipeWire source selection declarations.
 */
#pragma once

#include "src/steamos_virtual_session_core.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace gamescope_source {
  /**
   * @brief Stable identity and PipeWire metadata for one Gamescope Video/Source node.
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
    std::string render_node;  ///< Producer DRM render node.
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
    std::string required_render_node;  ///< Selected capture/encoder DRM render node.
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
