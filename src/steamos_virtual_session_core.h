/**
 * @file src/steamos_virtual_session_core.h
 * @brief Pure SteamOS virtual-display request and Gamescope command helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace steamos_virtual_session {
  /**
   * @brief Identifies who owns the selected Gamescope session.
   */
  enum class session_origin_e {
    none,  ///< No Gamescope source is selected.
    owned_private,  ///< SteamShine owns the Gamescope process and its private runtime.
    attached_existing,  ///< SteamShine only consumes a verified resident Gamescope source.
  };

  /**
   * @brief Return the stable status spelling for a Gamescope session origin.
   *
   * @param origin Ownership origin to serialize.
   * @return The lowercase status value.
   */
  std::string_view to_string(session_origin_e origin);

  /**
   * @brief Selects which class of Gamescope source a launch may use.
   */
  enum class session_source_policy_e {
    auto_select,  ///< Prefer a verified resident source, then an owned private source.
    existing_gamescope,  ///< Require a verified resident Gamescope source.
    owned_private,  ///< Require a SteamShine-owned private Gamescope source.
  };

  /**
   * @brief Parse a canonical SteamOS session-source policy value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed policy, or std::nullopt when the value is unsupported.
   */
  std::optional<session_source_policy_e> parse_session_source_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a session-source policy.
   *
   * @param policy Policy to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(session_source_policy_e policy);

  /**
   * @brief Selects whether an owned private session is mirrored locally.
   */
  enum class local_presentation_policy_e {
    auto_select,  ///< Mirror only when an owned session and host display are available.
    off,  ///< Keep the session remote-only.
    mirror,  ///< Require a local mirror for a SteamShine-owned private session.
  };

  /**
   * @brief Parse a canonical local-presentation policy value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed policy, or std::nullopt when unsupported.
   */
  std::optional<local_presentation_policy_e> parse_local_presentation_policy(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a local-presentation policy.
   *
   * @param policy Policy to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(local_presentation_policy_e policy);

  /**
   * @brief Active presentation paths for the selected Gamescope session.
   */
  enum class presentation_e {
    remote_only,  ///< Stream only through the remote encoder path.
    remote_and_local,  ///< Stream remotely and mirror the owned private session locally.
  };

  /**
   * @brief Choose local presentation without mirroring an existing Game Mode output.
   *
   * @param policy Local presentation configuration.
   * @param origin Ownership origin of the selected Gamescope source.
   * @param host_wayland_available Whether a captured host Wayland endpoint exists.
   * @param physical_output_connected Whether a local physical output is available.
   * @return Desired presentation paths; unavailable local presentation fails closed to remote-only.
   */
  presentation_e decide_presentation(local_presentation_policy_e policy, session_origin_e origin, bool host_wayland_available, bool physical_output_connected);

  /**
   * @brief Return a stable status spelling for presentation paths.
   *
   * @param presentation Presentation paths to serialize.
   * @return The lowercase status value.
   */
  std::string_view to_string(presentation_e presentation);

  /**
   * @brief Policy governing whether SteamShine owns a virtual display.
   */
  enum class virtual_display_mode_e {
    off,  ///< Never create an owned virtual display.
    auto_detect,  ///< Create one only when no capturable output is available.
    force,  ///< Always create and capture from an owned headless Gamescope session.
  };

  /**
   * @brief Parse a canonical virtual-display mode value.
   *
   * @param value Configuration value to parse; matching is case-sensitive.
   * @return Parsed mode, or std::nullopt when the value is unsupported.
   */
  std::optional<virtual_display_mode_e> parse_virtual_display_mode(std::string_view value);

  /**
   * @brief Return the canonical configuration spelling for a display mode.
   *
   * @param mode Mode to serialize.
   * @return The lowercase configuration value.
   */
  std::string_view to_string(virtual_display_mode_e mode);

  /**
   * @brief Inputs used to make a deterministic virtual-display decision.
   */
  struct virtual_display_decision_input_t {
    bool feature_enabled;  ///< Whether the SteamOS virtual-display feature is enabled.
    virtual_display_mode_e mode;  ///< Requested virtual-display policy.
    bool physical_output_connected;  ///< Whether DRM reports a connected physical output.
    bool active_crtc_present;  ///< Whether a physical CRTC is active.
    bool capturable_output_present;  ///< Whether a normal capture target is available.
    bool existing_owned_session;  ///< Whether SteamShine already owns a compatible session.
    bool host_supported;  ///< Whether the host can create an owned Gamescope session.
    bool verified_existing_gamescope_present {false};  ///< Whether a verified resident Game Mode source must take precedence.
    bool existing_gamescope_required {false};  ///< Whether policy requires attachment instead of normal desktop capture.
  };

  /**
   * @brief Result of a virtual-display policy decision.
   */
  struct virtual_display_decision_t {
    bool required;  ///< Whether the launch must prepare an owned virtual display.
    std::string reason;  ///< Stable diagnostic reason for the decision.
  };

  /**
   * @brief Decide whether a launch requires an owned virtual display.
   *
   * Host support is deliberately not used to disable force mode: callers must
   * report unsupported hosts as a launch error when this returns required.
   *
   * @param input Immutable policy and host-observation inputs.
   * @return Deterministic requirement and diagnostic reason.
   */
  virtual_display_decision_t decide_virtual_display(const virtual_display_decision_input_t &input);

  /**
   * @brief Determine whether the physical Desktop has a usable capture path.
   *
   * A compositor can expose the connected output through KWin ScreenCast or
   * XDG Portal while sysfs reports its DRM CRTC disabled. Either an active
   * CRTC or a verified compositor source is sufficient, but a physical
   * connector is always required.
   *
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param active_crtc_present Whether sysfs reports an enabled physical CRTC.
   * @param compositor_capture_available Whether a compositor exposed a capture source.
   * @return True when automatic policy can keep the physical Desktop.
   */
  bool physical_desktop_capturable(
    bool physical_output_connected,
    bool active_crtc_present,
    bool compositor_capture_available
  );

  /**
   * @brief Decide whether capture must use the private virtual compositor.
   *
   * @param virtual_display_required Whether policy initially requested a virtual display.
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param compositor_capture_available Whether the host compositor exposed a capture source.
   * @param wlr_capture_forced Whether the user explicitly selected the WLR backend.
   * @return True when capture must remain on the private virtual compositor.
   */
  bool use_virtual_capture_backend(
    bool virtual_display_required,
    bool physical_output_connected,
    bool compositor_capture_available,
    bool wlr_capture_forced
  );

  /**
   * @brief Decide whether initialization should probe XDG Portal capture.
   *
   * Automatic capture probes Portal only for a connected physical output and
   * only when a higher-priority compositor backend was not verified. An
   * explicit Portal selection remains authoritative without a physical output.
   *
   * @param automatic_capture Whether no capture backend was explicitly selected.
   * @param portal_explicitly_requested Whether the user selected the Portal backend.
   * @param physical_output_connected Whether a physical DRM connector is connected.
   * @param higher_priority_available Whether KWin or another preferred backend is ready.
   * @return True when Portal discovery should run.
   */
  bool should_probe_physical_portal(
    bool automatic_capture,
    bool portal_explicitly_requested,
    bool physical_output_connected,
    bool higher_priority_available
  );

  /**
   * @brief Select a Gamescope EIS socket owned by the verified producer process.
   *
   * @param runtime_directory Trusted current-user runtime directory.
   * @param unix_socket_table Contents of `/proc/net/unix`.
   * @param producer_socket_inodes Socket inodes held by the verified Gamescope PID.
   * @return Contained `gamescope-*-ei` path, or no value when none is unique.
   */
  std::optional<std::filesystem::path> select_gamescope_eis_socket(
    const std::filesystem::path &runtime_directory,
    std::string_view unix_socket_table,
    std::span<const std::uint64_t> producer_socket_inodes
  );

  /**
   * @brief A validated nested Gamescope display request.
   */
  struct display_request_t {
    int width;  ///< Nested display width in pixels.
    int height;  ///< Nested display height in pixels.
    int fps;  ///< Nested display refresh rate.
  };

  /**
   * @brief Clamp a client display request to SteamOS virtual-session bounds.
   *
   * @param requested_width Client-provided width, or zero when unavailable.
   * @param requested_height Client-provided height, or zero when unavailable.
   * @param requested_fps Client-provided frame rate, or zero when unavailable.
   * @param default_width Configured width used for a missing request.
   * @param default_height Configured height used for a missing request.
   * @param default_fps Configured frame rate used for a missing request.
   * @return A request constrained to 640x480 through 7680x4320 at 30 through 240 FPS.
   */
  display_request_t normalize_display_request(int requested_width, int requested_height, int requested_fps, int default_width, int default_height, int default_fps);

  /**
   * @brief Build a Gamescope command from options advertised by its own help text.
   *
   * The result never uses an option absent from @p help_text. This keeps the
   * virtual-display provider compatible with the Gamescope version installed on
   * the SteamOS host.
   *
   * @param help_text Output captured from `gamescope --help`.
   * @param width Normalized nested display width.
   * @param height Normalized nested display height.
   * @param fps Normalized nested display refresh rate.
   * @param enable_hdr Whether the client requested HDR output.
   * @param gpu_device PCI vendor/device identifier accepted by Gamescope, if selected.
   * @param error Receives a reason when the advertised option set is insufficient.
   * @return Arguments after the executable, or an empty vector on failure.
   */
  std::vector<std::string> gamescope_arguments(const std::string &help_text, int width, int height, int fps, bool enable_hdr, const std::string &gpu_device, std::string &error);
}  // namespace steamos_virtual_session
