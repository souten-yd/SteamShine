/**
 * @file src/steamshine_terminal.h
 * @brief Multi-session PTY-backed shells for the SteamShine web Terminal.
 *
 * Each session runs as the same unprivileged user as Sunshine. The HTTP and
 * WebSocket layers own authentication; this module owns PTY lifecycle,
 * session metadata, and bounded output replay for reconnecting browsers.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
  #include <termios.h>
#endif

namespace steamshine_terminal {

  using output_callback_t = std::function<void(std::string_view)>;  ///< Callback invoked for one PTY output chunk.

#if defined(__linux__)
  /**
   * @brief Close descriptors that a terminal child must not inherit.
   *
   * Standard input, output, and error remain open. This prevents persistent
   * tmux servers from retaining SteamShine listener sockets across a service
   * restart and blocking the replacement process from binding its ports.
   */
  void close_inherited_file_descriptors();

  /**
   * @brief Put a tmux attachment PTY into raw, non-echoing mode.
   *
   * This must run in the attachment child before tmux is executed. Otherwise,
   * terminal capability replies can be echoed during the short interval before
   * tmux configures the PTY, leaving fragments such as `0;276;0c` in history.
   *
   * @param terminal_fd File descriptor for the attachment PTY slave.
   * @return True when the terminal attributes were updated.
   */
  bool prepare_tmux_attachment_terminal(int terminal_fd);
#endif

  /**
   * @brief Public state for one web terminal session.
   */
  struct session_snapshot_t {
    std::string id;  ///< Stable identifier used by the HTTP and WebSocket APIs.
    std::string name;  ///< Short user-facing session name.
    std::string explicit_end_token;  ///< Process-local nonce required to explicitly terminate this session.
    std::uint64_t created_at;  ///< Unix creation time in seconds.
    bool running;  ///< Whether the login shell is still alive.
    bool persistent;  ///< Whether the shell survives a SteamShine restart.
  };

  /**
   * @brief Create and start a new login-shell session.
   *
   * @return New session identifier, or an empty string when the PTY could not be created.
   */
  std::string create();

  /**
   * @brief Return snapshots for every retained session, oldest first.
   *
   * Exited sessions remain visible until explicitly stopped so the UI can
   * explain that they ended instead of silently losing the tab.
   *
   * @return Current session snapshots.
   */
  std::vector<session_snapshot_t> list();

  /**
   * @brief Terminate and remove one session.
   *
   * Browser disconnects and stale pages cannot invoke this operation without
   * the current process-local confirmation token returned by list().
   *
   * @param session_id Session to terminate.
   * @param explicit_end_token Confirmation nonce returned by list().
   * @return True when the session and confirmation token matched and the
   * session was removed.
   */
  bool stop(std::string_view session_id, std::string_view explicit_end_token);

  /**
   * @brief Terminate and remove every session.
   */
  void stop_all();

  /**
   * @brief Detach SteamShine from every session during service shutdown.
   *
   * OS-managed tmux sessions remain alive for the next SteamShine process.
   * In-process fallback shells are terminated because their owner is exiting.
   */
  void detach_all();

  /**
   * @brief Test whether one session is running.
   *
   * @param session_id Session to inspect.
   * @return True when the session exists and its shell is alive.
   */
  bool running(std::string_view session_id);

  /**
   * @brief Write raw bytes to one shell's standard input.
   *
   * @param session_id Destination session.
   * @param data Bytes to write.
   * @return True when all bytes were written to a running PTY.
   */
  bool write_input(std::string_view session_id, std::string_view data);

  /**
   * @brief Scroll one persistent tmux session's retained history.
   *
   * Negative lines move toward older output and positive lines move toward
   * the live bottom. In-process fallback PTYs do not expose a remote history
   * mode and therefore reject this operation.
   *
   * @param session_id Destination session.
   * @param lines Signed number of history lines to move.
   * @return True when tmux accepted the scroll operation.
   */
  bool scroll(std::string_view session_id, int lines);

  /**
   * @brief Resize one PTY window.
   *
   * @param session_id Destination session.
   * @param cols Terminal column count.
   * @param rows Terminal row count.
   * @return True when the resize was applied to a running PTY.
   */
  bool resize(std::string_view session_id, unsigned short cols, unsigned short rows);

  /**
   * @brief Subscribe to one session's PTY output.
   *
   * The bounded backlog is delivered synchronously before live output. The
   * callback is serialized with unsubscribe(), allowing a connection to
   * destroy callback state immediately after unsubscribe() returns.
   *
   * @param session_id Session to observe.
   * @param callback Callback receiving backlog and live chunks.
   * @return Subscription identifier, or zero when the session does not exist.
   */
  std::uint64_t subscribe(std::string_view session_id, output_callback_t callback);

  /**
   * @brief Remove a subscription from one session.
   *
   * @param session_id Session holding the subscription.
   * @param subscription_id Identifier returned by subscribe().
   */
  void unsubscribe(std::string_view session_id, std::uint64_t subscription_id);

}  // namespace steamshine_terminal
