/**
 * @file tests/unit/test_steamshine_terminal.cpp
 * @brief Tests for the SteamShine multi-session PTY manager.
 */

#include "../tests_common.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <src/steamshine_terminal.h>
#include <string>

#if defined(__linux__)
  #include <cerrno>
  #include <fcntl.h>
  #include <pty.h>
  #include <sys/wait.h>
  #include <termios.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace {

  /**
   * @brief Stop every test-created login shell after each assertion path.
   */
  class SteamshineTerminalTest: public testing::Test {
  protected:
#if defined(__linux__)
    std::filesystem::path isolated_tmux_root_;  ///< Private tmux socket directory used by this test only.
    std::optional<std::string> original_tmux_;  ///< Inherited tmux client identity restored after the test.
    std::optional<std::string> original_tmux_tmpdir_;  ///< Inherited tmux socket root restored after the test.
#endif

    /**
     * @brief Begin each test with an empty, isolated tmux server.
     *
     * A developer commonly runs this suite from the SteamShine Web Terminal.
     * tmux gives its own client identity precedence over TMUX_TMPDIR, so both
     * values must be isolated before stop_all() can safely clean test sessions.
     */
    void SetUp() override {
#if defined(__linux__)
      if (const char *value {std::getenv("TMUX")}) {
        original_tmux_ = value;
      }
      if (const char *value {std::getenv("TMUX_TMPDIR")}) {
        original_tmux_tmpdir_ = value;
      }
      isolated_tmux_root_ = std::filesystem::temp_directory_path() / std::format(
                                                                       "steamshine-terminal-test-{}-{}",
                                                                       ::getpid(),
                                                                       std::chrono::steady_clock::now().time_since_epoch().count()
                                                                     );
      ASSERT_TRUE(std::filesystem::create_directories(isolated_tmux_root_));
      ASSERT_EQ(::unsetenv("TMUX"), 0);
      ASSERT_EQ(::setenv("TMUX_TMPDIR", isolated_tmux_root_.c_str(), 1), 0);
#endif
      steamshine_terminal::stop_all();
    }

    /**
     * @brief Reap isolated shells and restore the caller's tmux environment.
     */
    void TearDown() override {
      steamshine_terminal::stop_all();
#if defined(__linux__)
      if (original_tmux_) {
        (void) ::setenv("TMUX", original_tmux_->c_str(), 1);
      } else {
        (void) ::unsetenv("TMUX");
      }
      if (original_tmux_tmpdir_) {
        (void) ::setenv("TMUX_TMPDIR", original_tmux_tmpdir_->c_str(), 1);
      } else {
        (void) ::unsetenv("TMUX_TMPDIR");
      }
      std::error_code error;
      std::filesystem::remove_all(isolated_tmux_root_, error);
#endif
    }
  };

}  // namespace

/**
 * @brief Verify missing session identifiers fail without changing global state.
 */
TEST_F(SteamshineTerminalTest, RejectsUnknownSession) {
  EXPECT_FALSE(steamshine_terminal::running("missing"));
  EXPECT_FALSE(steamshine_terminal::write_input("missing", "echo ignored\n"));
  EXPECT_FALSE(steamshine_terminal::scroll("missing", -1));
  EXPECT_FALSE(steamshine_terminal::scroll("missing", 0));
  EXPECT_FALSE(steamshine_terminal::resize("missing", 100, 40));
  EXPECT_FALSE(steamshine_terminal::redraw("missing"));
  EXPECT_EQ(steamshine_terminal::subscribe("missing", [](std::string_view) {
            }),
            0U);
  EXPECT_FALSE(steamshine_terminal::stop("missing", "invalid"));
  EXPECT_TRUE(steamshine_terminal::list().empty());
}

#if defined(__linux__)
/**
 * @brief Verify persistent terminal children cannot retain service sockets.
 */
TEST_F(SteamshineTerminalTest, ClosesInheritedDescriptorsInTerminalChild) {
  int inherited_pipe[2] {-1, -1};
  ASSERT_EQ(::pipe(inherited_pipe), 0);
  ASSERT_GT(inherited_pipe[0], STDERR_FILENO);
  ASSERT_GT(inherited_pipe[1], STDERR_FILENO);

  const pid_t child {::fork()};
  ASSERT_GE(child, 0);
  if (child == 0) {
    steamshine_terminal::close_inherited_file_descriptors();
    errno = 0;
    const bool read_closed {::fcntl(inherited_pipe[0], F_GETFD) == -1 && errno == EBADF};
    errno = 0;
    const bool write_closed {::fcntl(inherited_pipe[1], F_GETFD) == -1 && errno == EBADF};
    _exit(read_closed && write_closed ? 0 : 1);
  }

  int status {};
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ::close(inherited_pipe[0]);
  ::close(inherited_pipe[1]);
}

/**
 * @brief Verify tmux attachment PTYs cannot echo terminal capability replies.
 */
TEST_F(SteamshineTerminalTest, PreparesTmuxAttachmentPtyWithoutEcho) {
  int master_fd {-1};
  int slave_fd {-1};
  ASSERT_EQ(::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr), 0);

  ASSERT_TRUE(steamshine_terminal::prepare_tmux_attachment_terminal(slave_fd));
  struct termios attributes {};
  ASSERT_EQ(::tcgetattr(slave_fd, &attributes), 0);
  EXPECT_EQ(attributes.c_lflag & ECHO, 0U);
  EXPECT_EQ(attributes.c_lflag & ICANON, 0U);

  ::close(slave_fd);
  ::close(master_fd);
}

/**
 * @brief Verify independent sessions, PTY I/O, backlog replay, and selective deletion.
 */
TEST_F(SteamshineTerminalTest, ManagesIndependentSessionsAndReplaysOutput) {
  const auto first {steamshine_terminal::create()};
  const auto second {steamshine_terminal::create()};
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_NE(first, second);
  EXPECT_TRUE(steamshine_terminal::running(first));
  EXPECT_TRUE(steamshine_terminal::running(second));

  const auto sessions {steamshine_terminal::list()};
  ASSERT_EQ(sessions.size(), 2U);
  const auto first_session {std::ranges::find(sessions, first, &steamshine_terminal::session_snapshot_t::id)};
  ASSERT_NE(first_session, sessions.end());
  EXPECT_FALSE(first_session->explicit_end_token.empty());
  EXPECT_NE(std::ranges::find(sessions, second, &steamshine_terminal::session_snapshot_t::id), sessions.end());

  std::mutex output_mutex;
  std::condition_variable output_ready;
  std::string output;
  const auto subscription {steamshine_terminal::subscribe(first, [&](const std::string_view chunk) {
    {
      std::lock_guard lock {output_mutex};
      output.append(chunk);
    }
    output_ready.notify_all();
  })};
  ASSERT_NE(subscription, 0U);
  EXPECT_FALSE(steamshine_terminal::scroll(first, 0));
  if (first_session->persistent) {
    EXPECT_TRUE(steamshine_terminal::scroll(first, -2));
  } else {
    EXPECT_FALSE(steamshine_terminal::scroll(first, -2));
  }
  EXPECT_TRUE(steamshine_terminal::resize(first, 111, 37));
  ASSERT_TRUE(steamshine_terminal::write_input(first, "printf 'STEAMSHINE_TERMINAL_TEST:%s\\n' \"$PWD\"\n"));
  {
    std::unique_lock lock {output_mutex};
    const char *home {std::getenv("HOME")};
    ASSERT_NE(home, nullptr);
    ASSERT_TRUE(output_ready.wait_for(lock, 5s, [&] {
      return output.find(std::string {"STEAMSHINE_TERMINAL_TEST:"} + home) != std::string::npos;
    }));
  }
  steamshine_terminal::unsubscribe(first, subscription);

  std::string replay;
  const auto replay_subscription {steamshine_terminal::subscribe(first, [&](const std::string_view chunk) {
    {
      std::lock_guard lock {output_mutex};
      replay.append(chunk);
    }
    output_ready.notify_all();
  })};
  ASSERT_NE(replay_subscription, 0U);
  if (first_session->persistent) {
    // Raw tmux output is geometry-specific, so reconnecting viewers receive a
    // fresh repaint at their reported size instead of a byte replay.
    {
      std::lock_guard lock {output_mutex};
      EXPECT_TRUE(replay.empty());
    }
    EXPECT_TRUE(steamshine_terminal::resize(first, 90, 30));
    EXPECT_TRUE(steamshine_terminal::redraw(first));
  } else {
    EXPECT_FALSE(steamshine_terminal::redraw(first));
  }
  {
    std::unique_lock lock {output_mutex};
    EXPECT_TRUE(output_ready.wait_for(lock, 5s, [&] {
      return replay.find("STEAMSHINE_TERMINAL_TEST") != std::string::npos;
    }));
  }
  steamshine_terminal::unsubscribe(first, replay_subscription);

  if (first_session->persistent) {
    // Scrolling back down past the live bottom leaves copy mode (`-e`), and a
    // later upward swipe re-enters it instead of failing.
    EXPECT_TRUE(steamshine_terminal::scroll(first, -3));
    EXPECT_TRUE(steamshine_terminal::scroll(first, 50));
    EXPECT_FALSE(steamshine_terminal::scroll(first, 1));
    EXPECT_TRUE(steamshine_terminal::scroll(first, -1));
    EXPECT_TRUE(steamshine_terminal::write_input(first, "\n"));
  }

  EXPECT_FALSE(steamshine_terminal::stop(first, "stale-token"));
  EXPECT_TRUE(steamshine_terminal::running(first));
  EXPECT_TRUE(steamshine_terminal::stop(first, first_session->explicit_end_token));
  EXPECT_FALSE(steamshine_terminal::running(first));
  EXPECT_TRUE(steamshine_terminal::running(second));
  ASSERT_EQ(steamshine_terminal::list().size(), 1U);
  EXPECT_EQ(steamshine_terminal::list().front().id, second);
}
#endif
