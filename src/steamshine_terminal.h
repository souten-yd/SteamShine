/**
 * @file src/steamshine_terminal.h
 * @brief Single-session PTY-backed shell for the SteamShine web Terminal.
 *
 * The shell runs as the same unprivileged user as the Sunshine process
 * itself -- no capability is raised for this feature, unlike
 * `steamshine_gpuctl`. Authenticating the WebSocket transport (session
 * cookie + CSRF handshake) is the caller's responsibility, in
 * `confighttp.cpp`; this module only owns the PTY lifecycle and a small
 * output ring buffer so a reconnecting browser can see what it missed.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace steamshine_terminal {

  using output_callback_t = std::function<void(std::string_view)>;

  /**
   * @brief Start the single global shell session if one is not already running.
   *
   * @return True when a session is running after this call.
   */
  bool ensure_started();

  /**
   * @brief Terminate the current shell session, if any, and wait for cleanup to finish.
   */
  void stop();

  /**
   * @brief Whether a shell session is currently running.
   */
  bool running();

  /**
   * @brief Write raw bytes to the shell's stdin. No-op when no session is running.
   */
  void write_input(std::string_view data);

  /**
   * @brief Resize the PTY window. No-op when no session is running.
   */
  void resize(unsigned short cols, unsigned short rows);

  /**
   * @brief Subscribe to PTY output chunks.
   *
   * The current backlog (bounded ring buffer, most-recent ~64 KiB) is
   * delivered synchronously to `callback` before this call returns, so a
   * reconnecting client immediately sees what it missed.
   *
   * @param callback Invoked with each output chunk as it arrives.
   * @return Subscription id to pass to unsubscribe().
   */
  std::uint64_t subscribe(output_callback_t callback);

  /**
   * @brief Remove a previously registered subscription.
   */
  void unsubscribe(std::uint64_t id);

}  // namespace steamshine_terminal
