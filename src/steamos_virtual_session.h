/**
 * @file src/steamos_virtual_session.h
 * @brief SteamOS headless Gamescope session lifecycle declarations.
 */
#pragma once

#include "platform/linux/pipewire_capture.h"
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
   * @brief Verification state for a session display endpoint.
   */
  enum class display_verification_e {
    unavailable,  ///< No session-specific display endpoint is available.
    verified,  ///< The producer identity, sockets, and authorization file were verified.
    rejected,  ///< Endpoint evidence was present but failed closed validation.
  };

  /**
   * @brief Immutable application display endpoint for one verified session generation.
   */
  struct session_display_endpoint_t {
    session_origin_e origin {session_origin_e::none};  ///< Session that produced the endpoint.
    std::string xdg_runtime_directory;  ///< Runtime directory inherited by session applications.
    std::string wayland_display;  ///< Wayland socket name exposed to compatible applications.
    std::string gamescope_wayland_display;  ///< Gamescope's explicitly exposed Wayland socket name.
    std::string x11_display;  ///< Dynamically allocated Gamescope Xwayland display name.
    std::string xauthority;  ///< Xauthority file, or empty for a verified auth-less SteamOS vendor Xwayland.
    std::string pipewire_runtime_directory;  ///< Verified host PipeWire runtime directory.
    std::string pipewire_remote;  ///< Verified host PipeWire remote socket name.
    std::string pulse_runtime_path;  ///< Host PulseAudio compatibility runtime path.
    std::string dbus_session_bus_address;  ///< Verified resident session bus address, when available.
    std::string xdg_session_type;  ///< Verified display protocol classification for application launches.
    std::string xdg_current_desktop;  ///< Verified desktop identity for application launches.
    int producer_pid {-1};  ///< Gamescope producer PID bound to the snapshot.
    uint64_t producer_start_time {0};  ///< Producer start time used to reject PID reuse.
    int environment_source_pid {-1};  ///< Bootstrap or resident Steam PID supplying the environment.
    uint64_t environment_source_start_time {0};  ///< Environment source start time used to reject PID reuse.
    uint64_t generation {0};  ///< Monotonic session generation invalidating older snapshots.
    display_verification_e verification {display_verification_e::unavailable};  ///< Endpoint verification result.
    std::string error;  ///< Stable rejection reason without credentials.
  };

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
    session_origin_e origin {session_origin_e::none};  ///< Origin of the selected Gamescope source.
    bool process_owned {false};  ///< Whether SteamShine may signal the Gamescope process group.
    bool runtime_owned {false};  ///< Whether SteamShine may remove the session runtime directory.
    std::string source_description;  ///< Human-readable verified PipeWire source description.
    std::string source_executable;  ///< Verified Gamescope executable path.
    uint64_t source_process_start_time {0};  ///< Verified Gamescope `/proc` start time.
    std::string steam_location;  ///< Steam singleton location relative to the selected Gamescope.
    bool migration_required {false};  ///< Whether an explicitly confirmed Desktop Steam migration is required.
    std::string app_launch_rejected_reason;  ///< Stable machine-readable reason for the latest rejected application launch.
    std::string app_launch_rejected_message;  ///< Safe operator-facing detail for the latest rejected application launch.
    session_display_endpoint_t display_endpoint;  ///< Active verified application display endpoint.
    std::string selection_reason;  ///< Stable reason for selecting Desktop capture or a Gamescope source.
    presentation_e presentation {presentation_e::remote_only};  ///< Desired remote/local presentation paths.
    bool local_presenter_active {false};  ///< Whether a local presenter has attached successfully.
    uint64_t local_presented_frames {0};  ///< Frames shown by the local presenter.
    uint64_t local_dropped_frames {0};  ///< Latest-frame-wins drops by the local presenter.
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
   * @brief Check whether a physical DRM connector is connected.
   *
   * @return True when a non-writeback physical connector reports connected.
   */
  bool physical_output_connected();

  /**
   * @brief Publish whether the compositor exposed a physical Desktop capture source.
   *
   * Linux platform initialization calls this after KWin and Portal
   * enumeration so the later Moonlight launch decision uses the same source
   * evidence as encoder probing.
   *
   * @param available Whether a compositor capture source is currently available.
   */
  void set_physical_compositor_capture_available(bool available);

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
   * @brief Handle stream disconnection according to the owned-session policy.
   *
   * A transient Moonlight disconnect retains the session by default. When
   * @c steamos_keep_session_alive is disabled, only a SteamShine-owned session
   * is stopped; an attached Game Mode session is always left untouched.
   */
  void mark_streaming_disconnected();

  /**
   * @brief Record one encoded video packet for the active virtual session.
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
   * @brief Record one successfully acquired verified-session frame.
   *
   * This lock-free counter is maintained only while the owned session is
   * streaming. It supplies final acceptance evidence without file I/O on the
   * capture thread.
   */
  void mark_captured_frame();

  /**
   * @brief Return one verified display endpoint snapshot for application launch.
   *
   * The snapshot is available only while its producer PID, start time, and
   * session generation remain current. Physical Desktop sessions return no
   * endpoint so the launcher's inherited host environment remains unchanged.
   *
   * @return Verified immutable endpoint, or std::nullopt when none is active.
   */
  std::optional<session_display_endpoint_t> application_environment();

  /**
   * @brief Persist the environment inherited by a Gamescope bootstrap child.
   *
   * This internal command writes one owner-only atomic endpoint report and then
   * watches the exact owning daemon identity. If that daemon exits or is
   * replaced, the bootstrap exits so Gamescope's reaper closes the entire owned
   * session. It performs no capture, rendering, or GPU work.
   *
   * @param report_directory Owned session runtime directory.
   * @param generation Session generation supplied by the owning daemon.
   * @param owner_pid Process identifier of the owning SteamShine daemon.
   * @param owner_start_time Linux process start time of the owning daemon.
   * @return Zero after normal termination, or a nonzero validation/write error.
   */
  int run_display_endpoint_bootstrap(std::string_view report_directory, uint64_t generation, int owner_pid, uint64_t owner_start_time);

  /**
   * @brief Check whether a configured command may start or address Steam.
   *
   * Every Steam request rechecks the live process placement. Steam commands
   * are rejected when an existing instance is outside the selected Gamescope
   * or its placement cannot be verified. This avoids silently creating a
   * second Steam instance while allowing ordinary non-Steam applications to
   * run normally.
   *
   * @param command Configured application command.
   * @param error_message Human-readable rejection reason.
   * @return True when the command is safe to execute.
   */
  bool steam_command_allowed(std::string_view command, std::string &error_message);

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
   * @brief Return the verified host PipeWire endpoint and selected Gamescope PID.
   *
   * @param runtime_directory Receives the host PipeWire runtime directory.
   * @param remote_name Receives the host PipeWire remote name.
   * @param gamescope_pid Receives the selected Gamescope producer PID.
   * @return True only while an active session can use its host PipeWire endpoint.
   */
  bool gamescope_pipewire_endpoint(std::string &runtime_directory, std::string &remote_name, int &gamescope_pid);

  /**
   * @brief Test whether input must be isolated to a selected Gamescope source.
   *
   * @return True while an owned or attached Gamescope session is streamable.
   */
  bool gamescope_input_required();

  /**
   * @brief Return the identity generation of the active Gamescope input source.
   *
   * The selected producer PID and start time are revalidated before returning.
   * An established libei connection may be reused only while this generation
   * remains unchanged.
   *
   * @param generation Receives the active display-endpoint generation.
   * @param error Receives a stable failure reason without credentials.
   * @return True while the selected Gamescope identity remains current.
   */
  bool gamescope_input_generation(std::uint64_t &generation, std::string &error);

  /**
   * @brief Open the EIS input socket owned by the selected verified Gamescope process.
   *
   * The socket is accepted only when its path remains inside the selected
   * runtime, it is a current-user UNIX-domain socket, and the kernel-reported
   * `SO_PEERCRED` PID matches the selected Gamescope identity.
   *
   * The returned descriptor is the exact kernel-authenticated connection and
   * must be closed by the caller or transferred to libei.
   *
   * @param socket_path Receives the absolute EIS socket path.
   * @param descriptor Receives a connected close-on-exec descriptor.
   * @param generation Receives the generation verified with the connection.
   * @param error Receives a stable failure reason without credentials.
   * @return True when input can be bound to the selected Gamescope session.
   */
  bool open_verified_gamescope_input(std::string &socket_path, int &descriptor, std::uint64_t &generation, std::string &error);

  /**
   * @brief Open one dedicated PipeWire connection for the verified source.
   *
   * Every caller receives a new connection FD. The remote encoder and local
   * presenter therefore share source identity only, never a PipeWire core or
   * queue. The source is rediscovered and checked against its PID, start time,
   * object serial, and render node before the FD is returned.
   *
   * @param descriptor Receives the dedicated consumer descriptor.
   * @param error Receives a human-readable validation or socket error.
   * @return True only when the returned descriptor is still verified.
   */
  bool open_verified_gamescope_pipewire_consumer(pipewire_capture::stream_descriptor_t &descriptor, std::string &error);

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
   * @brief Prepare an active verified capture source for backend reinitialization.
   *
   * PipeWire may pause a Gamescope stream while renegotiating its frame size.
   * Reinitialization is permitted only while the selected compositor process
   * still has the original PID start-time identity.
   *
   * @return True when capture may be rebuilt against the same verified source.
   */
  bool mark_capture_reinitializing();

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
