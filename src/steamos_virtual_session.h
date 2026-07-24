/**
 * @file src/steamos_virtual_session.h
 * @brief SteamOS headless Gamescope session lifecycle declarations.
 */
#pragma once

#include "steamos_virtual_session_core.h"

#include <cstddef>
#include <string>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace steamos_virtual_session {
  /**
   * @brief States owned by the SteamOS virtual-session manager.
   */
  enum class state_e {
    Disabled,  ///< Feature flag or host capability is unavailable.
    Idle,  ///< No virtual session is owned.
    Starting,  ///< Gamescope is being spawned.
    WaitingForDisplay,  ///< Waiting for a verified Wayland display endpoint.
    WaitingForCapture,  ///< Waiting for Sunshine to attach its capture backend.
    Ready,  ///< A capture-ready virtual display is available.
    Streaming,  ///< Sunshine has accepted the associated GameStream session.
    Stopping,  ///< Owned child processes are being stopped.
    Failed,  ///< Startup or readiness failed.
    Recovering,  ///< Cleanup is returning the manager to Idle.
  };

  /**
   * @brief Start an owned headless Gamescope session for a GameStream launch.
   *
   * @param launch_session Moonlight request containing width, height, FPS, and HDR intent.
   * @param error Human-readable failure reason for the GameStream response.
   * @return True only after a Wayland readiness signal is observed.
   */
  bool prepare(const rtsp_stream::launch_session_t &launch_session, std::string &error);

  /**
   * @brief Report whether SteamOS virtual-display capture is enabled.
   *
   * Linux platform initialization uses this before a session exists so it can
   * retain the Wayland DMA-BUF backend while the host has no physical output.
   *
   * @return True when the opt-in SteamOS virtual-display feature is enabled.
   */
  bool capture_backend_required();

  /**
   * @brief Remove stale runtime sessions that are provably owned by SteamShine.
   *
   * This is called once during normal service startup. It only considers
   * directories with a SteamShine ownership marker and processes whose
   * `XDG_RUNTIME_DIR` exactly matches that directory.
   */
  void cleanup_orphan_sessions();

  /**
   * @brief Mark a prepared session as streaming after the RTSP launch is queued.
   */
  void mark_streaming();

  /**
   * @brief Record one encoded video packet for the owned virtual session.
   *
   * This is an in-memory, lock-free counter used only for the final lifecycle
   * report. It deliberately performs no file or network I/O on the video
   * broadcast thread.
   *
   * @param bytes Encoded payload bytes before GameStream packetization.
   * @param idr True when the packet is an IDR/key frame.
   */
  void mark_encoded_packet(size_t bytes, bool idr);

  /**
   * @brief Return the owned Wayland environment for the application launcher.
   *
   * The values are available only after Gamescope has passed readiness. Callers
   * must not retain them after the associated launch session ends.
   *
   * @param runtime_directory Receives the session-owned XDG runtime directory.
   * @param wayland_display Receives the session-owned Wayland display name.
   * @return True when an application may safely connect to the virtual display.
   */
  bool application_environment(std::string &runtime_directory, std::string &wayland_display);

  /**
   * @brief Return the absolute path to the owned Wayland socket for capture.
   *
   * Capture backends use this path to create their own socket connection rather
   * than changing Sunshine's process-wide XDG runtime environment.
   *
   * @param socket_path Receives the absolute Wayland socket path.
   * @return True when the owned socket is ready to accept a capture connection.
   */
  bool capture_socket(std::string &socket_path);

  /**
   * @brief Return the active session's AMD render node for capture and encoding.
   *
   * @param render_node Receives the selected `/dev/dri/renderD*` path.
   * @return True when the active virtual session selected an AMD dGPU render node.
   */
  bool encoder_render_node(std::string &render_node);

  /**
   * @brief Check whether SteamShine currently owns a virtual Gamescope session.
   *
   * @return True only while an owned process group and session runtime are active.
   */
  bool active();

  /**
   * @brief Record that a capture backend successfully attached to the session.
   *
   * This transition is deliberately called by the capture backend, rather than
   * treating a socket's existence as proof that frames can be captured.
   */
  void mark_capture_ready();

  /**
   * @brief Stop only the process group and runtime directory owned by SteamShine.
   */
  void stop();

  /**
   * @brief Return the current manager state.
   *
   * @return Current lifecycle state.
   */
  state_e state();
}  // namespace steamos_virtual_session
