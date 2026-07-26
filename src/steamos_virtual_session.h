/**
 * @file src/steamos_virtual_session.h
 * @brief SteamOS headless Gamescope session lifecycle declarations.
 */
#pragma once

#include "steamos_virtual_session_core.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
   * @brief Read-only diagnostics for the active owned virtual session.
   */
  struct status_snapshot_t {
    state_e state {state_e::Disabled};  ///< Current lifecycle state.
    std::string runtime_directory;  ///< Owned runtime directory, when active.
    std::string socket_path;  ///< Private Gamescope Wayland socket, when ready.
    std::string pci_bdf;  ///< Selected AMD PCI BDF, when available.
    std::string render_node;  ///< Selected AMD render node, when available.
    std::string pipewire_runtime;  ///< Host PipeWire runtime directory, when available.
    std::string pipewire_remote;  ///< Host PipeWire remote name, when available.
    std::optional<uint32_t> pipewire_node_id;  ///< Verified volatile PipeWire node ID, when attached.
    std::optional<uint64_t> pipewire_object_serial;  ///< Verified stable PipeWire object serial, when attached.
    int pipewire_producer_pid {-1};  ///< Verified Gamescope producer PID, when attached.
    int width {0};  ///< Requested virtual-display width in pixels.
    int height {0};  ///< Requested virtual-display height in pixels.
    int fps {0};  ///< Requested virtual-display refresh rate.
    int gamescope_pid {-1};  ///< Owned Gamescope leader PID, when available.
    uint64_t captured_frames {0};  ///< Frames acquired from the owned DMA-BUF path.
    uint64_t encoded_packets {0};  ///< Encoded packets emitted by the session.
    uint64_t encoded_bytes {0};  ///< Encoded payload bytes emitted by the session.
    uint64_t idr_packets {0};  ///< IDR packets emitted by the session.
  };

  /**
   * @brief Return a stable state label suitable for diagnostics and Web UI output.
   *
   * @param state Lifecycle state to serialize.
   * @return Canonical state label.
   */
  std::string_view to_string(state_e state);

  /**
   * @brief Capture a thread-safe view of the current owned-session diagnostics.
   *
   * @return Snapshot that does not expose credentials or session cookies.
   */
  status_snapshot_t status_snapshot();

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
   * @brief Suspend packet accounting while retaining the owned virtual session.
   *
   * A transient Moonlight disconnect must not terminate the application or its
   * Gamescope session. A later `/resume` request calls @ref mark_streaming to
   * attach a new stream to the retained virtual display.
   */
  void mark_streaming_disconnected();

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
   * @brief Record one successfully acquired Wayland DMA-BUF frame.
   *
   * This lock-free counter is maintained only while the owned session is
   * streaming. It supplies final acceptance evidence without file I/O on the
   * capture thread.
   */
  void mark_captured_frame();

  /**
   * @brief Return the owned Wayland environment for the application launcher.
   *
   * The values are available only after Gamescope has passed readiness. Callers
   * must not retain them after the associated launch session ends.
   *
   * @param runtime_directory Receives the session-owned XDG runtime directory.
   * @param wayland_display Receives the session-owned Wayland display name.
   * @param pipewire_runtime Receives the host PipeWire runtime directory.
   * @param pipewire_remote Receives the host PipeWire remote name.
   * @param pulse_runtime Receives the host PulseAudio compatibility runtime directory.
   * @return True when an application may safely connect to the virtual display.
   */
  bool application_environment(std::string &runtime_directory, std::string &wayland_display, std::string &pipewire_runtime, std::string &pipewire_remote, std::string &pulse_runtime);

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
   * @brief Report whether capture must use the owned Wayland socket.
   *
   * A prepared virtual session must fail closed when its private socket is
   * unavailable.  This prevents a lost or invalid Gamescope socket from
   * silently reconnecting capture to a desktop Wayland display.
   *
   * @return True when desktop-Wayland capture fallback is forbidden.
   */
  bool capture_socket_required();

  /**
   * @brief Return the active session's AMD render node for capture and encoding.
   *
   * @param render_node Receives the selected `/dev/dri/renderD*` path.
   * @return True when the active virtual session selected an AMD dGPU render node.
   */
  bool encoder_render_node(std::string &render_node);

  /**
   * @brief Return the verified host PipeWire endpoint and owned Gamescope PID.
   *
   * @param runtime_directory Receives the host PipeWire runtime directory.
   * @param remote_name Receives the host PipeWire remote name.
   * @param gamescope_pid Receives the owned Gamescope process-group leader.
   * @return True only while an owned session can use its host PipeWire endpoint.
   */
  bool gamescope_pipewire_endpoint(std::string &runtime_directory, std::string &remote_name, int &gamescope_pid);

  /**
   * @brief Record the identity of the verified owned Gamescope PipeWire node.
   *
   * @param node_id Volatile node ID on the current PipeWire core.
   * @param object_serial Stable PipeWire object serial for the node.
   * @param producer_pid Verified Gamescope producer process ID.
   */
  void mark_gamescope_pipewire_node(uint32_t node_id, uint64_t object_serial, int producer_pid);

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
   * @brief Record an owned virtual-display capture failure without blocking capture.
   *
   * The capture thread only performs this state transition. The existing stream
   * teardown path subsequently stops the owned application, Gamescope process
   * group, and runtime directory; no process wait or filesystem operation is
   * introduced on the capture path.
   */
  void mark_capture_lost();

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
