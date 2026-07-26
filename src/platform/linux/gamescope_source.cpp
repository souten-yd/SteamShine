/**
 * @file src/platform/linux/gamescope_source.cpp
 * @brief Verified Gamescope PipeWire source selection implementation.
 */
#include "gamescope_source.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>

#if defined(__linux__)
  #include <sys/stat.h>
#endif

namespace gamescope_source {
  namespace {
    /**
     * @brief Read the Linux kernel process start-time field from a stat record.
     *
     * @param contents Complete `/proc/<pid>/stat` contents.
     * @return Field 22, or std::nullopt for malformed records.
     */
    std::optional<uint64_t> process_start_time_from_stat(const std::string &contents) {
      const auto command_end {contents.rfind(')')};
      if (command_end == std::string::npos || command_end + 2 >= contents.size()) {
        return std::nullopt;
      }
      std::istringstream fields {contents.substr(command_end + 2)};
      std::string field;
      for (int index {0}; index < 20; ++index) {
        if (!(fields >> field)) {
          return std::nullopt;
        }
      }
      try {
        size_t consumed {};
        const auto value {std::stoull(field, &consumed)};
        return consumed == field.size() ? std::optional<uint64_t> {value} : std::nullopt;
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }

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

  std::optional<process_identity_t> read_process_identity(const int pid) {
#if defined(__linux__)
    if (pid <= 0) {
      return std::nullopt;
    }
    const std::filesystem::path process_directory {"/proc/" + std::to_string(pid)};
    struct stat process_stat {};
    if (::stat(process_directory.c_str(), &process_stat) != 0) {
      return std::nullopt;
    }
    std::ifstream stat_file {process_directory / "stat"};
    const std::string stat_contents {std::istreambuf_iterator<char> {stat_file}, {}};
    const auto start_time {process_start_time_from_stat(stat_contents)};
    std::error_code error;
    const auto executable {std::filesystem::canonical(process_directory / "exe", error)};
    if (!start_time || error || executable.empty()) {
      return std::nullopt;
    }
    return process_identity_t {
      .pid = pid,
      .uid = static_cast<int>(process_stat.st_uid),
      .start_time = *start_time,
      .executable = executable,
    };
#else
    (void) pid;
    return std::nullopt;
#endif
  }

  bool source_identity_is_current(const gamescope_source_t &source) {
    const auto identity {read_process_identity(source.producer_pid)};
    return identity && identity->uid == source.producer_uid && identity->start_time == source.producer_start_time && identity->executable == source.executable && identity->executable.filename() == "gamescope";
  }

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
