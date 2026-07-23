/**
 * @file src/steamos_virtual_session.h
 * @brief SteamOS headless Gamescope session lifecycle declarations.
 */
#pragma once

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
   * @brief Mark a prepared session as streaming after the RTSP launch is queued.
   */
  void mark_streaming();

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
