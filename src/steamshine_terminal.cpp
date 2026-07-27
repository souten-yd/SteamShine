/**
 * @file src/steamshine_terminal.cpp
 * @brief Single-session PTY-backed shell for the SteamShine web Terminal.
 */
#include "steamshine_terminal.h"

#include "logging.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
  #include <csignal>
  #include <pty.h>
  #include <sys/ioctl.h>
  #include <sys/wait.h>
  #include <termios.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace steamshine_terminal {

  namespace {

    constexpr std::size_t MAX_BACKLOG_BYTES = 64 * 1024;

    std::mutex g_pty_mutex;
    int g_master_fd {-1};
#if defined(__linux__)
    pid_t g_pid {-1};
#endif
    std::jthread g_reader;

    std::mutex g_sub_mutex;
    std::unordered_map<std::uint64_t, output_callback_t> g_subscribers;
    std::uint64_t g_next_subscriber_id {1};
    std::string g_backlog;

    /**
     * @brief Append one output chunk to the backlog and fan it out to current subscribers.
     *
     * Holds `g_sub_mutex` for the entire fan-out (not just the backlog update)
     * so that unsubscribe() -- which needs the same lock to erase an entry --
     * cannot return until any in-flight callback invocation for that
     * subscriber has finished. This is what makes it safe for a caller to
     * destroy state a callback closes over immediately after unsubscribe()
     * returns (see the WebSocket handler in confighttp.cpp, which references
     * its own stack-local connection object from the callback).
     */
    void broadcast(std::string_view chunk) {
      std::lock_guard lock {g_sub_mutex};
      g_backlog.append(chunk);
      if (g_backlog.size() > MAX_BACKLOG_BYTES) {
        g_backlog.erase(0, g_backlog.size() - MAX_BACKLOG_BYTES);
      }
      for (const auto &[id, callback] : g_subscribers) {
        callback(chunk);
      }
    }

#if defined(__linux__)
    /**
     * @brief Read PTY output until the child exits or the master side is closed, then clean up.
     */
    void reader_loop(int fd) {
      char buffer[4096];
      while (true) {
        const auto count {::read(fd, buffer, sizeof(buffer))};
        if (count <= 0) {
          break;
        }
        broadcast(std::string_view {buffer, static_cast<std::size_t>(count)});
      }
      std::lock_guard lock {g_pty_mutex};
      if (g_master_fd == fd) {
        ::close(g_master_fd);
        g_master_fd = -1;
        if (g_pid > 0) {
          int status {};
          ::waitpid(g_pid, &status, 0);
          g_pid = -1;
        }
      }
    }
#endif

  }  // namespace

  bool ensure_started() {
#if defined(__linux__)
    std::lock_guard lock {g_pty_mutex};
    if (g_pid > 0) {
      return true;
    }
    struct winsize window_size {};
    window_size.ws_row = 24;
    window_size.ws_col = 80;
    int master_fd {};
    const pid_t pid {::forkpty(&master_fd, nullptr, nullptr, &window_size)};
    if (pid < 0) {
      BOOST_LOG(warning) << "steamshine_terminal: forkpty failed"sv;
      return false;
    }
    if (pid == 0) {
      // Child: exec the user's login shell, inheriting the same unprivileged
      // credentials as the Sunshine process. No capability is raised here.
      ::setenv("TERM", "xterm-256color", 1);
      const char *shell {std::getenv("SHELL")};
      if (!shell || !*shell) {
        shell = "/bin/bash";
      }
      ::execl(shell, shell, "-l", static_cast<char *>(nullptr));
      _exit(127);
    }
    g_master_fd = master_fd;
    g_pid = pid;
    {
      std::lock_guard sub_lock {g_sub_mutex};
      g_backlog.clear();
    }
    g_reader = std::jthread {reader_loop, master_fd};
    return true;
#else
    return false;
#endif
  }

  void stop() {
#if defined(__linux__)
    pid_t pid_to_kill {-1};
    std::jthread reader_to_join;
    {
      // Move g_reader out under the lock (rather than reading it directly
      // here) so a concurrent ensure_started() can never race on the same
      // jthread handle; the actual blocking join happens below, after the
      // lock is released, since reader_loop's own cleanup needs to acquire
      // this same mutex.
      std::lock_guard lock {g_pty_mutex};
      pid_to_kill = g_pid;
      reader_to_join = std::move(g_reader);
    }
    if (pid_to_kill > 0) {
      ::kill(pid_to_kill, SIGKILL);
    }
    if (reader_to_join.joinable()) {
      reader_to_join.join();
    }
#endif
  }

  bool running() {
#if defined(__linux__)
    std::lock_guard lock {g_pty_mutex};
    return g_pid > 0;
#else
    return false;
#endif
  }

  void write_input(std::string_view data) {
#if defined(__linux__)
    std::lock_guard lock {g_pty_mutex};
    if (g_master_fd >= 0) {
      // Best-effort: a short write on a PTY under normal terminal load is not
      // something an interactive shell session needs to recover from byte-exactly.
      [[maybe_unused]] const auto written {::write(g_master_fd, data.data(), data.size())};
    }
#else
    (void) data;
#endif
  }

  void resize(unsigned short cols, unsigned short rows) {
#if defined(__linux__)
    std::lock_guard lock {g_pty_mutex};
    if (g_master_fd >= 0) {
      struct winsize window_size {};
      window_size.ws_col = cols;
      window_size.ws_row = rows;
      ::ioctl(g_master_fd, TIOCSWINSZ, &window_size);
    }
#else
    (void) cols;
    (void) rows;
#endif
  }

  std::uint64_t subscribe(output_callback_t callback) {
    // Deliver the backlog replay under the same lock used by broadcast() so a
    // concurrent live chunk can never be interleaved ahead of it.
    std::lock_guard lock {g_sub_mutex};
    const std::uint64_t id {g_next_subscriber_id++};
    if (!g_backlog.empty()) {
      callback(g_backlog);
    }
    g_subscribers.emplace(id, callback);
    return id;
  }

  void unsubscribe(std::uint64_t id) {
    std::lock_guard lock {g_sub_mutex};
    g_subscribers.erase(id);
  }

}  // namespace steamshine_terminal
