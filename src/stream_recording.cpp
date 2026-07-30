/**
 * @file src/stream_recording.cpp
 * @brief Sender-side encoded stream recording and bounded retention.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <fcntl.h>
  #include <pthread.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#if defined(__linux__)
/** @brief Process environment forwarded unchanged to the FFmpeg muxer. */
extern char **environ;
#endif

// local includes
#include "logging.h"
#include "stream_recording.h"

namespace stream_recording {
  namespace {
    constexpr std::uint64_t BYTES_PER_MEGABYTE = 1024U * 1024U;  ///< Binary megabyte used by recording limits.
    constexpr std::uint64_t MAXIMUM_CAPACITY_MB = 102400U;  ///< Maximum configurable capacity accepted from Web clients.
    constexpr std::size_t MAXIMUM_QUEUE_BYTES = 32U * 1024U * 1024U;  ///< Bounded writer queue that cannot back up the sender indefinitely.

    /**
     * @brief Resolve the owner-private default recording directory.
     *
     * @return Absolute recording directory, or an empty path without a valid user state root.
     */
    std::filesystem::path default_root() {
      if (const auto *const state_home {std::getenv("XDG_STATE_HOME")}; state_home && *state_home) {
        const std::filesystem::path path {state_home};
        if (path.is_absolute()) {
          return path / "steamshine" / "recordings";
        }
      }
      if (const auto *const user_home {std::getenv("HOME")}; user_home && *user_home) {
        const std::filesystem::path path {user_home};
        if (path.is_absolute()) {
          return path / ".local" / "state" / "steamshine" / "recordings";
        }
      }
      return {};
    }

    /**
     * @brief Return whether an identifier can name only one generated recording.
     *
     * @param id Candidate identifier.
     * @return True for a bounded ASCII identifier without path separators.
     */
    bool valid_id(const std::string_view id) {
      return !id.empty() && id.size() <= 64U && std::ranges::all_of(id, [](const unsigned char character) {
        return std::isalnum(character) || character == '-';
      });
    }

    /**
     * @brief Return the FFmpeg raw demuxer for a GameStream codec identifier.
     *
     * @param codec GameStream codec identifier.
     * @return Raw demuxer name, or an empty view for unsupported codecs.
     */
    std::string_view input_format(const int codec) {
      switch (codec) {
        case 0:
          return "h264";
        case 1:
          return "hevc";
        case 2:
          return "av1";
        default:
          return {};
      }
    }

    /**
     * @brief Write a JSON document through owner-private atomic replacement.
     *
     * @param path Destination document path.
     * @param value JSON value to persist.
     * @return True when replacement completed.
     */
    bool write_private_json(const std::filesystem::path &path, const nlohmann::json &value) {
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) {
        return false;
      }
      std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
      const auto temporary {path.string() + ".tmp"};
      {
        std::ofstream output {temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
          return false;
        }
        output << value.dump(2) << '\n';
      }
      std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
      std::filesystem::rename(temporary, path, error);
      if (error) {
        std::filesystem::remove(temporary, error);
        return false;
      }
      return true;
    }
  }  // namespace

  class Service::Impl {
  public:
    /**
     * @brief Construct storage state and start its isolated writer.
     *
     * @param requested_root Explicit or default recording root.
     * @param executable FFmpeg executable name or path.
     */
    Impl(std::filesystem::path requested_root, std::string executable):
        root_ {requested_root.empty() ? default_root() : std::move(requested_root)},
        ffmpeg_executable_ {std::move(executable)} {
      load_settings();
      enforce_capacity();
      worker_ = std::thread {[this]() {
        run();
      }};
    }

    /**
     * @brief Stop and join the writer.
     */
    ~Impl() {
      {
        std::scoped_lock lock {mutex_};
        enabled_ = false;
        commands_.push_back(command_t {.kind = command_kind_e::shutdown});
      }
      wake_.notify_one();
      worker_.join();
    }

    /** @copydoc Service::set_enabled */
    result_t set_enabled(const bool enabled) {
      std::scoped_lock lock {mutex_};
      if (root_.empty()) {
        return {false, "recording_path_unavailable", "No owner-private recording directory is available."};
      }
      if (enabled_ == enabled) {
        return {true, enabled ? "recording_already_enabled" : "recording_already_disabled", enabled ? "Recording is already armed." : "Recording is already stopped."};
      }
      enabled_ = enabled;
      last_error_.clear();
      if (!enabled) {
        commands_.push_back(command_t {.kind = command_kind_e::stop});
        wake_.notify_one();
      }
      return {true, enabled ? "recording_armed" : "recording_stopping", enabled ? "Recording is armed and will begin on the next key frame." : "Recording is stopping."};
    }

    /** @copydoc Service::set_capacity_megabytes */
    result_t set_capacity_megabytes(const std::uint64_t capacity_mb) {
      if (capacity_mb == 0 || capacity_mb > MAXIMUM_CAPACITY_MB) {
        return {false, "invalid_recording_capacity", "Recording capacity must be between 1 and 102400 MB."};
      }
      std::uint64_t previous_capacity {};
      {
        std::scoped_lock lock {mutex_};
        previous_capacity = capacity_mb_;
        capacity_mb_ = capacity_mb;
      }
      if (!write_private_json(root_ / "settings.json", {{"schema_version", 1}, {"capacity_mb", capacity_mb}})) {
        std::scoped_lock lock {mutex_};
        capacity_mb_ = previous_capacity;
        return {false, "recording_settings_save_failed", "Unable to save recording capacity."};
      }
      enforce_capacity();
      return {true, "recording_capacity_saved", "Recording capacity saved and retention applied."};
    }

    /** @copydoc Service::submit */
    void submit(const frame_t &frame) {
      if (frame.payload.empty()) {
        return;
      }
      std::scoped_lock lock {mutex_};
      if (!enabled_) {
        return;
      }
      if (queued_bytes_ + frame.payload.size() > MAXIMUM_QUEUE_BYTES) {
        enabled_ = false;
        last_error_ = "Recording stopped because its writer queue exceeded 32 MB.";
        commands_.push_back(command_t {.kind = command_kind_e::stop});
        wake_.notify_one();
        return;
      }
      command_t command;
      command.kind = command_kind_e::frame;
      command.bytes.assign(frame.payload.begin(), frame.payload.end());
      command.stream_id = frame.stream_id;
      command.frame_index = frame.frame_index;
      command.codec = frame.codec;
      command.width = frame.width;
      command.height = frame.height;
      command.fps = frame.fps;
      command.hdr = frame.hdr;
      command.idr = frame.idr;
      queued_bytes_ += command.bytes.size();
      commands_.push_back(std::move(command));
      wake_.notify_one();
    }

    /** @copydoc Service::stream_ended */
    void stream_ended(const std::uintptr_t stream_id) {
      std::scoped_lock lock {mutex_};
      if (recording_ && active_stream_id_ == stream_id) {
        enabled_ = false;
      }
      commands_.push_back(command_t {.kind = command_kind_e::stop, .stream_id = stream_id});
      wake_.notify_one();
    }

    /** @copydoc Service::snapshot */
    nlohmann::json snapshot() const {
      std::vector<nlohmann::json> recordings;
      std::uint64_t used_bytes {0};
      std::error_code error;
      if (!root_.empty() && std::filesystem::is_directory(root_, error)) {
        for (const auto &entry : std::filesystem::directory_iterator(root_, error)) {
          if (error || !entry.is_regular_file() || entry.path().extension() != ".mp4" || entry.path().filename().string().ends_with(".part.mp4")) {
            continue;
          }
          const auto size {entry.file_size(error)};
          if (error) {
            error.clear();
            continue;
          }
          used_bytes += size;
          const auto id {entry.path().stem().string()};
          nlohmann::json metadata {
            {"id", id},
            {"file_name", entry.path().filename().string()},
            {"size_bytes", size},
          };
          std::ifstream input {root_ / (id + ".json")};
          if (input) {
            try {
              auto persisted = nlohmann::json::parse(input);
              metadata.update(persisted);
              metadata["size_bytes"] = size;
            } catch (const nlohmann::json::exception &) {
            }
          }
          recordings.push_back(std::move(metadata));
        }
      }
      std::ranges::sort(recordings, [](const nlohmann::json &left, const nlohmann::json &right) {
        return left.value("created_at_unix_ms", std::int64_t {}) > right.value("created_at_unix_ms", std::int64_t {});
      });
      std::scoped_lock lock {mutex_};
      return {
        {"schema_version", 1},
        {"enabled", enabled_},
        {"state", recording_ ? "recording" : enabled_          ? "armed" :
                                           finalizing_         ? "finalizing" :
                                           last_error_.empty() ? "idle" :
                                                                 "error"},
        {"active_recording_id", active_id_},
        {"last_error", last_error_},
        {"capacity_mb", capacity_mb_},
        {"capacity_bytes", capacity_mb_ * BYTES_PER_MEGABYTE},
        {"used_bytes", used_bytes},
        {"recordings", nlohmann::json(std::move(recordings))},
      };
    }

    /** @copydoc Service::remove */
    result_t remove(const std::string_view id) {
      if (!valid_id(id)) {
        return {false, "invalid_recording_id", "Recording identifier is invalid."};
      }
      {
        std::scoped_lock lock {mutex_};
        if (active_id_ == id) {
          return {false, "recording_active", "Stop the active recording before deleting it."};
        }
      }
      std::error_code error;
      const auto media {root_ / (std::string {id} + ".mp4")};
      if (!std::filesystem::is_regular_file(media, error)) {
        return {false, "recording_not_found", "Recording was not found."};
      }
      std::filesystem::remove(media, error);
      if (error) {
        return {false, "recording_delete_failed", "Unable to delete recording."};
      }
      std::filesystem::remove(root_ / (std::string {id} + ".json"), error);
      error.clear();
      std::filesystem::remove(root_ / (std::string {id} + ".log"), error);
      return {true, "recording_deleted", "Recording deleted."};
    }

    /** @copydoc Service::resolve */
    std::filesystem::path resolve(const std::string_view id) const {
      if (!valid_id(id)) {
        return {};
      }
      const auto path {root_ / (std::string {id} + ".mp4")};
      std::error_code error;
      return std::filesystem::is_regular_file(path, error) ? path : std::filesystem::path {};
    }

  private:
    /**
     * @brief Operations consumed by the recording worker.
     */
    enum class command_kind_e {
      frame,  ///< Write one encoded frame.
      stop,  ///< Finalize the active container.
      shutdown,  ///< Finalize and stop the worker.
    };

    /**
     * @brief Owned command and frame metadata transferred to the worker.
     */
    struct command_t {
      command_kind_e kind {command_kind_e::frame};  ///< Worker operation.
      std::vector<std::uint8_t> bytes;  ///< Owned encoded payload for frame operations.
      std::uintptr_t stream_id {0};  ///< Process-local source stream identity.
      std::int64_t frame_index {0};  ///< Sender frame index.
      int codec {0};  ///< GameStream codec identifier.
      int width {0};  ///< Encoded width.
      int height {0};  ///< Encoded height.
      int fps {0};  ///< Nominal frame rate.
      bool hdr {false};  ///< HDR state.
      bool idr {false};  ///< Key-frame state.
    };

    /**
     * @brief Load bounded persisted capacity without trusting arbitrary JSON types.
     */
    void load_settings() {
      std::ifstream input {root_ / "settings.json"};
      if (!input) {
        return;
      }
      try {
        const auto settings = nlohmann::json::parse(input);
        const auto capacity {settings.at("capacity_mb").get<std::uint64_t>()};
        if (capacity > 0 && capacity <= MAXIMUM_CAPACITY_MB) {
          capacity_mb_ = capacity;
        }
      } catch (const nlohmann::json::exception &) {
      }
    }

    /**
     * @brief Return a unique sortable identifier for one recording.
     *
     * @return Generated identifier without path separators.
     */
    std::string next_id() {
      const auto now {std::chrono::system_clock::now()};
      const auto seconds {std::chrono::floor<std::chrono::seconds>(now)};
      const auto millisecond {std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count()};
      while (true) {
        const auto candidate {std::format("{:%Y%m%d-%H%M%S}-{:03}-{}", seconds, millisecond, sequence_.fetch_add(1, std::memory_order_relaxed))};
        if (!std::filesystem::exists(root_ / (candidate + ".mp4")) && !std::filesystem::exists(root_ / (candidate + ".part.mp4"))) {
          return candidate;
        }
      }
    }

#if defined(__linux__)
    /**
     * @brief Start a lossless FFmpeg muxer connected through a private pipe.
     *
     * @param frame First independently decodable frame and stream metadata.
     * @return True when the child process and pipe were created.
     */
    bool start_muxer(const command_t &frame) {
      const std::string format {input_format(frame.codec)};
      if (format.empty()) {
        set_error("Recording does not support the active video codec.");
        return false;
      }
      std::error_code error;
      std::filesystem::create_directories(root_, error);
      if (error) {
        set_error("Unable to create the recording directory.");
        return false;
      }
      std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
      prune_before_start();
      const auto new_id {next_id()};
      {
        std::scoped_lock lock {mutex_};
        active_id_ = new_id;
        active_stream_id_ = frame.stream_id;
      }
      partial_path_ = root_ / (active_id_ + ".part.mp4");
      final_path_ = root_ / (active_id_ + ".mp4");
      metadata_ = {
        {"schema_version", 1},
        {"id", active_id_},
        {"created_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
        {"first_frame_index", frame.frame_index},
        {"codec", frame.codec == 0 ? "h264" : frame.codec == 1 ? "hevc" :
                                                                 "av1"},
        {"width", frame.width},
        {"height", frame.height},
        {"fps", frame.fps},
        {"hdr", frame.hdr},
      };
      int descriptors[2] {-1, -1};
      if (::pipe2(descriptors, O_CLOEXEC) != 0) {
        set_error("Unable to create the recording muxer pipe.");
        std::scoped_lock lock {mutex_};
        active_id_.clear();
        active_stream_id_ = 0;
        return false;
      }
      const auto log_path {(root_ / (active_id_ + ".log")).string()};
      const auto partial_path {partial_path_.string()};
      const auto log_fd {::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
      posix_spawn_file_actions_t file_actions;
      ::posix_spawn_file_actions_init(&file_actions);
      ::posix_spawn_file_actions_adddup2(&file_actions, descriptors[0], STDIN_FILENO);
      ::posix_spawn_file_actions_addclose(&file_actions, descriptors[0]);
      ::posix_spawn_file_actions_addclose(&file_actions, descriptors[1]);
      if (log_fd >= 0) {
        ::posix_spawn_file_actions_adddup2(&file_actions, log_fd, STDERR_FILENO);
        ::posix_spawn_file_actions_addclose(&file_actions, log_fd);
      }
      std::array<char *, 21> arguments {
        ffmpeg_executable_.data(),
        const_cast<char *>("-hide_banner"),
        const_cast<char *>("-loglevel"),
        const_cast<char *>("error"),
        const_cast<char *>("-nostdin"),
        const_cast<char *>("-y"),
        const_cast<char *>("-use_wallclock_as_timestamps"),
        const_cast<char *>("1"),
        const_cast<char *>("-f"),
        const_cast<char *>(format.c_str()),
        const_cast<char *>("-i"),
        const_cast<char *>("pipe:0"),
        const_cast<char *>("-map"),
        const_cast<char *>("0:v:0"),
        const_cast<char *>("-c:v"),
        const_cast<char *>("copy"),
        const_cast<char *>("-an"),
        const_cast<char *>("-movflags"),
        const_cast<char *>("+faststart"),
        const_cast<char *>(partial_path.c_str()),
        nullptr,
      };
      pid_t child {-1};
      const auto spawn_result {::posix_spawnp(&child, ffmpeg_executable_.c_str(), &file_actions, nullptr, arguments.data(), environ)};
      ::posix_spawn_file_actions_destroy(&file_actions);
      ::close(descriptors[0]);
      if (log_fd >= 0) {
        ::close(log_fd);
      }
      if (spawn_result != 0) {
        ::close(descriptors[1]);
        set_error("Unable to start the recording muxer.");
        {
          std::scoped_lock lock {mutex_};
          active_id_.clear();
          active_stream_id_ = 0;
        }
        std::filesystem::remove(log_path, error);
        return false;
      }
      pipe_fd_ = descriptors[1];
      child_pid_ = child;
      {
        std::scoped_lock lock {mutex_};
        recording_ = true;
        finalizing_ = false;
      }
      BOOST_LOG(info) << "STREAM_RECORDING_STARTED id=" << active_id_ << " codec=" << metadata_.at("codec") << " path=" << final_path_;
      return true;
    }

    /**
     * @brief Write all encoded bytes to the muxer pipe.
     *
     * @param bytes Encoded frame bytes.
     * @return True when all bytes were accepted.
     */
    bool write_frame(const std::span<const std::uint8_t> bytes) {
      std::size_t offset {0};
      while (offset < bytes.size()) {
        const auto written {::write(pipe_fd_, bytes.data() + offset, bytes.size() - offset)};
        if (written <= 0) {
          return false;
        }
        offset += static_cast<std::size_t>(written);
      }
      return true;
    }

    /**
     * @brief Close the muxer, publish a complete file, and apply retention.
     */
    void finalize_muxer() {
      if (pipe_fd_ < 0) {
        return;
      }
      {
        std::scoped_lock lock {mutex_};
        recording_ = false;
        finalizing_ = true;
      }
      ::close(pipe_fd_);
      pipe_fd_ = -1;
      int status {};
      ::waitpid(child_pid_, &status, 0);
      child_pid_ = -1;
      std::error_code error;
      const bool succeeded {WIFEXITED(status) && WEXITSTATUS(status) == 0 && std::filesystem::is_regular_file(partial_path_, error) && std::filesystem::file_size(partial_path_, error) > 0};
      if (succeeded) {
        std::filesystem::rename(partial_path_, final_path_, error);
      }
      if (!succeeded || error) {
        std::filesystem::remove(partial_path_, error);
        set_error("FFmpeg could not finalize the sender recording.");
      } else {
        metadata_["ended_at_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        metadata_["size_bytes"] = std::filesystem::file_size(final_path_, error);
        write_private_json(root_ / (active_id_ + ".json"), metadata_);
        BOOST_LOG(info) << "STREAM_RECORDING_FINALIZED id=" << active_id_ << " bytes=" << metadata_.at("size_bytes");
      }
      {
        std::scoped_lock lock {mutex_};
        finalizing_ = false;
        active_id_.clear();
        active_stream_id_ = 0;
      }
      enforce_capacity();
    }
#else
    bool start_muxer(const command_t &) {
      set_error("Sender recording is supported only on Linux.");
      return false;
    }

    bool write_frame(std::span<const std::uint8_t>) {
      return false;
    }

    void finalize_muxer() {
    }
#endif

    /**
     * @brief Store a safe worker error and disarm recording.
     *
     * @param message Safe error message.
     */
    void set_error(std::string message) {
      std::scoped_lock lock {mutex_};
      enabled_ = false;
      recording_ = false;
      finalizing_ = false;
      last_error_ = std::move(message);
    }

    /**
     * @brief Process frames and control operations outside the sender thread.
     */
    void run() {
#if defined(__linux__)
      sigset_t blocked_signals;
      ::sigemptyset(&blocked_signals);
      ::sigaddset(&blocked_signals, SIGPIPE);
      ::pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);
#endif
      bool running {true};
      while (running) {
        command_t command;
        {
          std::unique_lock lock {mutex_};
          wake_.wait(lock, [this]() {
            return !commands_.empty();
          });
          command = std::move(commands_.front());
          commands_.pop_front();
          queued_bytes_ -= command.bytes.size();
        }
        if (command.kind == command_kind_e::shutdown) {
          finalize_muxer();
          running = false;
          continue;
        }
        if (command.kind == command_kind_e::stop) {
          bool should_stop {};
          {
            std::scoped_lock lock {mutex_};
            should_stop = command.stream_id == 0 || (recording_ && active_stream_id_ == command.stream_id);
            if (should_stop) {
              enabled_ = false;
            }
          }
          if (should_stop) {
            finalize_muxer();
          }
          continue;
        }
        bool active {};
        {
          std::scoped_lock lock {mutex_};
          active = recording_;
          if (active && active_stream_id_ != command.stream_id) {
            continue;
          }
        }
        if (!active) {
          bool enabled {};
          {
            std::scoped_lock lock {mutex_};
            enabled = enabled_;
          }
          if (!enabled || !command.idr || !start_muxer(command)) {
            continue;
          }
        }
        if (!write_frame(command.bytes)) {
          set_error("Recording stopped because the FFmpeg pipe closed unexpectedly.");
          finalize_muxer();
        }
      }
    }

    /**
     * @brief Remove oldest completed recordings until total usage fits capacity.
     */
    void enforce_capacity() const {
      struct candidate_t {
        std::filesystem::path path;  ///< Completed media file.
        std::filesystem::file_time_type modified;  ///< Stable oldest-first retention key.
        std::uint64_t size;  ///< Media size in bytes.
      };

      std::vector<candidate_t> candidates;
      std::uint64_t used {};
      std::error_code error;
      if (root_.empty() || !std::filesystem::is_directory(root_, error)) {
        return;
      }
      for (const auto &entry : std::filesystem::directory_iterator(root_, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != ".mp4" || entry.path().filename().string().ends_with(".part.mp4")) {
          continue;
        }
        const auto size {entry.file_size(error)};
        const auto modified {entry.last_write_time(error)};
        if (error) {
          error.clear();
          continue;
        }
        used += size;
        candidates.push_back({entry.path(), modified, size});
      }
      std::ranges::sort(candidates, {}, &candidate_t::modified);
      std::uint64_t capacity {};
      {
        std::scoped_lock lock {mutex_};
        capacity = capacity_mb_ * BYTES_PER_MEGABYTE;
      }
      for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (used <= capacity || candidates.size() - index <= 1U) {
          break;
        }
        const auto &candidate {candidates[index]};
        std::filesystem::remove(candidate.path, error);
        if (error) {
          error.clear();
          continue;
        }
        used -= candidate.size;
        const auto id {candidate.path.stem().string()};
        std::filesystem::remove(root_ / (id + ".json"), error);
        error.clear();
        std::filesystem::remove(root_ / (id + ".log"), error);
        BOOST_LOG(info) << "STREAM_RECORDING_PRUNED id=" << id << " reason=capacity";
      }
    }

    /**
     * @brief Free capacity before opening a new recording, including an oversized sole predecessor.
     */
    void prune_before_start() const {
      struct candidate_t {
        std::filesystem::path path;  ///< Completed media file.
        std::filesystem::file_time_type modified;  ///< Stable oldest-first retention key.
        std::uint64_t size;  ///< Media size in bytes.
      };

      std::vector<candidate_t> candidates;
      std::uint64_t used {};
      std::error_code error;
      if (root_.empty() || !std::filesystem::is_directory(root_, error)) {
        return;
      }
      for (const auto &entry : std::filesystem::directory_iterator(root_, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != ".mp4" || entry.path().filename().string().ends_with(".part.mp4")) {
          continue;
        }
        const auto size {entry.file_size(error)};
        const auto modified {entry.last_write_time(error)};
        if (error) {
          error.clear();
          continue;
        }
        used += size;
        candidates.push_back({entry.path(), modified, size});
      }
      std::ranges::sort(candidates, {}, &candidate_t::modified);
      std::uint64_t capacity {};
      {
        std::scoped_lock lock {mutex_};
        capacity = capacity_mb_ * BYTES_PER_MEGABYTE;
      }
      for (const auto &candidate : candidates) {
        if (used < capacity) {
          break;
        }
        std::filesystem::remove(candidate.path, error);
        if (error) {
          error.clear();
          continue;
        }
        used -= candidate.size;
        const auto id {candidate.path.stem().string()};
        std::filesystem::remove(root_ / (id + ".json"), error);
        error.clear();
        std::filesystem::remove(root_ / (id + ".log"), error);
        BOOST_LOG(info) << "STREAM_RECORDING_PRUNED id=" << id << " reason=next_recording";
      }
    }

    std::filesystem::path root_;  ///< Owner-private recording directory.
    std::string ffmpeg_executable_;  ///< Lossless muxer executable.
    mutable std::mutex mutex_;  ///< Protects commands and public state.
    std::condition_variable wake_;  ///< Wakes the isolated writer.
    std::deque<command_t> commands_;  ///< Ordered frame and lifecycle operations.
    std::size_t queued_bytes_ {0};  ///< Encoded bytes waiting for the writer.
    bool enabled_ {false};  ///< Whether Web requested recording.
    bool recording_ {false};  ///< Whether an FFmpeg muxer is active.
    bool finalizing_ {false};  ///< Whether a completed container is being published.
    std::string active_id_;  ///< Identifier of the in-progress recording.
    std::uintptr_t active_stream_id_ {0};  ///< Stream selected by the first accepted IDR.
    std::string last_error_;  ///< Safe most recent error.
    std::uint64_t capacity_mb_ {DEFAULT_CAPACITY_MB};  ///< Retained media limit.
    std::atomic_uint64_t sequence_ {0};  ///< Collision-resistant suffix within one process.
    std::thread worker_;  ///< Isolated pipe writer and finalizer.
    std::filesystem::path partial_path_;  ///< Active incomplete container.
    std::filesystem::path final_path_;  ///< Active final destination.
    nlohmann::json metadata_;  ///< Metadata for the active recording.
#if defined(__linux__)
    int pipe_fd_ {-1};  ///< Parent write side of the FFmpeg pipe.
    pid_t child_pid_ {-1};  ///< Active FFmpeg process identifier.
#endif
  };

  Service::Service(std::filesystem::path root, std::string ffmpeg_executable):
      impl_ {std::make_unique<Impl>(std::move(root), std::move(ffmpeg_executable))} {
  }

  Service::~Service() = default;

  result_t Service::set_enabled(const bool enabled) {
    return impl_->set_enabled(enabled);
  }

  result_t Service::set_capacity_megabytes(const std::uint64_t capacity_mb) {
    return impl_->set_capacity_megabytes(capacity_mb);
  }

  void Service::submit(const frame_t &frame) {
    impl_->submit(frame);
  }

  void Service::stream_ended(const std::uintptr_t stream_id) {
    impl_->stream_ended(stream_id);
  }

  nlohmann::json Service::snapshot() const {
    return impl_->snapshot();
  }

  result_t Service::remove(const std::string_view id) {
    return impl_->remove(id);
  }

  std::filesystem::path Service::resolve(const std::string_view id) const {
    return impl_->resolve(id);
  }

  Service &service() {
    static Service instance;
    return instance;
  }
}  // namespace stream_recording
