/**
 * @file src/platform/linux/gamescope_source.cpp
 * @brief Verified Gamescope PipeWire source selection implementation.
 */
#include "gamescope_source.h"

#include <algorithm>

namespace gamescope_source {
  namespace {
    /**
     * @brief Check whether a source meets universal identity and GPU constraints.
     *
     * @param source Candidate source to inspect.
     * @param request Source-selection constraints.
     * @return True only when the candidate is safe to consider further.
     */
    bool eligible(const gamescope_source_t &source, const source_selection_request_t &request) {
      return source.identity_verified && source.media_class == "Video/Source" && source.producer_pid > 0 && source.producer_start_time != 0 && !source.executable.empty() && (request.required_render_node.empty() || source.render_node == request.required_render_node);
    }

    /**
     * @brief Find one unique source matching an origin and optional explicit PID.
     *
     * @param sources Candidate source list.
     * @param request Source-selection constraints.
     * @param origin Required ownership origin.
     * @return The unique source, or a fail-closed selection error.
     */
    std::expected<gamescope_source_t, source_error_e> select_unique(const std::vector<gamescope_source_t> &sources, const source_selection_request_t &request, const steamos_virtual_session::session_origin_e origin) {
      std::vector<const gamescope_source_t *> matches;
      for (const auto &source : sources) {
        if (!eligible(source, request) || source.origin != origin) {
          continue;
        }
        if (request.explicit_gamescope_pid && source.producer_pid != *request.explicit_gamescope_pid) {
          continue;
        }
        if (origin == steamos_virtual_session::session_origin_e::attached_existing && !source.game_mode_verified) {
          continue;
        }
        matches.push_back(&source);
      }
      if (matches.empty()) {
        return std::unexpected {request.explicit_gamescope_pid ? source_error_e::explicit_pid_invalid : source_error_e::unavailable};
      }
      if (matches.size() != 1) {
        return std::unexpected {source_error_e::ambiguous};
      }
      return *matches.front();
    }
  }  // namespace

  std::expected<gamescope_source_t, source_error_e> select_gamescope_source(const std::vector<gamescope_source_t> &sources, const source_selection_request_t &request) {
    using steamos_virtual_session::session_origin_e;
    using steamos_virtual_session::session_source_policy_e;

    if (request.policy == session_source_policy_e::existing_gamescope) {
      return select_unique(sources, request, session_origin_e::attached_existing);
    }
    if (request.policy == session_source_policy_e::owned_private) {
      return select_unique(sources, request, session_origin_e::owned_private);
    }

    const auto existing {select_unique(sources, request, session_origin_e::attached_existing)};
    if (existing.has_value() || existing.error() == source_error_e::ambiguous || request.explicit_gamescope_pid) {
      return existing;
    }
    return select_unique(sources, request, session_origin_e::owned_private);
  }
}  // namespace gamescope_source
