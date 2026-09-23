/**
 * @file src/steamshine_terminal.cpp
 * @brief Multi-session PTY-backed shells for the SteamShine web Terminal.
 */
#include "steamshine_terminal.h"

#include "logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#if defined(__linux__)
  #include <cerrno>
  #include <csignal>
  #include <pty.h>
  #include <pwd.h>
  #include <sys/ioctl.h>
  #include <sys/wait.h>
  #include <termios.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace steamshine_terminal {

  namespace {

    constexpr std::size_t MAX_BACKLOG_BYTES = 4 * 1024 * 1024;

    /**
     * @brief Internal state for one independent PTY shell.
     */
    struct session_t {
      std::string id;  ///< Stable API identifier.
      std::string name;  ///< User-facing name.
      std::uint64_t created_at;  ///< Unix creation time in seconds.
      std::mutex pty_mutex;  ///< Protects PTY descriptors and child state.
      int master_fd {-1};  ///< Master side of the pseudo terminal.
      bool stopping {false};  ///< Whether explicit deletion or service shutdown has begun.
#if defined(__linux__)
      pid_t pid {-1};  ///< Login-shell process identifier.
#endif
      std::jthread reader;  ///< PTY output reader.
      std::mutex subscriber_mutex;  ///< Serializes backlog and callbacks.
      std::unordered_map<std::uint64_t, output_callback_t> subscribers;  ///< Live output subscribers.
      std::uint64_t next_subscriber_id {1};  ///< Next non-zero subscription identifier.
      std::string backlog;  ///< Bounded recent PTY output for reconnects.
    };

    std::mutex sessions_mutex;
    std::unordered_map<std::string, std::shared_ptr<session_t>> sessions;
    std::atomic_uint64_t next_session_id {1};

#if defined(__linux__)
    /**
     * @brief Resolve the service user's home directory for a new shell.
     *
     * @return HOME when set, otherwise the current user's passwd home, or an
     * empty string when neither source provides a usable directory.
     */
    std::string home_directory() {
      if (const char *home {std::getenv("HOME")}; home && *home) {
        return home;
      }
      if (const auto *entry {::getpwuid(::getuid())}; entry && entry->pw_dir && *entry->pw_dir) {
        return entry->pw_dir;
      }
      return {};
    }
#endif

    /**
     * @brief Find a retained session by identifier.
     *
     * @param session_id Identifier to find.
     * @return Shared session state, or null when absent.
     */
    std::shared_ptr<session_t> find_session(const std::string_view session_id) {
      std::lock_guard lock {sessions_mutex};
      const auto found {sessions.find(std::string {session_id})};
      return found == sessions.end() ? nullptr : found->second;
    }

    /**
     * @brief Append output to replay history and synchronously fan it out.
     *
     * @param session Destination session.
     * @param chunk PTY bytes to retain and publish.
     */
    void broadcast(const std::shared_ptr<session_t> &session, const std::string_view chunk) {
      std::lock_guard lock {session->subscriber_mutex};
      session->backlog.append(chunk);
      if (session->backlog.size() > MAX_BACKLOG_BYTES) {
        session->backlog.erase(0, session->backlog.size() - MAX_BACKLOG_BYTES);
      }
      for (const auto &[id, callback] : session->subscribers) {
        (void) id;
        callback(chunk);
      }
    }

#if defined(__linux__)
    /**
     * @brief Read one PTY until EOF and reap its child process.
     *
     * @param session Session whose PTY should be drained.
     * @param fd Stable descriptor value captured when the reader starts.
     */
    void reader_loop(const std::shared_ptr<session_t> &session, const int fd) {
      char buffer[8192];
      while (true) {
        const auto count {::read(fd, buffer, sizeof(buffer))};
        if (count > 0) {
          broadcast(session, std::string_view {buffer, static_cast<std::size_t>(count)});
          continue;
        }
        if (count < 0 && errno == EINTR) {
          continue;
        }
        break;
      }

      std::lock_guard lock {session->pty_mutex};
      if (session->master_fd == fd) {
        ::close(session->master_fd);
        session->master_fd = -1;
        if (session->pid > 0) {
          int status {};
          while (::waitpid(session->pid, &status, 0) < 0 && errno == EINTR) {
          }
          session->pid = -1;
        }
      }
    }

    /**
     * @brief Terminate one retained session and join its reader.
     *
     * @param session Session to stop.
     */
    void stop_session(const std::shared_ptr<session_t> &session) {
      pid_t pid_to_kill {-1};
      std::jthread reader_to_join;
      {
        std::lock_guard lock {session->pty_mutex};
        session->stopping = true;
        pid_to_kill = session->pid;
        reader_to_join = std::move(session->reader);
      }
      if (pid_to_kill > 0) {
        // forkpty() creates a session leader. Kill the process group as well as
        // the shell so foreground children do not outlive a deleted web tab.
        ::kill(-pid_to_kill, SIGKILL);
        ::kill(pid_to_kill, SIGKILL);
      }
      if (reader_to_join.joinable()) {
        reader_to_join.join();
      }
      std::lock_guard subscriber_lock {session->subscriber_mutex};
      session->subscribers.clear();
    }
#endif

  }  // namespace

  std::string create() {
#if defined(__linux__)
    const auto shell_home {home_directory()};
    if (shell_home.empty()) {
      BOOST_LOG(warning) << "steamshine_terminal: could not resolve the service user's home directory"sv;
      return {};
    }
    const auto numeric_id {next_session_id.fetch_add(1)};
    auto session {std::make_shared<session_t>()};
    session->id = std::format("terminal-{}", numeric_id);
    session->name = std::format("Shell {}", numeric_id);
    session->created_at = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                       std::chrono::system_clock::now().time_since_epoch()
    )
                                                       .count());

    struct winsize window_size {};
    window_size.ws_row = 24;
    window_size.ws_col = 80;
    int master_fd {};
    const pid_t pid {::forkpty(&master_fd, nullptr, nullptr, &window_size)};
    if (pid < 0) {
      BOOST_LOG(warning) << "steamshine_terminal: forkpty failed"sv;
      return {};
    }
    if (pid == 0) {
      if (::chdir(shell_home.c_str()) != 0) {
        _exit(126);
      }
      ::setenv("TERM", "xterm-256color", 1);
      ::setenv("COLORTERM", "truecolor", 1);
      const char *shell {std::getenv("SHELL")};
      if (!shell || !*shell) {
        shell = "/bin/bash";
      }
      ::execl(shell, shell, "-l", static_cast<char *>(nullptr));
      _exit(127);
    }

    session->master_fd = master_fd;
    session->pid = pid;
    {
      std::lock_guard lock {sessions_mutex};
      sessions.emplace(session->id, session);
    }
    session->reader = std::jthread {reader_loop, session, master_fd};
    return session->id;
#else
    return {};
#endif
  }

  std::vector<session_snapshot_t> list() {
    std::vector<std::shared_ptr<session_t>> retained;
    {
      std::lock_guard lock {sessions_mutex};
      retained.reserve(sessions.size());
      for (const auto &[id, session] : sessions) {
        (void) id;
        retained.push_back(session);
      }
    }

    std::vector<session_snapshot_t> result;
    result.reserve(retained.size());
    for (const auto &session : retained) {
      std::lock_guard lock {session->pty_mutex};
#if defined(__linux__)
      result.push_back({session->id, session->name, session->created_at, session->pid > 0});
#else
      result.push_back({session->id, session->name, session->created_at, false});
#endif
    }
    std::ranges::sort(result, [](const auto &left, const auto &right) {
      return std::tie(left.created_at, left.id) < std::tie(right.created_at, right.id);
    });
    return result;
  }

  bool stop(const std::string_view session_id) {
    std::shared_ptr<session_t> session;
    {
      std::lock_guard lock {sessions_mutex};
      const auto found {sessions.find(std::string {session_id})};
      if (found == sessions.end()) {
        return false;
      }
      session = found->second;
    }
#if defined(__linux__)
    stop_session(session);
#endif
    {
      std::lock_guard lock {sessions_mutex};
      const auto found {sessions.find(std::string {session_id})};
      if (found != sessions.end() && found->second == session) {
        sessions.erase(found);
      }
    }
    return true;
  }

  void stop_all() {
    std::vector<std::shared_ptr<session_t>> retained;
    {
      std::lock_guard lock {sessions_mutex};
      retained.reserve(sessions.size());
      for (const auto &[id, session] : sessions) {
        (void) id;
        retained.push_back(session);
      }
    }
#if defined(__linux__)
    for (const auto &session : retained) {
      stop_session(session);
    }
#endif
    std::lock_guard lock {sessions_mutex};
    sessions.clear();
  }

  bool running(const std::string_view session_id) {
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
#if defined(__linux__)
    return session->pid > 0 && !session->stopping;
#else
    return false;
#endif
  }

  bool write_input(const std::string_view session_id, const std::string_view data) {
#if defined(__linux__)
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
    if (session->master_fd < 0 || session->stopping) {
      return false;
    }
    std::size_t offset {};
    while (offset < data.size()) {
      const auto written {::write(session->master_fd, data.data() + offset, data.size() - offset)};
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
      } else if (written < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
#else
    (void) session_id;
    (void) data;
    return false;
#endif
  }

  bool resize(const std::string_view session_id, const unsigned short cols, const unsigned short rows) {
#if defined(__linux__)
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
    if (session->master_fd < 0 || session->stopping) {
      return false;
    }
    struct winsize window_size {};
    window_size.ws_col = cols;
    window_size.ws_row = rows;
    return ::ioctl(session->master_fd, TIOCSWINSZ, &window_size) == 0;
#else
    (void) session_id;
    (void) cols;
    (void) rows;
    return false;
#endif
  }

  std::uint64_t subscribe(const std::string_view session_id, output_callback_t callback) {
    const auto session {find_session(session_id)};
    if (!session) {
      return 0;
    }
    std::lock_guard lock {session->subscriber_mutex};
    const std::uint64_t id {session->next_subscriber_id++};
    if (!session->backlog.empty()) {
      callback(session->backlog);
    }
    session->subscribers.emplace(id, std::move(callback));
    return id;
  }

  void unsubscribe(const std::string_view session_id, const std::uint64_t subscription_id) {
    const auto session {find_session(session_id)};
    if (!session) {
      return;
    }
    std::lock_guard lock {session->subscriber_mutex};
    session->subscribers.erase(subscription_id);
  }

}  // namespace steamshine_terminal
