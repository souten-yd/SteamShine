/**
 * @file src/steamshine_terminal.cpp
 * @brief OS-managed tmux shells with PTY attachments for the SteamShine web Terminal.
 */
#include "steamshine_terminal.h"

#include "crypto.h"
#include "logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
  #include <cerrno>
  #include <csignal>
  #include <fcntl.h>
  #include <pty.h>
  #include <pwd.h>
  #include <sys/ioctl.h>
  #include <sys/syscall.h>
  #include <sys/wait.h>
  #include <termios.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace steamshine_terminal {

#if defined(__linux__)
  void close_inherited_file_descriptors() {
  #ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1), ~0U, 0U) == 0) {
      return;
    }
  #endif
    const long open_max {::sysconf(_SC_OPEN_MAX)};
    const int descriptor_limit {open_max > STDERR_FILENO ? static_cast<int>(open_max) : 1024};
    for (int descriptor = STDERR_FILENO + 1; descriptor < descriptor_limit; ++descriptor) {
      ::close(descriptor);
    }
  }

  bool prepare_tmux_attachment_terminal(const int terminal_fd) {
    struct termios attributes {};
    if (::tcgetattr(terminal_fd, &attributes) != 0) {
      return false;
    }
    ::cfmakeraw(&attributes);
    return ::tcsetattr(terminal_fd, TCSANOW, &attributes) == 0;
  }
#endif

  namespace {

    constexpr std::size_t MAX_BACKLOG_BYTES = 4 * 1024 * 1024;
    constexpr std::string_view TMUX_PREFIX = "steamshine-terminal-";

    /**
     * @brief Internal state for one persistent shell or fallback PTY.
     */
    struct session_t {
      std::string id;  ///< Stable API identifier.
      std::string name;  ///< User-facing name.
      std::string explicit_end_token;  ///< Process-local nonce authorizing explicit deletion.
      std::uint64_t created_at;  ///< Unix creation time in seconds.
      bool persistent {false};  ///< Whether tmux owns the shell outside this process.
      std::string tmux_name;  ///< OS-managed tmux name when persistent.
      std::mutex pty_mutex;  ///< Protects the current attachment PTY and child state.
      int master_fd {-1};  ///< Master side of the current pseudo terminal.
      bool stopping {false};  ///< Whether explicit deletion or service shutdown has begun.
      bool copy_mode_active {false};  ///< Whether Web touch scrolling placed tmux in copy mode.
#if defined(__linux__)
      pid_t pid {-1};  ///< Fallback shell or tmux attachment process identifier.
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
     * @brief Result of a synchronously executed child command.
     */
    struct command_result_t {
      int exit_code;  ///< Normalized exit status, or -1 when launch failed.
      std::string output;  ///< Captured standard output.
    };

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

    /**
     * @brief Test whether an executable can be resolved through PATH.
     *
     * @param executable Bare executable name.
     * @return True when one executable regular file is available.
     */
    bool executable_available(const std::string_view executable) {
      const char *path_value {std::getenv("PATH")};
      if (!path_value || !*path_value) {
        return false;
      }
      std::stringstream paths {path_value};
      std::string directory;
      while (std::getline(paths, directory, ':')) {
        const auto candidate {std::filesystem::path {directory.empty() ? "." : directory} / executable};
        if (::access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Execute an argv vector and optionally capture standard output.
     *
     * No shell is involved, so session names and paths are never interpreted
     * as command syntax.
     *
     * @param arguments Executable followed by its arguments.
     * @param capture_output Whether to capture standard output.
     * @return Child exit status and captured output.
     */
    command_result_t run_command(const std::vector<std::string> &arguments, const bool capture_output = false) {
      if (arguments.empty()) {
        return {-1, {}};
      }
      int output_pipe[2] {-1, -1};
      if (capture_output && ::pipe2(output_pipe, O_CLOEXEC) != 0) {
        return {-1, {}};
      }
      const pid_t pid {::fork()};
      if (pid < 0) {
        if (output_pipe[0] >= 0) {
          ::close(output_pipe[0]);
          ::close(output_pipe[1]);
        }
        return {-1, {}};
      }
      if (pid == 0) {
        if (capture_output) {
          ::close(output_pipe[0]);
          ::dup2(output_pipe[1], STDOUT_FILENO);
          ::close(output_pipe[1]);
        }
        const int null_fd {::open("/dev/null", O_WRONLY | O_CLOEXEC)};
        if (null_fd >= 0) {
          ::dup2(null_fd, STDERR_FILENO);
          ::close(null_fd);
        }
        close_inherited_file_descriptors();
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) {
          argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
      }
      if (capture_output) {
        ::close(output_pipe[1]);
      }
      std::string output;
      if (capture_output) {
        char buffer[4096];
        while (true) {
          const auto count {::read(output_pipe[0], buffer, sizeof(buffer))};
          if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
          } else if (count < 0 && errno == EINTR) {
            continue;
          } else {
            break;
          }
        }
        ::close(output_pipe[0]);
      }
      int status {};
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      if (WIFEXITED(status)) {
        return {WEXITSTATUS(status), std::move(output)};
      }
      return {-1, std::move(output)};
    }

    /**
     * @brief Test whether the persistent tmux backend is available.
     *
     * @return True when tmux can be launched through PATH.
     */
    bool tmux_available() {
      return executable_available("tmux");
    }

    /**
     * @brief Ask tmux whether one managed session still exists.
     *
     * @param tmux_name Exact managed tmux session name.
     * @return True when tmux reports the session.
     */
    bool tmux_session_running(const std::string &tmux_name) {
      return tmux_available() && run_command({"tmux", "has-session", "-t", tmux_name}).exit_code == 0;
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
      // tmux repaints persistent sessions on demand. Its raw output contains
      // absolute cursor movement for whichever geometry was active, so
      // replaying it into a browser with another size corrupts the screen.
      if (!session->persistent) {
        session->backlog.append(chunk);
        if (session->backlog.size() > MAX_BACKLOG_BYTES) {
          session->backlog.erase(0, session->backlog.size() - MAX_BACKLOG_BYTES);
        }
      }
      for (const auto &[id, callback] : session->subscribers) {
        (void) id;
        callback(chunk);
      }
    }

#if defined(__linux__)
    /**
     * @brief Discover managed tmux sessions created by any SteamShine process.
     */
    void synchronize_tmux_sessions() {
      if (!tmux_available()) {
        return;
      }
      const auto result {run_command({"tmux", "list-sessions", "-F", "#{session_name}\t#{session_created}"}, true)};
      if (result.exit_code != 0) {
        return;
      }
      std::stringstream lines {result.output};
      std::string line;
      while (std::getline(lines, line)) {
        const auto tab {line.find('\t')};
        const std::string name {line.substr(0, tab)};
        if (!name.starts_with(TMUX_PREFIX)) {
          continue;
        }
        const std::string id {name.substr(TMUX_PREFIX.size())};
        if (id.empty()) {
          continue;
        }
        std::uint64_t created_at {};
        if (tab != std::string::npos) {
          try {
            created_at = std::stoull(line.substr(tab + 1));
          } catch (const std::exception &) {
          }
        }
        std::lock_guard lock {sessions_mutex};
        if (!sessions.contains(id)) {
          auto session {std::make_shared<session_t>()};
          session->id = id;
          session->name = name;
          session->explicit_end_token = crypto::rand_alphabet(32);
          session->created_at = created_at;
          session->persistent = true;
          session->tmux_name = name;
          sessions.emplace(id, std::move(session));
          BOOST_LOG(info) << "TERMINAL_SESSION_RECOVERED id=" << id << " backend=tmux";
        }
      }
    }

    /**
     * @brief Read one PTY until EOF and reap its attachment process.
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
     * @brief Start a PTY bridge to an OS-managed tmux session.
     *
     * @param session Persistent session requiring an attachment.
     * @return True when an existing or newly started bridge is ready.
     */
    bool ensure_tmux_attachment(const std::shared_ptr<session_t> &session) {
      std::lock_guard lock {session->pty_mutex};
      if (!session->persistent || session->stopping) {
        return false;
      }
      if (session->master_fd >= 0 && session->pid > 0) {
        return true;
      }
      if (!tmux_session_running(session->tmux_name)) {
        return false;
      }
      // tmux must own wheel handling for persistent sessions. Without mouse
      // mode, xterm translates wheel motion in the alternate screen into
      // cursor-up/down input, which recalls shell command history.
      if (run_command({"tmux", "set-option", "-t", session->tmux_name, "mouse", "on"}).exit_code != 0) {
        BOOST_LOG(warning) << "steamshine_terminal: failed to enable tmux mouse scrolling"sv;
        return false;
      }
      struct winsize window_size {};
      window_size.ws_row = 24;
      window_size.ws_col = 80;
      int master_fd {};
      const pid_t pid {::forkpty(&master_fd, nullptr, nullptr, &window_size)};
      if (pid < 0) {
        return false;
      }
      if (pid == 0) {
        if (!prepare_tmux_attachment_terminal(STDIN_FILENO)) {
          _exit(126);
        }
        close_inherited_file_descriptors();
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::execlp("tmux", "tmux", "attach-session", "-t", session->tmux_name.c_str(), static_cast<char *>(nullptr));
        _exit(127);
      }
      session->master_fd = master_fd;
      session->pid = pid;
      session->reader = std::jthread {reader_loop, session, master_fd};
      return true;
    }

    /**
     * @brief Terminate the current PTY process and join its reader.
     *
     * For persistent sessions this only stops the tmux attachment. For a
     * fallback session it stops the login shell itself.
     *
     * @param session Session whose process should be detached.
     */
    void stop_attachment(const std::shared_ptr<session_t> &session) {
      pid_t pid_to_kill {-1};
      std::jthread reader_to_join;
      {
        std::lock_guard lock {session->pty_mutex};
        session->stopping = true;
        pid_to_kill = session->pid;
        reader_to_join = std::move(session->reader);
      }
      if (pid_to_kill > 0) {
        ::kill(-pid_to_kill, SIGKILL);
        ::kill(pid_to_kill, SIGKILL);
      }
      if (reader_to_join.joinable()) {
        reader_to_join.join();
      }
      std::lock_guard subscriber_lock {session->subscriber_mutex};
      session->subscribers.clear();
    }

    /**
     * @brief Start an in-process PTY fallback when tmux is unavailable.
     *
     * @param session New session state.
     * @param shell_home Working directory for the login shell.
     * @return True when the child and reader started.
     */
    bool start_fallback_shell(const std::shared_ptr<session_t> &session, const std::string &shell_home) {
      struct winsize window_size {};
      window_size.ws_row = 24;
      window_size.ws_col = 80;
      int master_fd {};
      const pid_t pid {::forkpty(&master_fd, nullptr, nullptr, &window_size)};
      if (pid < 0) {
        return false;
      }
      if (pid == 0) {
        if (::chdir(shell_home.c_str()) != 0) {
          _exit(126);
        }
        close_inherited_file_descriptors();
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
      session->reader = std::jthread {reader_loop, session, master_fd};
      return true;
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
    const auto created_at {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::system_clock::now().time_since_epoch()
    )
                                                        .count())};
    auto session {std::make_shared<session_t>()};
    session->id = std::format("{}-{}", created_at, numeric_id);
    session->name = std::format("Shell {}", numeric_id);
    session->explicit_end_token = crypto::rand_alphabet(32);
    session->created_at = created_at;

    if (tmux_available()) {
      session->persistent = true;
      session->tmux_name = std::string {TMUX_PREFIX} + session->id;
      session->name = session->tmux_name;
      std::vector<std::string> tmux_command {"tmux", "new-session", "-d", "-s", session->tmux_name, "-c", shell_home};
      command_result_t result {-1, {}};
      if (executable_available("systemd-run")) {
        std::vector<std::string> scoped_command {"systemd-run", "--user", "--quiet", "--collect", "--scope", "--"};
        scoped_command.insert(scoped_command.end(), tmux_command.begin(), tmux_command.end());
        result = run_command(scoped_command);
      }
      if (result.exit_code != 0) {
        result = run_command(tmux_command);
      }
      if (result.exit_code != 0 || !tmux_session_running(session->tmux_name)) {
        BOOST_LOG(warning) << "steamshine_terminal: failed to create an OS-managed tmux session"sv;
        return {};
      }
      (void) run_command({"tmux", "set-option", "-t", session->tmux_name, "status", "off"});
      (void) run_command({"tmux", "set-option", "-t", session->tmux_name, "history-limit", "100000"});
      (void) run_command({"tmux", "set-option", "-t", session->tmux_name, "mouse", "on"});
      BOOST_LOG(info) << "TERMINAL_SESSION_CREATED id=" << session->id << " backend=tmux os_managed=true";
    } else if (!start_fallback_shell(session, shell_home)) {
      BOOST_LOG(warning) << "steamshine_terminal: forkpty failed"sv;
      return {};
    } else {
      BOOST_LOG(warning) << "TERMINAL_SESSION_CREATED id=" << session->id << " backend=in_process os_managed=false reason=tmux_unavailable";
    }

    {
      std::lock_guard lock {sessions_mutex};
      sessions.emplace(session->id, session);
    }
    return session->id;
#else
    return {};
#endif
  }

  std::vector<session_snapshot_t> list() {
#if defined(__linux__)
    synchronize_tmux_sessions();
#endif
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
      const bool is_running {session->persistent ? tmux_session_running(session->tmux_name) : session->pid > 0 && !session->stopping};
      result.push_back({session->id, session->name, session->explicit_end_token, session->created_at, is_running, session->persistent});
#else
      result.push_back({session->id, session->name, session->explicit_end_token, session->created_at, false, false});
#endif
    }
    std::ranges::sort(result, [](const auto &left, const auto &right) {
      return std::tie(left.created_at, left.id) < std::tie(right.created_at, right.id);
    });
    return result;
  }

  bool stop(const std::string_view session_id, const std::string_view explicit_end_token) {
#if defined(__linux__)
    synchronize_tmux_sessions();
#endif
    std::shared_ptr<session_t> session;
    {
      std::lock_guard lock {sessions_mutex};
      const auto found {sessions.find(std::string {session_id})};
      if (found == sessions.end() || explicit_end_token.empty() || found->second->explicit_end_token != explicit_end_token) {
        return false;
      }
      session = found->second;
    }
#if defined(__linux__)
    if (session->persistent) {
      (void) run_command({"tmux", "kill-session", "-t", session->tmux_name});
    }
    stop_attachment(session);
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
#if defined(__linux__)
    synchronize_tmux_sessions();
#endif
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
      if (session->persistent) {
        (void) run_command({"tmux", "kill-session", "-t", session->tmux_name});
      }
      stop_attachment(session);
    }
#endif
    std::lock_guard lock {sessions_mutex};
    sessions.clear();
  }

  void detach_all() {
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
      BOOST_LOG(info) << "TERMINAL_SESSION_DETACHED id=" << session->id
                      << " persistent=" << (session->persistent ? "true" : "false");
      stop_attachment(session);
    }
#endif
    std::lock_guard lock {sessions_mutex};
    sessions.clear();
  }

  bool running(const std::string_view session_id) {
#if defined(__linux__)
    synchronize_tmux_sessions();
#endif
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
#if defined(__linux__)
    return session->persistent ? tmux_session_running(session->tmux_name) : session->pid > 0 && !session->stopping;
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
    if (session->persistent && session->copy_mode_active) {
      (void) run_command({"tmux", "send-keys", "-t", session->tmux_name, "-X", "cancel"});
      session->copy_mode_active = false;
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

  bool scroll(const std::string_view session_id, const int lines) {
#if defined(__linux__)
    if (lines == 0) {
      return false;
    }
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
    if (!session->persistent || session->master_fd < 0 || session->stopping) {
      return false;
    }

    const auto repeat_count {std::to_string(std::clamp(std::abs(lines), 1, 200))};
    const std::string command {lines < 0 ? "scroll-up" : "scroll-down"};
    // `-e` leaves copy mode when scrolling reaches the live bottom, so a
    // downward swipe returns to live output without requiring a keypress.
    const auto enter_copy_mode = [&session] {
      session->copy_mode_active = run_command({"tmux", "copy-mode", "-e", "-t", session->tmux_name}).exit_code == 0;
      return session->copy_mode_active;
    };
    const auto send_scroll = [&] {
      return run_command({"tmux", "send-keys", "-t", session->tmux_name, "-X", "-N", repeat_count, command}).exit_code == 0;
    };
    if (!session->copy_mode_active && (lines > 0 || !enter_copy_mode())) {
      return false;
    }
    if (send_scroll()) {
      return true;
    }
    // tmux may already have left copy mode at the bottom; re-enter it for an
    // upward swipe instead of dropping the first gesture frame.
    session->copy_mode_active = false;
    return lines < 0 && enter_copy_mode() && send_scroll();
#else
    (void) session_id;
    (void) lines;
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

  bool redraw(const std::string_view session_id) {
#if defined(__linux__)
    const auto session {find_session(session_id)};
    if (!session) {
      return false;
    }
    std::lock_guard lock {session->pty_mutex};
    if (!session->persistent || session->master_fd < 0 || session->stopping) {
      return false;
    }
    std::array<char, 128> client_tty {};
    if (::ptsname_r(session->master_fd, client_tty.data(), client_tty.size()) != 0) {
      return false;
    }
    return run_command({"tmux", "refresh-client", "-t", client_tty.data()}).exit_code == 0;
#else
    (void) session_id;
    return false;
#endif
  }

  std::uint64_t subscribe(const std::string_view session_id, output_callback_t callback) {
    const auto session {find_session(session_id)};
    if (!session) {
      return 0;
    }
#if defined(__linux__)
    if (session->persistent && !ensure_tmux_attachment(session)) {
      return 0;
    }
#endif
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
